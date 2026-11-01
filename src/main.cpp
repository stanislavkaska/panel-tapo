// Panel světel: zapínání a vypínání pásků Tapo z krabičky
// Waveshare ESP32-S3-Touch-AMOLED-2.16 (hardware převzatý z Rozhodovače).
//
// Chod na baterii: krabička spí (hluboký spánek, displej i Wi-Fi vypnuté).
// Dotyk nebo tlačítko ji probudí, připojí se k Wi-Fi, zjistí stav světel
// a ukáže ho. Klepnutím na přepínač se světlo přepne, klepnutím na jméno se
// otevře detail se základními barvami a jasem. Po prodlevě bez dotyku zase
// zhasne a usne. Na USB jen zhasne a čeká (probuzení je okamžité).

#include <Arduino.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "pin_config.h"
#include "hw_waveshare.h"
#include "settings.h"
#include "net.h"
#include "tapo_klap.h"
#include "tapo_device.h"
#include "smooth_font.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Chybí include/secrets.h – zkopíruj include/secrets.example.h na include/secrets.h a vyplň údaje"
#endif

// ─── Světla ────────────────────────────────────────────────────────────────
#define MAX_DEVS 6
static const uint8_t DEV_COUNT =
  (sizeof(TAPO_DEVICES) / sizeof(TAPO_DEVICES[0]) < MAX_DEVS)
    ? (uint8_t)(sizeof(TAPO_DEVICES) / sizeof(TAPO_DEVICES[0]))
    : (uint8_t)MAX_DEVS;

struct DevView {
  TapoKlap  klap;
  TapoState st;
  char      label[32];
  bool      stale;     // stav je z minula, ještě neověřený
  bool      busy;      // právě se čte nebo přepíná
  char      err[32];
};
static DevView devs[MAX_DEVS];

// Relace KLAP a poslední známé stavy přežijí hluboký spánek v RTC paměti
static RTC_DATA_ATTR KlapSession g_sess[MAX_DEVS];

struct LastStates {
  uint32_t magic;
  uint8_t  known[MAX_DEVS];
  uint8_t  on[MAX_DEVS];
  uint8_t  isPlug[MAX_DEVS];
  uint8_t  bri[MAX_DEVS];
  uint8_t  color[MAX_DEVS];   // 1 = světlo hlásí barvu
  uint16_t hue[MAX_DEVS];
  uint8_t  sat[MAX_DEVS];
  uint8_t  schedOnEn[MAX_DEVS];
  uint16_t schedOnMin[MAX_DEVS];
  uint8_t  schedOffEn[MAX_DEVS];
  uint16_t schedOffMin[MAX_DEVS];
  char     label[MAX_DEVS][32];
};
static RTC_DATA_ATTR LastStates g_last;
static const uint32_t LAST_MAGIC = 0x50414E33;   // "PAN3"

// ─── Barvy (RGB565): černá a odstíny modré, oranžová jen pro chyby ─────────
// Stejné hodnoty mají tools/preview.py a docs/simulator.js, při změně upravit vše.
static const uint16_t COL_BG        = 0x0000;   // černá
static const uint16_t COL_CARD      = 0x0884;   // #0A1020 karta
static const uint16_t COL_CARD_HI   = 0x1108;   // #122040 karta pod prstem / vybraný řádek
static const uint16_t COL_BORDER    = 0x19CD;   // #1E3A6E rámeček
static const uint16_t COL_BLUE      = 0x3C5F;   // #3D8BFF akcent (zapnuto, aktivní prvek)
static const uint16_t COL_BLUE_LT   = 0x8E3F;   // #8FC6FF světle modrá (text ZAPNUTO, knoflík)
static const uint16_t COL_BLUE_DIM  = 0x4BD7;   // #4A79B8 ztlumená modrá (neověřený stav)
static const uint16_t COL_ON_TRACK  = 0x094B;   // #0C2A5A dráha zapnutého přepínače
static const uint16_t COL_ON_TRACK_D= 0x00C7;   // #071A3A totéž ztlumeně
static const uint16_t COL_OFF_KNOB  = 0x3A6D;   // #3A4C6E knoflík vypnutého přepínače
static const uint16_t COL_OFF_KNOB_D= 0x21A9;   // #27354F totéž ztlumeně
static const uint16_t COL_TEXT      = 0xFFFF;   // bílá
static const uint16_t COL_DIM       = 0x7C97;   // #7A93B8 vedlejší text, VYPNUTO
static const uint16_t COL_HOT       = 0xFB88;   // #FF7043 chyby, slabá baterie
static const uint16_t COL_RED_BG    = 0x3841;   // #3A0A0A štítek OFFLINE

// ─── Základní barvy pásku v detailu světla (odstín 0–359°, sytost 0–100 %) ─
// L900 nemá bílé diody ani teplotu bílé: bílá je sytost 0, teplá bílá jen
// nádech do oranžova. Stejný seznam mají tools/preview.py a docs/simulator.js.
struct ColorPreset { const char* name; uint16_t hue; uint8_t sat; };
static const ColorPreset COLOR_PRESETS[] = {
  { "bílá", 0, 0 }, { "teplá bílá", 30, 45 }, { "červená", 0, 100 }, { "oranžová", 25, 100 },
  { "žlutá", 50, 100 }, { "zelená", 120, 100 }, { "tyrkysová", 175, 100 }, { "modrá", 240, 100 },
  { "fialová", 275, 100 }, { "růžová", 330, 70 },
};
static const uint8_t COLOR_PRESET_N = sizeof(COLOR_PRESETS) / sizeof(COLOR_PRESETS[0]);

// Barva pásku na displeji (plný jas): HSV → RGB565 v celých číslech, aby
// preview.py a simulator.js počítaly stejné pixely
static uint16_t hsvTo565(uint16_t hue, uint8_t sat) {
  uint32_t h = hue % 360, s = sat > 100 ? 100 : sat, f = h % 60;
  uint32_t p = 255 * (100 - s) / 100;
  uint32_t q = 255 * (6000 - s * f) / 6000;
  uint32_t t = 255 * (6000 - s * (60 - f)) / 6000;
  uint32_t r, g, b;
  switch (h / 60) {
    case 0:  r = 255; g = t;   b = p;   break;
    case 1:  r = q;   g = 255; b = p;   break;
    case 2:  r = p;   g = 255; b = t;   break;
    case 3:  r = p;   g = q;   b = 255; break;
    case 4:  r = t;   g = p;   b = 255; break;
    default: r = 255; g = p;   b = q;   break;
  }
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t halfColor(uint16_t c) { return (c >> 1) & 0x7BEF; }   // poloviční jas RGB565

// Základní barva, které odpovídá barva pásku (±2), jinak -1
static int8_t presetIndex(uint16_t hue, uint8_t sat) {
  for (uint8_t k = 0; k < COLOR_PRESET_N; k++) {
    const ColorPreset& c = COLOR_PRESETS[k];
    if (abs((int)sat - (int)c.sat) > 2) continue;
    if (c.sat == 0) return (int8_t)k;   // bílá: na odstínu nezáleží
    int dh = abs((int)(hue % 360) - (int)c.hue);
    if (dh > 180) dh = 360 - dh;
    if (dh <= 2) return (int8_t)k;
  }
  return -1;
}

// ─── Rozvržení 480 × 480 (okraje kvůli zaoblenému rámečku) ─────────────────
static const int CARD_X = 24, CARD_W = 432;
static const int CARDS_Y0 = 48, CARDS_Y1 = 376, CARD_GAP = 10;
static const int STATUS_Y = 386, STATUS_H = 28;
static const int NAV_Y = 424, NAV_H = 44, NAV_W = 200;
static const int NAV1_X = 36, NAV2_X = 244;
static const int MODAL_X = 24, MODAL_W = 432, MODAL_Y = CARDS_Y0;
static const int MODAL_H = CARDS_Y1 - CARDS_Y0;   // končí jako karty, pod ním řádek stavu
static const int ROW_Y0 = 48;                     // první řádek od horního okraje modalu
static const int ROW_H = 38;

// Detail světla (barva a jas) ve stejné ploše jako nastavení
static const int DET_HEAD_H = 54;                 // záhlaví: jméno, stav a přepínač
static const int DET_SW_W = 84, DET_SW_H = 40;
static const int TILE_X0 = MODAL_X + 20, TILE_Y0 = MODAL_Y + 64;   // dlaždice barev 5 × 2
static const int TILE_W = 68, TILE_H = 54, TILE_GAP = 13, TILE_ROW_GAP = 10;
static const int BRI_Y = MODAL_Y + 188;           // hodnota jasu
static const int BAR_Y = MODAL_Y + 234, BAR_H = 10, BAR_W = 392;
static const int STEP_Y = MODAL_Y + 256, STEP_H = 56, STEP_W = 89, STEP_GAP = 12;
static const int8_t BRI_STEPS[4] = { -10, -1, 1, 10 };
static const char* const BRI_STEP_LBL[4] = { "–10", "–1", "+1", "+10" };
static const int16_t SCHED_STEPS[4] = { -60, -5, 5, 60 };
static const char* const SCHED_STEP_LBL[4] = { "–1 h", "–5 m", "+5 m", "+1 h" };
// Prvky detailu (zvýraznění pod prstem):
// světlo: 0–9 barvy, 10–13 jas, 14 přepínač;
// zásuvka: 14 přepínač, 20 přepínač zapnutí, 21–24 čas zapnutí, 25 přepínač vypnutí, 26–29 čas vypnutí
static const int8_t CTL_STEP0 = 10, CTL_SWITCH = 14;
static const int8_t CTL_ON_SW = 20, CTL_ON_STEP0 = 21, CTL_OFF_SW = 25, CTL_OFF_STEP0 = 26;
static const int PLUG_SW_W = 72, PLUG_SW_H = 36;
static const int PLUG_ON_Y = MODAL_Y + 62, PLUG_ON_BTN_Y = MODAL_Y + 106;
static const int PLUG_DIV_Y = MODAL_Y + 162;
static const int PLUG_OFF_Y = MODAL_Y + 172, PLUG_OFF_BTN_Y = MODAL_Y + 216;
static const int PLUG_BTN_H = 46;

// ─── Stav aplikace ─────────────────────────────────────────────────────────
enum AppState : uint8_t { ST_WIFI, ST_QUERY, ST_IDLE, ST_DOZE };
static AppState app = ST_WIFI;

static uint8_t  queryIdx = 0;
static bool     wifiFailed = false;
static int8_t   pendingToggle = -1;   // klepnutí během připojování se provede po načtení stavu
static uint8_t  sel = 0;              // světlo vybrané tlačítky
static bool     selVisible = false;
static bool     settingsOpen = false;
static uint8_t  settingsSel = 0;
static bool     dirty = true;
static uint32_t lastInteractMs = 0;
static uint32_t lastSecShown = 0xFFFFFFFF;
static uint32_t wifiAnimTick = 0;
static bool     onUsb = true;
static bool     touchArmed = false;   // prst, který krabičku probudil, se nepočítá
static uint32_t touchArmUntil = 0;
static bool     swallowA = false, swallowB = false;
static bool     aLong = false;
static int8_t   pressedCard = -1;     // karta pod prstem (zvýraznění)
static bool     pressedRight = false; // prst na pravé části karty (přepínač)
static int8_t   pressedNav = -1;
static bool     dimmed = false;       // displej ztlumený před zhasnutím
static bool     detailOpen = false;   // detail světla: barva a jas přes celou plochu
static uint8_t  detailIdx = 0;
static int8_t   pressedCtl = -1;      // prvek detailu pod prstem

// Úprava z detailu, která čeká na odeslání. Klepnutí během čekání na pásek/zásuvku ji
// jen přepíšou, takže zařízení dostane vždy poslední hodnotu.
struct LightEdit {
  int8_t   dev = -1;       // světlo/zásuvka, -1 = nic nečeká
  uint8_t  bri = 0;        // jas 1..100, 0 = neměnit
  int16_t  hue = -1;       // odstín, -1 = barvu neměnit
  uint8_t  sat = 0;
  bool     schedOn = false;
  bool     schedOnEn = false;
  uint16_t schedOnMin = 420;
  bool     schedOff = false;
  bool     schedOffEn = false;
  uint16_t schedOffMin = 1320;
};
static LightEdit edit;

// ─── Ukázka (DEMO) ─────────────────────────────────────────────────────────
// Překryv nad skutečnou obrazovkou jako Nastavení. Karty kreslí stejný kód
// jako skutečná světla, jen z pevných dat. Připojování a načítání stavu běží
// dál na pozadí, ovládání světel je během ukázky vypnuté.
struct DemoLight {
  const char* label;
  int8_t      on;       // 1 zapnuto, 0 vypnuto, -1 neznámý stav
  uint8_t     bri;
  bool        dim;      // stav z minula, ještě neověřený
  const char* err;
  uint16_t    hue;      // barva pásku (bílá = sytost 0)
  uint8_t     sat;
  bool        plug = false;       // zásuvka P100 (bez jasu a barvy)
  uint32_t    onTime = 0;         // doba provozu zásuvky v sekundách
  uint16_t    onMin = 420;        // časovač zapnutí: minuta dne
  bool        onEn = false;
  uint16_t    offMin = 1320;      // časovač vypnutí: minuta dne
  bool        offEn = false;
};
enum : uint8_t { TOP_CONNECTED = 0, TOP_CONNECTING, TOP_OFFLINE };
struct DemoStep {
  const char* name;
  uint8_t     count;
  DemoLight   lights[MAX_DEVS];
  int8_t      selected;   // světlo vybrané tlačítky, -1 = žádné
  uint8_t     top;        // štítek Wi-Fi v horní liště
  int8_t      battery;    // procenta baterie, -1 = skutečný stav
  bool        detail;     // detail prvního světla místo karet
};
// Barvy (odstín, sytost): teplá bílá 30/45, bílá 0/0, modrá 240/100,
// fialová 275/100, tyrkysová 175/100, zelená 120/100
static const DemoStep DEMO_STEPS[] = {
  { "1 světlo zapnuté", 1, { { "LED pásek", 1, 100, false, "", 240, 100 } }, -1, TOP_CONNECTED, -1, false },
  { "1 světlo vypnuté", 1, { { "LED pásek", 0, 0, false, "", 240, 100 } }, -1, TOP_CONNECTED, -1, false },
  { "2 světla", 2, { { "Obývák", 1, 80, false, "", 30, 45 }, { "Kuchyň", 0, 0, false, "", 0, 0 } },
    -1, TOP_CONNECTED, -1, false },
  { "3 světla", 3, { { "Obývák", 1, 70, false, "", 30, 45 }, { "Kuchyň", 0, 0, false, "", 0, 0 },
                     { "Chodba", 1, 100, false, "", 0, 0 } }, -1, TOP_CONNECTED, -1, false },
  { "4 světla, výběr tlačítky", 4, { { "Obývák", 1, 80, false, "", 30, 45 }, { "Kuchyň", 0, 0, false, "", 0, 0 },
                                     { "Ložnice", 0, 0, false, "", 275, 100 }, { "Pracovna", 1, 45, false, "", 0, 0 } },
    2, TOP_CONNECTED, -1, false },
  { "6 světel", 6, { { "Obývák", 1, 80, false, "", 30, 45 }, { "Kuchyň", 1, 60, false, "", 0, 0 },
                     { "Ložnice", 0, 0, false, "", 275, 100 }, { "Pracovna", 1, 100, false, "", 0, 0 },
                     { "Koupelna", 0, 0, false, "", 175, 100 }, { "Terasa", 0, 0, false, "", 120, 100 } },
    -1, TOP_CONNECTED, -1, false },
  { "Barva a jas", 1, { { "Obývák", 1, 60, false, "", 30, 45 } }, -1, TOP_CONNECTED, -1, true },
  { "Světlo a zásuvky P100", 3, { { "Obývák", 1, 80, false, "", 30, 45 },
                                   { "LED dvůr", 1, 0, false, "", 0, 0, true, 18239, 420, false, 1320, true },
                                   { "3D tiskárna", 0, 0, false, "", 0, 0, true, 0, 420, true, 1320, true } },
    -1, TOP_CONNECTED, -1, false },
  { "Zásuvka: časovač zap/vyp", 1, { { "LED dvůr", 1, 0, false, "", 0, 0, true, 18239, 1080, true, 1320, true } },
    -1, TOP_CONNECTED, -1, true },
  { "Po probuzení: stav z minula", 2, { { "Obývák", 1, 80, true, "", 30, 45 }, { "Kuchyň", 0, 0, true, "", 0, 0 } },
    -1, TOP_CONNECTING, -1, false },
  { "Světlo mimo síť", 2, { { "Obývák", 1, 80, false, "", 30, 45 }, { "Terasa", 0, 0, true, "není v síti", 120, 100 } },
    -1, TOP_CONNECTED, -1, false },
  { "Wi-Fi nedostupná, slabá baterie", 2, { { "Obývák", 1, 80, true, "", 30, 45 }, { "Kuchyň", 0, 0, true, "", 0, 0 } },
    -1, TOP_OFFLINE, 14, false },
};
static const uint8_t  DEMO_STEP_COUNT = sizeof(DEMO_STEPS) / sizeof(DEMO_STEPS[0]);
static const uint32_t DEMO_STEP_MS = 3500;
static bool     demoOn = false;
static uint8_t  demoStep = 0;
static uint32_t demoLastMs = 0;

// ─── Zvuky ─────────────────────────────────────────────────────────────────
struct Note { uint16_t f; uint16_t ms; };   // f = 0 je pauza
static const Note SND_ON[]   = { {1500, 45}, {2000, 70} };
static const Note SND_OFF[]  = { {2000, 45}, {1500, 70} };
static const Note SND_ERR[]  = { {500, 110}, {0, 40}, {500, 110} };
static const Note SND_BOOT[] = { {1047, 60}, {1568, 90} };

static void play(const Note* n, uint8_t cnt) {
  if (!settings().sound) return;
  for (uint8_t i = 0; i < cnt; i++) hwBeep(n[i].f, n[i].ms);
}
#define PLAY(m) play(m, sizeof(m) / sizeof(m[0]))

static void click() {
  if (settings().sound) hwBeep(2200, 25, 3500);
}

// ─── Pomocné ───────────────────────────────────────────────────────────────
// Před zhasnutím se displej na posledních pár sekund ztlumí na čtvrtinu:
// AMOLED bere proud podle jasu a ztlumení zároveň ohlásí, že krabička usíná.
static const uint32_t DIM_BEFORE_MS = 5000;

static void applyBrightness() {
  static const uint8_t LEVELS[5] = { 30, 80, 140, 200, 255 };
  uint8_t v = LEVELS[settings().brightness % 5];
  if (dimmed) v = (v / 4 < 8) ? 8 : v / 4;
  hwSetBrightness(v);
}

// Komunikace s páskem: rádio na tu chvíli bez úsporného režimu, aby odpověď
// nečekala na maják přístupového bodu; v klidu (ST_IDLE) zase úsporně.
struct NetBurst {
  NetBurst()  { netSetPowerSave(false); }
  ~NetBurst();
};

static bool hitRect(int tx, int ty, int rx, int ry, int rw, int rh) {
  return tx >= rx && tx < rx + rw && ty >= ry && ty < ry + rh;
}

// Obdélník i-té karty při n kartách na obrazovce
static void cardRectN(uint8_t i, uint8_t n, int& x, int& y, int& w, int& h) {
  if (n == 0) n = 1;
  h = ((CARDS_Y1 - CARDS_Y0) - CARD_GAP * (n - 1)) / n;
  x = CARD_X;
  w = CARD_W;
  y = CARDS_Y0 + i * (h + CARD_GAP);
}

static void cardRect(uint8_t i, int& x, int& y, int& w, int& h) {
  cardRectN(i, DEV_COUNT, x, y, w, h);
}

// Zkopíruje text do out; když je širší než maxW px, zkrátí ho na hranici
// znaku UTF-8 a přidá „…“
static void fitText(const SmoothFont& f, const char* src, int maxW, char* out, size_t cap) {
  if (!src) src = "";
  size_t n = strlen(src);
  if (n > cap - 1) {
    n = cap - 1;
    while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
  }
  memcpy(out, src, n);
  out[n] = 0;
  if (smoothTextWidth(f, out) <= maxW) return;
  const char* ELL = "…";
  int ellW = smoothTextWidth(f, ELL);
  while (n > 0) {
    do { n--; } while (n > 0 && ((uint8_t)out[n] & 0xC0) == 0x80);
    out[n] = 0;
    if (smoothTextWidth(f, out) + ellW <= maxW) break;
  }
  while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
  if (n + strlen(ELL) < cap) strcat(out, ELL);
}

static void setLabel(uint8_t i) {
  DevView& d = devs[i];
  const char* cfg = TAPO_DEVICES[i].name;
  if (cfg && cfg[0]) {
    strncpy(d.label, cfg, sizeof(d.label) - 1);
  } else if (d.st.nickname[0]) {
    strncpy(d.label, d.st.nickname, sizeof(d.label) - 1);
  } else if (d.label[0] == 0) {
    snprintf(d.label, sizeof(d.label), "Světlo %u", i + 1);
  }
  d.label[sizeof(d.label) - 1] = 0;
}

static void rememberLast(uint8_t i) {
  DevView& d = devs[i];
  g_last.magic = LAST_MAGIC;
  g_last.known[i] = 1;
  g_last.on[i] = d.st.on ? 1 : 0;
  g_last.isPlug[i] = d.st.isPlug ? 1 : 0;
  g_last.bri[i] = d.st.brightness;
  g_last.color[i] = d.st.hasColor ? 1 : 0;
  g_last.hue[i] = d.st.hue;
  g_last.sat[i] = d.st.sat;
  g_last.schedOnEn[i] = d.st.schedOnEn ? 1 : 0;
  g_last.schedOnMin[i] = d.st.schedOnMin;
  g_last.schedOffEn[i] = d.st.schedOffEn ? 1 : 0;
  g_last.schedOffMin[i] = d.st.schedOffMin;
  strncpy(g_last.label[i], d.label, sizeof(g_last.label[i]) - 1);
  g_last.label[i][sizeof(g_last.label[i]) - 1] = 0;
}

static uint32_t idleLimitMs() {
  if (demoOn) return 300000UL;   // ukázka běží bez dotyku nejvýš 5 minut
  uint32_t s = settings().sleepSec;
  if (onUsb) {
    uint32_t v = s * 3000UL;
    return v < 60000UL ? 60000UL : v;
  }
  return s * 1000UL;
}

NetBurst::~NetBurst() {
  if (app == ST_IDLE) netSetPowerSave(true);
}

static uint32_t remainingSec() {
  uint32_t limit = idleLimitMs();
  uint32_t idle = millis() - lastInteractMs;
  if (idle >= limit) return 0;
  return (limit - idle + 999) / 1000;
}

// Datum a čas pro horní lištu („30.10.2026“, „23:34“); false = čas ještě není nastavený
static const time_t CLOCK_MIN_VALID = 1735689600;   // 1. 1. 2025
static bool clockParts(char* date, size_t dcap, char* hm, size_t hcap) {
  time_t t = time(nullptr);
  if (t < CLOCK_MIN_VALID) return false;
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(date, dcap, "%d.%d.%d", lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900);
  snprintf(hm, hcap, "%d:%02d", lt.tm_hour, lt.tm_min);
  return true;
}

// ─── Kreslení ──────────────────────────────────────────────────────────────
// Svislé polohy textu: y je horní hrana řádku písma, velká písmena začínají
// o kus níž (fontBadge16, fontSmall16 a fontSub18 o 7 px, fontButton22 a fontMenu19
// o 8 px, fontTitle24 o 9 px, fontTitle28 o 11 px, fontState32 o 12 px). Proto
// posuny níže nejsou kulaté.
static void drawSwitch(Arduino_Canvas* cv, int x, int y, int w, int h, bool on, bool dim, bool pressed) {
  uint16_t track, border, knob;
  if (on) {
    track  = dim ? COL_ON_TRACK_D : COL_ON_TRACK;
    border = dim ? COL_BORDER : COL_BLUE;
    knob   = dim ? COL_BLUE_DIM : COL_BLUE_LT;
  } else {
    track  = COL_CARD_HI;
    border = dim ? COL_CARD_HI : COL_BORDER;
    knob   = dim ? COL_OFF_KNOB_D : COL_OFF_KNOB;
  }
  if (pressed) track = on ? 0x1210 : 0x198A;
  int r = h / 2;
  cv->fillRoundRect(x, y, w, h, r, track);
  cv->drawRoundRect(x, y, w, h, r, border);
  int kd = h - 8;
  int kx = on ? (x + w - 4 - kd / 2) : (x + 4 + kd / 2);
  cv->fillCircle(kx, y + h / 2, kd / 2, knob);
  if (on && !dim) cv->fillCircle(kx, y + h / 2, kd / 4, 0xDF7F);   // světlý střed knoflíku
}

// Datum šedomodře, čas bíle mezi left a right. Na střed displeje, když se tam
// vejde, jinak na střed volného místa; když se nevejde celé, jen čas.
static void drawClock(Arduino_Canvas* cv, const char* date, const char* hm, int left, int right) {
  int wd = smoothTextWidth(fontBadge16, date);
  int ws = smoothTextWidth(fontBadge16, " ");
  int wt = smoothTextWidth(fontBadge16, hm);
  bool withDate = (wd + ws + wt <= right - left);
  int total = withDate ? wd + ws + wt : wt;
  if (total > right - left) return;
  int x = 240 - total / 2;
  if (x < left || x + total > right) x = left + (right - left - total) / 2;
  if (withDate) {
    smoothDrawText(cv, fontBadge16, date, x, 10, COL_DIM);
    x += wd + ws;
  }
  smoothDrawText(cv, fontBadge16, hm, x, 10, COL_TEXT);
}

static void drawTopBar() {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillRect(0, 0, LCD_WIDTH, 50, COL_BG);

  // Štítek Wi-Fi vlevo
  uint8_t mode;
  if (demoOn)                          mode = DEMO_STEPS[demoStep].top;
  else if (app == ST_WIFI)             mode = TOP_CONNECTING;
  else if (netState() != NET_CONNECTED) mode = TOP_OFFLINE;
  else                                 mode = TOP_CONNECTED;

  char txt[40];
  uint16_t pill, fg;
  if (mode == TOP_CONNECTING) {
    static const char* const DOTS[4] = { "Wi-Fi", "Wi-Fi·", "Wi-Fi··", "Wi-Fi···" };
    snprintf(txt, sizeof(txt), "%s", DOTS[(millis() / 300) % 4]);
    pill = COL_CARD_HI; fg = COL_BLUE_DIM;
  } else if (mode == TOP_OFFLINE) {
    snprintf(txt, sizeof(txt), "OFFLINE");
    pill = COL_RED_BG; fg = COL_HOT;
  } else {
    fitText(fontBadge16, netSsid()[0] ? netSsid() : "Wi-Fi", 200, txt, sizeof(txt));
    pill = COL_ON_TRACK; fg = COL_BLUE_LT;
  }
  int pillW = smoothTextWidth(fontBadge16, txt) + 24;
  cv->fillRoundRect(40, 10, pillW, 28, 8, pill);
  cv->drawRoundRect(40, 10, pillW, 28, 8, fg);
  smoothDrawTextCentered(cv, fontBadge16, txt, 40 + pillW / 2, 11, fg);

  // Baterie vpravo
  const HwPowerInfo& pw = hwPower();
  char bb[12];
  uint16_t batCol;
  int pct;
  if (demoOn && DEMO_STEPS[demoStep].battery >= 0) {
    pct = DEMO_STEPS[demoStep].battery;
    snprintf(bb, sizeof(bb), "%d%%", pct);
    batCol = (pct <= 20) ? COL_HOT : COL_DIM;
  } else if (pw.batteryConnected) {
    pct = pw.batteryPct;
    snprintf(bb, sizeof(bb), "%d%%", pct);
    batCol = pw.charging ? COL_BLUE : (pct <= 20 ? COL_HOT : COL_DIM);
  } else {
    pct = 100;
    snprintf(bb, sizeof(bb), "USB");
    batCol = COL_DIM;
  }
  smoothDrawTextRight(cv, fontBadge16, bb, 398, 10, batCol);
  cv->drawRoundRect(408, 14, 26, 18, 3, batCol);
  cv->fillRect(434, 19, 3, 8, batCol);
  int fillW = pct * 20 / 100;
  if (fillW < 2) fillW = 2;
  cv->fillRect(411, 17, fillW, 12, batCol);

  // Datum a čas mezi štítkem Wi-Fi a baterií
  char date[16], hm[8];
  if (clockParts(date, sizeof(date), hm, sizeof(hm))) {
    drawClock(cv, date, hm, 40 + pillW + 12, 398 - smoothTextWidth(fontBadge16, bb) - 12);
  }
}

// Co ukazuje jedna karta (skutečné světlo/zásuvka i ukázka v demu)
struct CardView {
  const char* label;
  bool        known;      // stav je známý (aspoň z minula)
  bool        on;
  bool        dim;        // neověřený stav nebo právě probíhá dotaz
  bool        isPlug = false;
  uint8_t     bri;        // jas v %, 0 = neukazovat
  uint32_t    onTime = 0; // doba běhu v s (zásuvka)
  bool        schedOnEn = false;
  uint16_t    schedOnMin = 420;
  bool        schedOffEn = false;
  uint16_t    schedOffMin = 1320;
  const char* err;        // chyba, "" = bez chyby
  bool        selected;   // vybraná tlačítky (modrý rámeček)
  bool        pressed;    // prst na levé části (otevře detail)
  bool        pressedSw;  // prst na pravé části (přepne)
  bool        hasColor;   // barva pásku je známá
  uint16_t    hue;
  uint8_t     sat;
};

// Šířka „ZAP“ / „VYP“ (širší z obou), podle ní se zarovnává stav vlevo od přepínače
static int shortStateW(const SmoothFont& f) {
  int a = smoothTextWidth(f, "ZAP"), b = smoothTextWidth(f, "VYP");
  return a > b ? a : b;
}

// Hranice mezi levou částí karty (jméno a jas, klepnutí otevře detail) a pravou
// (stav a přepínač, klepnutí přepne). Odpovídá polohám v drawCardView().
static int cardSplitX(int h) {
  const int R = CARD_X + CARD_W;
  if (h >= 140) return R - 22 - 140 - 24;
  if (h >= 85)  return R - 18 - 88 - 16 - shortStateW(fontButton22) - 14;
  if (h >= 65)  return R - 16 - 76 - 14 - shortStateW(fontButton22) - 12;
  return R - 12 - 74 - 12;
}

// Část karty, která světlo přepíná; u jediného světla jen okolí velkého přepínače
static void cardToggleRect(uint8_t i, uint8_t n, int& x, int& y, int& w, int& h) {
  int cx, cy, cw, ch;
  cardRectN(i, n, cx, cy, cw, ch);
  if (n <= 1) {
    x = 240 - 100 - 16;
    y = cy + ch / 2 - 60 - 16;
    w = 200 + 32;
    h = 90 + 32;
    return;
  }
  x = cardSplitX(ch);
  y = cy;
  w = cx + cw - x;
  h = ch;
}

// Tečka před jménem: svítí = plná v barvě pásku, zhasnuto = kroužek, neznámo = šedý kroužek
static void drawColorDot(Arduino_Canvas* cv, int cx, int cy, int r, const CardView& v, uint16_t bg) {
  uint16_t c = v.hasColor ? hsvTo565(v.hue, v.sat) : COL_TEXT;
  if (!v.known) {
    cv->fillCircle(cx, cy, r, COL_BORDER);
    cv->fillCircle(cx, cy, r - 2, bg);
  } else if (v.on) {
    cv->fillCircle(cx, cy, r, v.dim ? halfColor(c) : c);
  } else {
    cv->fillCircle(cx, cy, r, halfColor(c));
    cv->fillCircle(cx, cy, r - 2, bg);
  }
}

static void drawCardView(uint8_t i, uint8_t n, const CardView& v) {
  Arduino_Canvas* cv = hwCanvas();
  int x, y, w, h;
  cardRectN(i, n, x, y, w, h);

  bool on = v.known && v.on;
  bool hasErr = v.err && v.err[0];
  uint16_t bg = v.pressed ? COL_CARD_HI : COL_CARD;
  uint16_t border = hasErr ? COL_HOT : ((on && !v.dim) ? COL_BLUE : COL_BORDER);
  int rad = (h >= 60) ? 14 : 12;
  cv->fillRoundRect(x, y, w, h, rad, bg);
  cv->drawRoundRect(x, y, w, h, rad, border);
  if (v.selected && n > 1) {
    cv->drawRoundRect(x + 1, y + 1, w - 2, h - 2, rad - 1, COL_BLUE_LT);
    cv->drawRoundRect(x + 2, y + 2, w - 4, h - 4, rad - 2, COL_BLUE_LT);
  }

  uint16_t nameCol  = v.dim ? COL_DIM : COL_TEXT;
  uint16_t stateCol = !v.known ? COL_DIM : (on ? (v.dim ? COL_BLUE_DIM : COL_BLUE_LT) : (v.dim ? COL_OFF_KNOB : COL_DIM));
  const char* stateTxt = !v.known ? "…" : (on ? "ZAPNUTO" : "VYPNUTO");

  // Doplňkový řádek: chyba má přednost před jasem / dobou běhu a časovačem
  char sub[48] = "";
  uint16_t subCol = COL_DIM;
  if (hasErr) {
    snprintf(sub, sizeof(sub), "%s", v.err);
    subCol = COL_HOT;
  } else if (v.isPlug) {
    bool compact = (h < 65);
    char run[24] = "";
    if (on && v.onTime > 0) {
      uint32_t rh = v.onTime / 3600;
      uint32_t rm = (v.onTime % 3600) / 60;
      if (rh > 0) snprintf(run, sizeof(run), "%s%lu h %lu m", compact && v.schedOffEn ? "" : "běží ", (unsigned long)rh, (unsigned long)rm);
      else if (rm > 0) snprintf(run, sizeof(run), "%s%lu min", compact && v.schedOffEn ? "" : "běží ", (unsigned long)rm);
      else snprintf(run, sizeof(run), "%s< 1 min", compact && v.schedOffEn ? "" : "běží ");
    }
    if (on && v.schedOffEn) {
      uint16_t om = v.schedOffMin % 1440;
      if (run[0]) snprintf(sub, sizeof(sub), compact ? "%s · %02u:%02u" : "%s · vyp %02u:%02u", run, om / 60, om % 60);
      else snprintf(sub, sizeof(sub), "vyp v %02u:%02u", om / 60, om % 60);
    } else if (run[0]) {
      snprintf(sub, sizeof(sub), "%s", run);
    } else if (!on && v.schedOnEn && v.schedOffEn) {
      uint16_t im = v.schedOnMin % 1440, om = v.schedOffMin % 1440;
      snprintf(sub, sizeof(sub), compact ? "%02u:%02u–%02u:%02u" : "zap %02u:%02u · vyp %02u:%02u", im / 60, im % 60, om / 60, om % 60);
    } else if (!on && v.schedOnEn) {
      uint16_t im = v.schedOnMin % 1440;
      snprintf(sub, sizeof(sub), "zap v %02u:%02u", im / 60, im % 60);
    } else if (!on && v.schedOffEn) {
      uint16_t om = v.schedOffMin % 1440;
      snprintf(sub, sizeof(sub), "vyp v %02u:%02u", om / 60, om % 60);
    }
  } else if (on && v.bri) {
    snprintf(sub, sizeof(sub), "jas %u %%", v.bri);
  }
  char name[48];
  char s2[40];

  if (n == 1) {
    // Jedno světlo: tečka a jméno nahoře, velký přepínač uprostřed, stav a jas pod ním
    fitText(fontTitle28, v.label, w - 48 - 26, name, sizeof(name));
    int nx = 240 - (smoothTextWidth(fontTitle28, name) + 26) / 2;
    drawColorDot(cv, nx + 8, y + 57, 8, v, bg);
    smoothDrawText(cv, fontTitle28, name, nx + 26, y + 36, nameCol);
    int sw = 200, sh = 90;
    drawSwitch(cv, 240 - sw / 2, y + h / 2 - 60, sw, sh, on, v.dim, v.pressedSw);
    smoothDrawTextCentered(cv, fontState32, stateTxt, 240, y + h / 2 + 52, stateCol);
    if (sub[0]) smoothDrawTextCentered(cv, fontSub18, sub, 240, y + h / 2 + 100, subCol);
  } else if (h >= 140) {
    // Dvě světla: tečka, jméno, stav a jas vlevo, přepínač vpravo za čarou
    int sw = 140, sh = 64;
    int sx = x + w - 22 - sw;
    int split = cardSplitX(h);
    int nx = x + 48;
    int textW = split - 12 - nx;
    fitText(fontTitle28, v.label, textW, name, sizeof(name));
    drawColorDot(cv, x + 30, y + 37, 8, v, bg);
    smoothDrawText(cv, fontTitle28, name, nx, y + 16, nameCol);
    smoothDrawText(cv, fontState32, stateTxt, nx, y + 64, stateCol);
    if (sub[0]) {
      fitText(fontSub18, sub, textW, s2, sizeof(s2));
      smoothDrawText(cv, fontSub18, s2, nx, y + 118, subCol);
    }
    cv->drawFastVLine(split, y + 14, h - 28, COL_BORDER);
    drawSwitch(cv, sx, y + h / 2 - sh / 2, sw, sh, on, v.dim, v.pressedSw);
  } else if (h >= 85) {
    // 3 světla (výška karty ~102 px): velký výrazný řádek, 28px tučný název
    int sw = 88, sh = 42;
    int sx = x + w - 18 - sw;
    drawSwitch(cv, sx, y + (h - sh) / 2, sw, sh, on, v.dim, v.pressedSw);
    int stateR = sx - 16;
    int tyState = y + h / 2 - 16;
    smoothDrawTextRight(cv, fontButton22, !v.known ? "…" : (on ? "ZAP" : "VYP"), stateR, tyState, stateCol);
    int split = cardSplitX(h);
    cv->drawFastVLine(split, y + 12, h - 24, COL_BORDER);
    int nx = x + 44;
    int textW = split - 12 - nx;
    fitText(fontTitle28, v.label, textW, name, sizeof(name));
    if (sub[0]) {
      // Dva řádky: název nahoře, jas/chyba pod ním s čistou mezerou (nelezou do sebe)
      int ty1 = y + (h >= 100 ? 16 : 13);
      int ty2 = y + (h >= 100 ? 51 : 44);
      drawColorDot(cv, x + 27, ty1 + 21, 7, v, bg);
      smoothDrawText(cv, fontTitle28, name, nx, ty1, nameCol);
      fitText(fontSub18, sub, textW, s2, sizeof(s2));
      smoothDrawText(cv, fontSub18, s2, nx, ty2, subCol);
    } else {
      // Jeden řádek (světlo vypnuto): přesně na svislý střed karty
      int ty = y + h / 2 - 22;
      drawColorDot(cv, x + 27, ty + 21, 7, v, bg);
      smoothDrawText(cv, fontTitle28, name, nx, ty, nameCol);
    }
  } else if (h >= 65) {
    // 4 světla (výška karty ~74 px)
    int sw = 76, sh = 34;
    int sx = x + w - 16 - sw;
    drawSwitch(cv, sx, y + (h - sh) / 2, sw, sh, on, v.dim, v.pressedSw);
    int stateR = sx - 14;
    int tyState = y + h / 2 - 14;
    smoothDrawTextRight(cv, fontButton22, !v.known ? "…" : (on ? "ZAP" : "VYP"), stateR, tyState, stateCol);
    int split = cardSplitX(h);
    cv->drawFastVLine(split, y + 10, h - 20, COL_BORDER);
    int nx = x + 38;
    int textW = split - 10 - nx;
    fitText(fontTitle24, v.label, textW, name, sizeof(name));
    if (sub[0]) {
      int ty1 = y + 8;
      int ty2 = y + 36;
      drawColorDot(cv, x + 24, ty1 + 18, 6, v, bg);
      smoothDrawText(cv, fontTitle24, name, nx, ty1, nameCol);
      fitText(fontSub18, sub, textW, s2, sizeof(s2));
      smoothDrawText(cv, fontSub18, s2, nx, ty2, subCol);
    } else {
      int ty = y + h / 2 - 19;
      drawColorDot(cv, x + 24, ty + 18, 6, v, bg);
      smoothDrawText(cv, fontTitle24, name, nx, ty, nameCol);
    }
  } else {
    // 5-6 světel: bez textu ZAP/VYP (stačí grafický přepínač), velké jméno 28 px a popis 22 px vedle sebe
    int sw = 74, sh = (h >= 52) ? 34 : 30;
    int sx = x + w - 12 - sw;
    drawSwitch(cv, sx, y + (h - sh) / 2, sw, sh, on, v.dim, v.pressedSw);
    int split = cardSplitX(h);
    cv->drawFastVLine(split, y + 8, h - 16, COL_BORDER);
    int nx = x + 34;
    int textW = split - 8 - nx;
    int tyName = y + h / 2 - 21;
    drawColorDot(cv, x + 20, y + h / 2, (h >= 52) ? 7 : 6, v, bg);
    if (sub[0]) {
      int nameFullW = smoothTextWidth(fontTitle28, v.label);
      bool fits22 = (nameFullW + 8 + smoothTextWidth(fontButton22, sub) <= textW);
      const SmoothFont& sf = fits22 ? fontButton22 : fontMenu19;
      int tySub = fits22 ? (tyName + 6) : (tyName + 8);
      int subW = smoothTextWidth(sf, sub);
      int maxSubW = textW * 46 / 100;
      int nameMaxW = textW - ((subW < maxSubW ? subW : maxSubW) + 8);
      fitText(fontTitle28, v.label, nameMaxW, name, sizeof(name));
      smoothDrawText(cv, fontTitle28, name, nx, tyName, nameCol);
      int remW = textW - smoothTextWidth(fontTitle28, name) - 8;
      fitText(sf, sub, remW, s2, sizeof(s2));
      smoothDrawTextRight(cv, sf, s2, split - 8, tySub, subCol);
    } else {
      fitText(fontTitle28, v.label, textW, name, sizeof(name));
      smoothDrawText(cv, fontTitle28, name, nx, tyName, nameCol);
    }
  }
}

static void drawCard(uint8_t i) {
  const DevView& d = devs[i];
  bool pending = (edit.dev == (int8_t)i);
  CardView v;
  v.label       = d.label;
  v.known       = d.st.valid;
  v.on          = d.st.on;
  v.dim         = d.stale || d.busy || !d.st.valid;
  v.isPlug      = d.st.isPlug;
  v.bri         = d.st.brightness;
  v.onTime      = d.st.onTime;
  v.schedOnEn   = (pending && edit.schedOn) ? edit.schedOnEn : d.st.schedOnEn;
  v.schedOnMin  = (pending && edit.schedOn) ? edit.schedOnMin : d.st.schedOnMin;
  v.schedOffEn  = (pending && edit.schedOff) ? edit.schedOffEn : d.st.schedOffEn;
  v.schedOffMin = (pending && edit.schedOff) ? edit.schedOffMin : d.st.schedOffMin;
  v.err         = d.err;
  v.selected    = selVisible && sel == i;
  v.pressed     = (pressedCard == (int8_t)i) && !pressedRight;
  v.pressedSw   = (pressedCard == (int8_t)i) && pressedRight;
  v.hasColor    = d.st.hasColor;
  v.hue         = d.st.hue;
  v.sat         = d.st.sat;
  drawCardView(i, DEV_COUNT, v);
}

static void drawCards() {
  if (DEV_COUNT == 0) {
    Arduino_Canvas* cv = hwCanvas();
    smoothDrawTextCentered(cv, fontTitle24, "Žádné světlo", 240, 200, COL_HOT);
    smoothDrawTextCentered(cv, fontSmall16, "doplň TAPO_DEVICES v include/secrets.h", 240, 240, COL_DIM);
    return;
  }
  for (uint8_t i = 0; i < DEV_COUNT; i++) drawCard(i);
}

static void drawDemoCards() {
  const DemoStep& s = DEMO_STEPS[demoStep];
  for (uint8_t i = 0; i < s.count; i++) {
    const DemoLight& l = s.lights[i];
    CardView v;
    v.label     = l.label;
    v.known     = l.on >= 0;
    v.on        = l.on > 0;
    v.dim       = l.dim || l.on < 0;
    v.bri       = l.bri;
    v.err       = l.err;
    v.selected  = (s.selected == (int8_t)i);
    v.pressed   = false;
    v.pressedSw = false;
    v.hasColor  = !l.plug;
    v.hue       = l.hue;
    v.sat       = l.sat;
    v.isPlug      = l.plug;
    v.onTime      = l.onTime;
    v.schedOnEn   = l.onEn;
    v.schedOnMin  = l.onMin;
    v.schedOffEn  = l.offEn;
    v.schedOffMin = l.offMin;
    drawCardView(i, s.count, v);
  }
}

// ─── Detail světla (barvy a jas) nebo chytré zásuvky (časovač zap/vyp) ──────
struct DetailView {
  const char* label;
  bool        known;      // stav je známý (aspoň z minula)
  bool        on;
  bool        dim;        // neověřený stav nebo právě probíhá dotaz
  bool        isPlug = false;
  uint8_t     bri;        // jas v %, 0 = neznámý
  bool        hasColor;   // barva je známá
  bool        colorless;  // světlo hlásí stav, ale barvu ne (bílá žárovka)
  uint16_t    hue;
  uint8_t     sat;
  uint32_t    onTime = 0;
  bool        schedOnEn = false;
  uint16_t    schedOnMin = 420;
  bool        schedOffEn = false;
  uint16_t    schedOffMin = 1320;
  int8_t      pressed;    // prvek pod prstem, -1 = žádný
};

static void tileRect(uint8_t k, int& x, int& y) {
  x = TILE_X0 + (k % 5) * (TILE_W + TILE_GAP);
  y = TILE_Y0 + (k / 5) * (TILE_H + TILE_ROW_GAP);
}

// Levý okraj části záhlaví, která přepíná (stav + přepínač)
static int detailSplitX() {
  return MODAL_X + MODAL_W - 20 - DET_SW_W - 14 - shortStateW(fontButton22) - 14;
}

static void drawPlugScheduleBlock(Arduino_Canvas* cv, const char* title, bool en, uint16_t minOfDay,
                                  int rowY, int btnY, int8_t swCtl, int8_t step0Ctl, int8_t pressed, bool dim) {
  int swX = MODAL_X + MODAL_W - 20 - PLUG_SW_W;
  drawSwitch(cv, swX, rowY, PLUG_SW_W, PLUG_SW_H, en, dim, pressed == swCtl);
  uint16_t m = minOfDay % 1440;
  char hm[12];
  snprintf(hm, sizeof(hm), "%02u:%02u", m / 60, m % 60);
  uint16_t timeCol = en ? (dim ? COL_BLUE_DIM : COL_BLUE_LT) : COL_DIM;
  smoothDrawTextRight(cv, fontTitle28, hm, swX - 16, rowY - 2, timeCol);
  smoothDrawText(cv, fontMenu19, title, MODAL_X + 20, rowY + 5, en ? COL_TEXT : COL_DIM);

  for (uint8_t b = 0; b < 4; b++) {
    int bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP);
    bool pr = (pressed == step0Ctl + b);
    cv->fillRoundRect(bx, btnY, STEP_W, PLUG_BTN_H, 12, pr ? COL_BORDER : COL_CARD_HI);
    cv->drawRoundRect(bx, btnY, STEP_W, PLUG_BTN_H, 12, pr ? COL_BLUE : COL_BORDER);
    smoothDrawTextCentered(cv, fontButton22, SCHED_STEP_LBL[b], bx + STEP_W / 2, btnY + 8, COL_TEXT);
  }
}

static void drawDetailView(const DetailView& v) {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillRect(0, 50, LCD_WIDTH, NAV_Y - 4 - 50, COL_BG);
  cv->fillRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_CARD);
  cv->drawRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_BLUE);
  cv->drawRoundRect(MODAL_X + 1, MODAL_Y + 1, MODAL_W - 2, MODAL_H - 2, 15, COL_BLUE);
  bool on = v.known && v.on;

  // Záhlaví: jméno vlevo, stav a přepínač vpravo (klepnutí přepne)
  int sx = MODAL_X + MODAL_W - 20 - DET_SW_W;
  drawSwitch(cv, sx, MODAL_Y + 7, DET_SW_W, DET_SW_H, on, v.dim, v.pressed == CTL_SWITCH);
  uint16_t stateCol = !v.known ? COL_DIM : (on ? (v.dim ? COL_BLUE_DIM : COL_BLUE_LT) : (v.dim ? COL_OFF_KNOB : COL_DIM));
  smoothDrawTextRight(cv, fontButton22, !v.known ? "…" : (on ? "ZAP" : "VYP"), sx - 14, MODAL_Y + 11, stateCol);
  char name[48];
  fitText(fontTitle28, v.label, detailSplitX() - 12 - (MODAL_X + 20), name, sizeof(name));
  smoothDrawText(cv, fontTitle28, name, MODAL_X + 20, MODAL_Y + 6, v.dim ? COL_DIM : COL_TEXT);
  cv->drawFastHLine(MODAL_X + 16, MODAL_Y + DET_HEAD_H, MODAL_W - 32, COL_BORDER);

  if (v.isPlug) {
    drawPlugScheduleBlock(cv, "Zapnout v", v.schedOnEn, v.schedOnMin,
                          PLUG_ON_Y, PLUG_ON_BTN_Y, CTL_ON_SW, CTL_ON_STEP0, v.pressed, v.dim);
    cv->drawFastHLine(MODAL_X + 16, PLUG_DIV_Y, MODAL_W - 32, COL_BORDER);
    drawPlugScheduleBlock(cv, "Vypnout v", v.schedOffEn, v.schedOffMin,
                          PLUG_OFF_Y, PLUG_OFF_BTN_Y, CTL_OFF_SW, CTL_OFF_STEP0, v.pressed, v.dim);
    char info[64];
    if (on && v.onTime > 0) {
      uint32_t h = v.onTime / 3600, m = (v.onTime % 3600) / 60;
      if (h > 0) snprintf(info, sizeof(info), "V provozu: %lu h %lu min · časovač v zásuvce", (unsigned long)h, (unsigned long)m);
      else if (m > 0) snprintf(info, sizeof(info), "V provozu: %lu min · časovač v zásuvce", (unsigned long)m);
      else snprintf(info, sizeof(info), "V provozu: < 1 min · časovač v zásuvce");
    } else {
      snprintf(info, sizeof(info), "Časovač běží přímo v zásuvce (denně)");
    }
    smoothDrawTextCentered(cv, fontSmall16, info, 240, MODAL_Y + 282, COL_DIM);
    return;
  }

  // Základní barvy: aktuální má bílý rámeček, dlaždice pod prstem modrý
  if (v.colorless) {
    smoothDrawTextCentered(cv, fontSub18, "Toto světlo barvu nemá", 240, TILE_Y0 + 50, COL_DIM);
  } else {
    int8_t cur = v.hasColor ? presetIndex(v.hue, v.sat) : -1;
    for (uint8_t k = 0; k < COLOR_PRESET_N; k++) {
      int tx, ty;
      tileRect(k, tx, ty);
      uint16_t c = hsvTo565(COLOR_PRESETS[k].hue, COLOR_PRESETS[k].sat);
      if (k == cur || v.pressed == (int8_t)k) {
        uint16_t ring = (k == cur) ? COL_TEXT : COL_BLUE_LT;
        cv->drawRoundRect(tx, ty, TILE_W, TILE_H, 12, ring);
        cv->drawRoundRect(tx + 1, ty + 1, TILE_W - 2, TILE_H - 2, 11, ring);
        cv->fillRoundRect(tx + 5, ty + 5, TILE_W - 10, TILE_H - 10, 8, c);
      } else {
        cv->fillRoundRect(tx, ty, TILE_W, TILE_H, 12, c);
      }
    }
  }

  // Jas: popisek, hodnota, proužek a tlačítka po desítkách a jedničkách
  char val[12];
  if (v.bri) snprintf(val, sizeof(val), "%u %%", v.bri);
  else snprintf(val, sizeof(val), "…");
  smoothDrawText(cv, fontMenu19, "Jas", MODAL_X + 20, BRI_Y + 14, COL_DIM);
  smoothDrawTextRight(cv, fontState32, val, MODAL_X + MODAL_W - 20, BRI_Y, on ? COL_TEXT : COL_DIM);
  cv->fillRoundRect(MODAL_X + 20, BAR_Y, BAR_W, BAR_H, BAR_H / 2, COL_CARD_HI);
  if (v.bri) {
    int fw = v.bri * BAR_W / 100;
    if (fw < BAR_H) fw = BAR_H;
    uint16_t fc = on ? (v.dim ? COL_BLUE_DIM : COL_BLUE) : COL_OFF_KNOB;
    cv->fillRoundRect(MODAL_X + 20, BAR_Y, fw, BAR_H, BAR_H / 2, fc);
  }
  for (uint8_t b = 0; b < 4; b++) {
    int bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP);
    bool pr = (v.pressed == CTL_STEP0 + b);
    cv->fillRoundRect(bx, STEP_Y, STEP_W, STEP_H, 12, pr ? COL_BORDER : COL_CARD_HI);
    cv->drawRoundRect(bx, STEP_Y, STEP_W, STEP_H, 12, pr ? COL_BLUE : COL_BORDER);
    smoothDrawTextCentered(cv, fontTitle24, BRI_STEP_LBL[b], bx + STEP_W / 2, STEP_Y + 11, COL_TEXT);
  }
}

// Detail skutečného světla nebo zásuvky; čekající úprava se ukáže hned, ještě před odesláním
static void drawDetail() {
  const DevView& d = devs[detailIdx];
  bool pending = (edit.dev == (int8_t)detailIdx);
  DetailView v;
  v.label       = d.label;
  v.known       = d.st.valid;
  v.on          = d.st.on;
  v.dim         = d.stale || d.busy || !d.st.valid;
  v.isPlug      = d.st.isPlug;
  v.onTime      = d.st.onTime;
  v.schedOnEn   = (pending && edit.schedOn) ? edit.schedOnEn : d.st.schedOnEn;
  v.schedOnMin  = (pending && edit.schedOn) ? edit.schedOnMin : d.st.schedOnMin;
  v.schedOffEn  = (pending && edit.schedOff) ? edit.schedOffEn : d.st.schedOffEn;
  v.schedOffMin = (pending && edit.schedOff) ? edit.schedOffMin : d.st.schedOffMin;
  v.bri         = (pending && edit.bri) ? edit.bri : (d.st.valid ? d.st.brightness : 0);
  if (pending && edit.hue >= 0) {
    v.hasColor = true;
    v.hue = (uint16_t)edit.hue;
    v.sat = edit.sat;
  } else {
    v.hasColor = d.st.valid && d.st.hasColor;
    v.hue = d.st.hue;
    v.sat = d.st.sat;
  }
  v.colorless = d.st.valid && !d.st.hasColor && !v.hasColor;
  v.pressed   = pressedCtl;
  drawDetailView(v);
}

static void drawDemoDetail() {
  const DemoLight& l = DEMO_STEPS[demoStep].lights[0];
  DetailView v;
  v.label     = l.label;
  v.known     = l.on >= 0;
  v.on        = l.on > 0;
  v.dim       = l.dim || l.on < 0;
  v.bri       = l.bri;
  v.hasColor  = true;
  v.colorless = false;
  v.hue       = l.hue;
  v.sat       = l.sat;
  v.isPlug      = l.plug;
  v.onTime      = l.onTime;
  v.schedOnEn   = l.onEn;
  v.schedOnMin  = l.onMin;
  v.schedOffEn  = l.offEn;
  v.schedOffMin = l.offMin;
  v.pressed   = -1;
  drawDetailView(v);
}

// Prvek detailu pod bodem: 0–9 barva, CTL_STEP0.. jas, CTL_SWITCH přepínač,
// u zásuvky CTL_ON_SW, CTL_ON_STEP0.., CTL_OFF_SW, CTL_OFF_STEP0.., -1 nic.
static int8_t detailCtlAt(int tx, int ty) {
  int split = detailSplitX();
  if (hitRect(tx, ty, split, MODAL_Y, MODAL_X + MODAL_W - split, DET_HEAD_H)) return CTL_SWITCH;
  if (!demoOn && devs[detailIdx].st.isPlug) {
    if (hitRect(tx, ty, MODAL_X + 20, PLUG_ON_Y - 4, MODAL_W - 40, PLUG_SW_H + 8)) return CTL_ON_SW;
    for (uint8_t b = 0; b < 4; b++) {
      int bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP);
      if (hitRect(tx, ty, bx - STEP_GAP / 2, PLUG_ON_BTN_Y - 4, STEP_W + STEP_GAP, PLUG_BTN_H + 8))
        return (int8_t)(CTL_ON_STEP0 + b);
    }
    if (hitRect(tx, ty, MODAL_X + 20, PLUG_OFF_Y - 4, MODAL_W - 40, PLUG_SW_H + 8)) return CTL_OFF_SW;
    for (uint8_t b = 0; b < 4; b++) {
      int bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP);
      if (hitRect(tx, ty, bx - STEP_GAP / 2, PLUG_OFF_BTN_Y - 4, STEP_W + STEP_GAP, PLUG_BTN_H + 8))
        return (int8_t)(CTL_OFF_STEP0 + b);
    }
    return -1;
  }
  for (uint8_t k = 0; k < COLOR_PRESET_N; k++) {
    int x, y;
    tileRect(k, x, y);
    if (hitRect(tx, ty, x - TILE_GAP / 2, y - TILE_ROW_GAP / 2, TILE_W + TILE_GAP, TILE_H + TILE_ROW_GAP)) return (int8_t)k;
  }
  for (uint8_t b = 0; b < 4; b++) {
    int bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP);
    if (hitRect(tx, ty, bx - STEP_GAP / 2, STEP_Y - 8, STEP_W + STEP_GAP, STEP_H + 16)) return (int8_t)(CTL_STEP0 + b);
  }
  return -1;
}

// ─── Nastavení (položky a popisy pro stavový řádek) ────────────────────────
static const char* const SET_ITEMS[] = {
  "Jas displeje", "Zhasnout po", "Probuzení dotykem", "Zvuk", "Demo režim", "Spánek", "Zpět"
};
static const char* const SET_DESC[] = {
  "Nižší jas = delší výdrž baterie",
  "Prodleva zhasnutí a uspání na baterii",
  "VYP = probudí jen tlačítka BOOT a KEY",
  "Cvaknutí a tóny při přepnutí",
  "Ukázka všech podob obrazovky",
  "VYPNOUT = zapne jen podržení PWR",
  "Klepnutím zavřeš nastavení"
};
static const uint8_t SET_N = sizeof(SET_ITEMS) / sizeof(SET_ITEMS[0]);

static void drawStatusLine() {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillRect(0, CARDS_Y1, LCD_WIDTH, NAV_Y - 4 - CARDS_Y1, COL_BG);
  char s[96] = "";
  uint16_t col = COL_DIM;
  bool anyBusy = false;
  for (uint8_t i = 0; i < DEV_COUNT; i++) if (devs[i].busy) anyBusy = true;

  if (demoOn) {
    snprintf(s, sizeof(s), "Ukázka %u/%u: %s", demoStep + 1, DEMO_STEP_COUNT, DEMO_STEPS[demoStep].name);
    col = COL_BLUE_LT;
  } else if (settingsOpen) {
    snprintf(s, sizeof(s), "%s", SET_DESC[settingsSel]);
    col = COL_BLUE_LT;
  } else if (app == ST_WIFI) {
    snprintf(s, sizeof(s), "Připojuji k Wi-Fi…");
    col = COL_BLUE_LT;
  } else if (wifiFailed) {
    // V detailu Obnovit není, změna barvy nebo jasu se ale o připojení pokusí sama
    snprintf(s, sizeof(s), "%s", detailOpen ? "Wi-Fi nedostupná" : "Wi-Fi nedostupná · klepni na Obnovit");
    col = COL_HOT;
  } else if (app == ST_QUERY) {
    snprintf(s, sizeof(s), "Zjišťuji stav světel…");
    col = COL_BLUE_LT;
  } else if (anyBusy) {
    snprintf(s, sizeof(s), "Přepínám…");
    col = COL_BLUE_LT;
  } else if (detailOpen && devs[detailIdx].err[0]) {
    snprintf(s, sizeof(s), "%s", devs[detailIdx].err);
    col = COL_HOT;
  } else if (detailOpen) {
    snprintf(s, sizeof(s), "Změny platí hned · zhasnu za %lu s", (unsigned long)remainingSec());
  } else {
    snprintf(s, sizeof(s), "Klepni na jméno: barva a jas · zhasnu za %lu s", (unsigned long)remainingSec());
  }
  if (s[0]) {
    char t[96];
    fitText(fontSub18, s, 432 - 32, t, sizeof(t));
    int pw = smoothTextWidth(fontSub18, t) + 32;
    int px = 240 - pw / 2;
    uint16_t bd = (col == COL_DIM) ? COL_BORDER : col;
    uint16_t bg = (col == COL_HOT) ? COL_RED_BG : COL_CARD;
    cv->fillRoundRect(px, STATUS_Y, pw, STATUS_H, 8, bg);
    cv->drawRoundRect(px, STATUS_Y, pw, STATUS_H, 8, bd);
    smoothDrawTextCentered(cv, fontSub18, t, 240, STATUS_Y + 1, col == COL_DIM ? COL_TEXT : col);
  }
}

static void drawNav() {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillRect(0, NAV_Y - 4, LCD_WIDTH, LCD_HEIGHT - (NAV_Y - 4), COL_BG);
  const char* lbl[2];
  if (demoOn) {
    lbl[0] = "Další";
    lbl[1] = "Ukončit";
  } else {
    lbl[0] = "Obnovit";
    lbl[1] = settingsOpen ? "Zavřít" : (detailOpen ? "Zpět" : "Nastavení");
  }
  bool modal = settingsOpen || detailOpen;
  const int xs[2] = { NAV1_X, NAV2_X };
  for (int i = 0; i < 2; i++) {
    if (i == 0 && modal && !demoOn) continue;
    bool active = (pressedNav == i) || (i == 1 && (modal || demoOn));
    uint16_t bg = active ? COL_CARD_HI : COL_CARD;
    uint16_t bd = active ? COL_BLUE : COL_BORDER;
    uint16_t fg = active ? COL_TEXT : COL_DIM;
    cv->fillRoundRect(xs[i], NAV_Y, NAV_W, NAV_H, 10, bg);
    cv->drawRoundRect(xs[i], NAV_Y, NAV_W, NAV_H, 10, bd);
    smoothDrawTextCentered(cv, fontButton22, lbl[i], xs[i] + NAV_W / 2, NAV_Y + 6, fg);
  }
}

// ─── Nastavení ─────────────────────────────────────────────────────────────
static void startDemo();
static void stopDemo();

static void settingsRow(uint8_t i, char* right, size_t cap, uint16_t& col) {
  Settings& s = settings();
  right[0] = 0;
  col = COL_DIM;
  switch (i) {
    case 0: snprintf(right, cap, "%u/4", s.brightness); col = COL_BLUE_LT; break;
    case 1: snprintf(right, cap, "%u s", s.sleepSec); col = COL_BLUE_LT; break;
    case 2: snprintf(right, cap, "%s", s.wakeTouch ? "ZAP" : "VYP"); col = s.wakeTouch ? COL_BLUE : COL_DIM; break;
    case 3: snprintf(right, cap, "%s", s.sound ? "ZAP" : "VYP"); col = s.sound ? COL_BLUE : COL_DIM; break;
    case 4: snprintf(right, cap, "SPUSTIT"); col = COL_BLUE_LT; break;
    case 5: snprintf(right, cap, "%s", s.fullOff ? "VYPNOUT" : "HLUBOKÝ"); col = s.fullOff ? COL_HOT : COL_BLUE_LT; break;
    default: break;
  }
}

static void drawSettings() {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillRect(0, 50, LCD_WIDTH, NAV_Y - 4 - 50, COL_BG);
  cv->fillRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_CARD);
  cv->drawRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_BLUE);
  cv->drawRoundRect(MODAL_X + 1, MODAL_Y + 1, MODAL_W - 2, MODAL_H - 2, 15, COL_BLUE);
  smoothDrawTextCentered(cv, fontTitle24, "NASTAVENÍ", 240, MODAL_Y + 5, COL_BLUE_LT);
  cv->drawFastHLine(MODAL_X + 16, MODAL_Y + 42, MODAL_W - 32, COL_BORDER);

  for (uint8_t i = 0; i < SET_N; i++) {
    int ry = MODAL_Y + ROW_Y0 + i * ROW_H;
    bool isSel = (i == settingsSel);
    if (isSel) cv->fillRoundRect(MODAL_X + 10, ry, MODAL_W - 20, ROW_H - 4, 8, COL_CARD_HI);
    char right[24];
    uint16_t rCol;
    settingsRow(i, right, sizeof(right), rCol);
    int ty = ry + 2;
    smoothDrawText(cv, fontMenu19, SET_ITEMS[i], MODAL_X + 24, ty, isSel ? COL_TEXT : COL_DIM);
    if (right[0]) smoothDrawTextRight(cv, fontMenu19, right, MODAL_X + MODAL_W - 24, ty, rCol);
  }
}

static int settingsRowAt(int tx, int ty) {
  if (tx < MODAL_X || tx >= MODAL_X + MODAL_W) return -1;
  for (uint8_t i = 0; i < SET_N; i++) {
    int ry = MODAL_Y + ROW_Y0 + i * ROW_H;
    if (ty >= ry && ty < ry + ROW_H) return i;
  }
  return -1;
}

static void openSettings()  { settingsOpen = true;  settingsSel = 0; detailOpen = false; dirty = true; }
static void closeSettings() { settingsOpen = false; dirty = true; }

static void changeSetting(uint8_t i) {
  Settings& s = settings();
  switch (i) {
    case 0:
      s.brightness = (uint8_t)((s.brightness + 1) % 5);
      applyBrightness();
      break;
    case 1: {
      uint8_t idx = 0;
      for (uint8_t k = 0; k < SLEEP_OPTION_COUNT; k++) if (SLEEP_OPTIONS[k] == s.sleepSec) idx = k;
      s.sleepSec = SLEEP_OPTIONS[(idx + 1) % SLEEP_OPTION_COUNT];
      lastSecShown = 0xFFFFFFFF;
      break;
    }
    case 2: s.wakeTouch = !s.wakeTouch; break;
    case 3: s.sound = !s.sound; if (s.sound) click(); break;
    case 4: startDemo(); return;
    case 5: s.fullOff = !s.fullOff; break;
    default:
      closeSettings();
      return;
  }
  settingsSave();
  dirty = true;
}

// ─── Ukázka ────────────────────────────────────────────────────────────────
static void startDemo() {
  closeSettings();
  demoOn = true;
  demoStep = 0;
  demoLastMs = millis();
  pendingToggle = -1;   // během ukázky se světla neovládají
  edit = LightEdit();
  detailOpen = false;
  dirty = true;
}

static void stopDemo() {
  demoOn = false;
  dirty = true;
  lastInteractMs = millis();
}

static void nextDemoStep() {
  demoStep = (uint8_t)((demoStep + 1) % DEMO_STEP_COUNT);
  demoLastMs = millis();
  dirty = true;
}

// ─── Celá obrazovka ────────────────────────────────────────────────────────
static void render() {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillScreen(COL_BG);
  drawTopBar();
  if (demoOn)            DEMO_STEPS[demoStep].detail ? drawDemoDetail() : drawDemoCards();
  else if (settingsOpen) drawSettings();
  else if (detailOpen)   drawDetail();
  else                   drawCards();
  drawStatusLine();
  drawNav();
  hwFlush();
  dirty = false;
}

static void drawSplash() {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillScreen(COL_BG);
  // Pásek LED: osm dílků od tmavě modré po světlou
  static const uint16_t COLS[8] = { 0x094B, 0x11CD, 0x1A6F, 0x2B3F, 0x3C5F, 0x5D1F, 0x8E3F, 0xDF7F };
  int x0 = 240 - (8 * 30) / 2;
  cv->drawRoundRect(x0 - 10, 95, 8 * 30 + 20, 44, 12, COL_BORDER);
  for (int i = 0; i < 8; i++) cv->fillRoundRect(x0 + i * 30 + 3, 104, 24, 26, 6, COLS[i]);
  smoothDrawTextCentered(cv, fontState32, "PANEL SVĚTEL", 240, 168, COL_TEXT);
  smoothDrawTextCentered(cv, fontButton22, "Tapo · Wi-Fi", 240, 224, COL_DIM);
  char line[40];
  snprintf(line, sizeof(line), "%u %s", DEV_COUNT,
           DEV_COUNT == 1 ? "světlo" : ((DEV_COUNT >= 2 && DEV_COUNT <= 4) ? "světla" : "světel"));
  smoothDrawTextCentered(cv, fontTitle24, line, 240, 264, COL_BLUE_LT);
  smoothDrawTextCentered(cv, fontSub18, "klepnutím pokračovat", 240, 415, COL_DIM);
  hwFlush();
}

// ─── Akce ──────────────────────────────────────────────────────────────────
static void startWifi() {
  netBegin(12000);
  app = ST_WIFI;
  wifiFailed = false;
  dirty = true;
}

static void queryDevice(uint8_t i) {
  DevView& d = devs[i];
  d.busy = true;
  dirty = true;
  render();
  // Časovač zásuvky se při každém načtení čte znovu (mohl se změnit v aplikaci);
  // když čtení pravidel selže, zůstanou na kartě poslední známé časy
  TapoState st = d.st;
  st.schedValid = false;
  TapoKlap::Err e = tapoGetInfo(d.klap, st);
  d.busy = false;
  if (e == TapoKlap::OK) {
    d.st = st;
    d.stale = false;
    d.err[0] = 0;
    setLabel(i);
    rememberLast(i);
  } else {
    snprintf(d.err, sizeof(d.err), "%s", d.klap.errText(e));
    Serial.printf("[tapo] %s: get_device_info failed (%u, http %d)\n", d.label, e, d.klap.lastHttpStatus());
  }
  d.klap.save(g_sess[i]);
  dirty = true;
}

static void toggleDevice(uint8_t i) {
  if (i >= DEV_COUNT) return;
  lastInteractMs = millis();
  if (app == ST_WIFI || app == ST_QUERY) {
    pendingToggle = (int8_t)i;
    dirty = true;
    return;
  }
  if (wifiFailed || netState() != NET_CONNECTED) {
    pendingToggle = (int8_t)i;
    startWifi();
    return;
  }
  DevView& d = devs[i];
  if (d.busy) return;
  bool target = !(d.st.valid && d.st.on);
  if (!target && edit.dev == (int8_t)i) {
    edit.bri = 0;
    edit.hue = -1;
    if (!edit.schedOn && !edit.schedOff) edit = LightEdit();   // zhasnutí přebije čekající barvu či jas
  }
  d.busy = true;
  d.err[0] = 0;
  dirty = true;
  render();

  NetBurst burst;
  TapoKlap::Err e = tapoSetOn(d.klap, target);
  if (e == TapoKlap::OK) {
    d.st.on = target;
    d.st.valid = true;
    d.stale = false;
    if (target) PLAY(SND_ON); else PLAY(SND_OFF);
    // Ověření stavu hned po přepnutí (a aktuální jas / doba běhu)
    TapoState st = d.st;
    if (tapoGetInfo(d.klap, st) == TapoKlap::OK) {
      d.st = st;
      setLabel(i);
    }
    rememberLast(i);
  } else {
    snprintf(d.err, sizeof(d.err), "%s", d.klap.errText(e));
    Serial.printf("[tapo] %s: set_device_info failed (%u, http %d)\n", d.label, e, d.klap.lastHttpStatus());
    PLAY(SND_ERR);
    if (e == TapoKlap::ERR_CONNECT || e == TapoKlap::ERR_TIMEOUT) d.stale = true;
  }
  d.klap.save(g_sess[i]);
  d.busy = false;
  dirty = true;
  lastInteractMs = millis();
}

static void refreshAll() {
  lastInteractMs = millis();
  if (app != ST_IDLE) return;
  for (uint8_t i = 0; i < DEV_COUNT; i++) devs[i].err[0] = 0;
  if (wifiFailed || netState() != NET_CONNECTED) {
    startWifi();
  } else {
    app = ST_QUERY;
    queryIdx = 0;
  }
  dirty = true;
}

// ─── Detail světla: barva a jas ────────────────────────────────────────────
static void openDetail(uint8_t i) {
  detailOpen = true;
  detailIdx = i;
  pressedCtl = -1;
  dirty = true;
}

static void closeDetail() {
  detailOpen = false;
  pressedCtl = -1;
  dirty = true;
}

static uint16_t stepMinutes(uint16_t curMin, int16_t delta) {
  int cur = (int)(curMin % 1440);
  if ((delta == 5 || delta == -5) && (cur % 5 != 0)) {
    cur = (delta > 0) ? ((cur / 5 + 1) * 5) : ((cur / 5) * 5);
  } else {
    cur += delta;
  }
  while (cur < 0) cur += 1440;
  return (uint16_t)(cur % 1440);
}

// Pošle čekající úpravu z detailu. Hlavní smyčka ji volá až po překreslení,
// takže nová hodnota je na displeji dřív, než pásek nebo zásuvka odpoví.
static void sendLightEdit() {
  if (edit.dev < 0 || app != ST_IDLE) return;   // během připojování počká na načtení stavu
  if (wifiFailed || netState() != NET_CONNECTED) {
    startWifi();   // po připojení se úprava pošle, když se nepřipojí, propadne
    return;
  }
  uint8_t i = (uint8_t)edit.dev;
  LightEdit e = edit;
  edit = LightEdit();
  DevView& d = devs[i];
  // Barva nebo jas zadané dřív, než se zjistilo, že jde o zásuvku: zásuvka je nemá
  if (d.st.isPlug && !e.schedOn && !e.schedOff) return;
  d.err[0] = 0;
  NetBurst burst;

  if (e.schedOn || e.schedOff) {
    // Bez přečtených pravidel by se nevědělo, které upravit, a vzniklo by další;
    // když čtení selže, změna se neodešle a ukáže se chyba
    TapoKlap::Err r = d.st.schedValid ? TapoKlap::OK : tapoGetSchedule(d.klap, d.st);
    if (r == TapoKlap::OK && e.schedOn) {
      r = tapoSetSchedule(d.klap, true, e.schedOnEn, e.schedOnMin, d.st.schedOnId, sizeof(d.st.schedOnId));
      if (r == TapoKlap::OK) {
        d.st.schedOnEn = e.schedOnEn;
        d.st.schedOnMin = e.schedOnMin;
      }
    }
    if (r == TapoKlap::OK && e.schedOff) {
      r = tapoSetSchedule(d.klap, false, e.schedOffEn, e.schedOffMin, d.st.schedOffId, sizeof(d.st.schedOffId));
      if (r == TapoKlap::OK) {
        d.st.schedOffEn = e.schedOffEn;
        d.st.schedOffMin = e.schedOffMin;
      }
    }
    if (r == TapoKlap::OK) {
      d.st.schedValid = true;
      d.stale = false;
      rememberLast(i);
    } else {
      snprintf(d.err, sizeof(d.err), "%s", d.klap.errText(r));
      Serial.printf("[tapo] %s: set_schedule failed (%u, http %d)\n", d.label, r, d.klap.lastHttpStatus());
      PLAY(SND_ERR);
      if (r == TapoKlap::ERR_CONNECT || r == TapoKlap::ERR_TIMEOUT) d.stale = true;
    }
    d.klap.save(g_sess[i]);
    dirty = true;
    lastInteractMs = millis();
    return;
  }

  bool wasKnown = d.st.valid;
  bool wasOn = d.st.valid && d.st.on;
  TapoKlap::Err r = tapoSetLight(d.klap, e.bri, e.hue, e.sat);
  if (r == TapoKlap::OK) {
    d.st.valid = true;
    d.st.on = true;
    if (e.bri) d.st.brightness = e.bri;
    if (e.hue >= 0) {
      d.st.hasColor = true;
      d.st.hue = (uint16_t)e.hue;
      d.st.sat = e.sat;
    }
    d.stale = false;
    if (!wasOn) PLAY(SND_ON);
    // Stav předtím neznámý: zbytek (jas nebo barvu) doplní čtení stavu
    TapoState st;
    if (!wasKnown && tapoGetInfo(d.klap, st) == TapoKlap::OK) {
      d.st = st;
      setLabel(i);
    }
    rememberLast(i);
  } else {
    snprintf(d.err, sizeof(d.err), "%s", d.klap.errText(r));
    Serial.printf("[tapo] %s: set_device_info (jas/barva) failed (%u, http %d)\n", d.label, r, d.klap.lastHttpStatus());
    PLAY(SND_ERR);
    if (r == TapoKlap::ERR_CONNECT || r == TapoKlap::ERR_TIMEOUT) d.stale = true;
  }
  d.klap.save(g_sess[i]);
  dirty = true;
  lastInteractMs = millis();
}

// Klepnutí v detailu: přepínač, základní barva, krok jasu nebo časovač zásuvky.
static void detailTap(int tx, int ty) {
  int8_t c = detailCtlAt(tx, ty);
  if (c < 0) return;
  const DevView& d = devs[detailIdx];
  if (!d.st.isPlug && c < CTL_STEP0 && d.st.valid && !d.st.hasColor) return;   // bílá žárovka barvu nemá
  click();
  if (c == CTL_SWITCH) {
    toggleDevice(detailIdx);
    return;
  }
  if (edit.dev >= 0 && edit.dev != (int8_t)detailIdx) sendLightEdit();   // úprava jiného zařízení nejdřív
  bool pending = (edit.dev == (int8_t)detailIdx);

  if (d.st.isPlug) {
    bool curOnEn   = (pending && edit.schedOn)  ? edit.schedOnEn  : d.st.schedOnEn;
    uint16_t curOnM = (pending && edit.schedOn)  ? edit.schedOnMin : d.st.schedOnMin;
    bool curOffEn  = (pending && edit.schedOff) ? edit.schedOffEn : d.st.schedOffEn;
    uint16_t curOffM = (pending && edit.schedOff) ? edit.schedOffMin : d.st.schedOffMin;
    if (!pending) {
      edit = LightEdit();
      edit.dev = (int8_t)detailIdx;
    }
    if (c == CTL_ON_SW) {
      edit.schedOn = true;
      edit.schedOnEn = !curOnEn;
      edit.schedOnMin = curOnM;
    } else if (c >= CTL_ON_STEP0 && c < CTL_ON_STEP0 + 4) {
      edit.schedOn = true;
      edit.schedOnEn = true;
      int16_t dlt = SCHED_STEPS[c - CTL_ON_STEP0];
      uint16_t nm = stepMinutes(curOnM, dlt);
      if (nm == curOffM % 1440) nm = stepMinutes(nm, dlt);   // zapnutí a vypnutí ve stejnou minutu zásuvka odmítne (-1803)
      edit.schedOnMin = nm;
    } else if (c == CTL_OFF_SW) {
      edit.schedOff = true;
      edit.schedOffEn = !curOffEn;
      edit.schedOffMin = curOffM;
    } else if (c >= CTL_OFF_STEP0 && c < CTL_OFF_STEP0 + 4) {
      edit.schedOff = true;
      edit.schedOffEn = true;
      int16_t dlt = SCHED_STEPS[c - CTL_OFF_STEP0];
      uint16_t nm = stepMinutes(curOffM, dlt);
      if (nm == curOnM % 1440) nm = stepMinutes(nm, dlt);   // zapnutí a vypnutí ve stejnou minutu zásuvka odmítne (-1803)
      edit.schedOffMin = nm;
    }
    dirty = true;
    return;
  }

  bool lit = d.st.valid && d.st.on;
  if (c >= CTL_STEP0) {
    int cur = (pending && edit.bri) ? edit.bri : ((d.st.valid && d.st.brightness) ? d.st.brightness : 100);
    int nv = cur + BRI_STEPS[c - CTL_STEP0];
    if (nv < 1) nv = 1;
    if (nv > 100) nv = 100;
    if (nv == cur && (pending || lit)) return;
    if (!pending) {
      edit = LightEdit();
      edit.dev = (int8_t)detailIdx;
    }
    edit.bri = (uint8_t)nv;
  } else {
    const ColorPreset& p = COLOR_PRESETS[c];
    bool same = pending ? (edit.hue >= 0 && presetIndex((uint16_t)edit.hue, edit.sat) == c)
                        : (d.st.valid && d.st.hasColor && presetIndex(d.st.hue, d.st.sat) == c);
    if (same && (pending || lit)) return;
    if (!pending) {
      edit = LightEdit();
      edit.dev = (int8_t)detailIdx;
    }
    edit.hue = (int16_t)p.hue;
    edit.sat = p.sat;
    Serial.printf("[tapo] %s: barva %s\n", d.label, p.name);
  }
  dirty = true;
}

static void goSleep() {
  demoOn = false;
  settingsOpen = false;
  detailOpen = false;
  sel = 0;              // po probuzení začíná výběr tlačítky vždy od prvního světla
  selVisible = false;
  pressedCard = -1;
  pressedRight = false;
  pressedNav = -1;
  pressedCtl = -1;
  pendingToggle = -1;   // klepnutí před usnutím se po probuzení neprovede
  edit = LightEdit();
  dimmed = false;
  hwSetDisplayPower(false);
  netOff();
  if (onUsb) {
    // Na USB jen zhasnout: probuzení dotykem nebo tlačítkem je okamžité
    app = ST_DOZE;
    Serial.println("[app] doze");
    return;
  }
  if (settings().fullOff) hwPowerOff();
  hwDeepSleep(settings().wakeTouch);
}

static void wakeFromDoze() {
  for (uint8_t i = 0; i < DEV_COUNT; i++) {
    devs[i].stale = true;
    devs[i].err[0] = 0;
  }
  touchArmed = false;
  touchArmUntil = millis() + 300;
  hwTakePalmCover();   // zakrytí z doby dřímání nesmí krabičku hned zase uspat
  dimmed = false;
  hwSetDisplayPower(true);
  applyBrightness();
  lastInteractMs = millis();
  startWifi();
  render();
}

static void lowBatteryOff() {
  Arduino_Canvas* cv = hwCanvas();
  cv->fillScreen(COL_BG);
  smoothDrawTextCentered(cv, fontState32, "VYBITÁ BATERIE", 240, 190, COL_HOT);
  smoothDrawTextCentered(cv, fontSmall16, "připoj nabíječku", 240, 240, COL_DIM);
  hwFlush();
  delay(2500);
  hwPowerOff();
}

// ─── Arduino ───────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== Panel světel (Waveshare ESP32-S3 AMOLED 2.16) ===");

  if (!hwInit()) {
    Serial.println("FATAL: hwInit failed");
    while (1) delay(1000);
  }
  settingsLoad();
  applyBrightness();
  HwWakeCause cause = hwWakeCause();

  // Čas: pásmo Praha, hned z hardwarových hodin (přesnější než čas procesoru
  // po dlouhém spánku); po připojení ho zpřesní synchronizace ze sítě
  setenv("TZ", PANEL_TZ, 1);
  tzset();
  {
    time_t t;
    if (hwRtcGetUtc(t)) {
      struct timeval tv = { t, 0 };
      settimeofday(&tv, nullptr);
    }
  }
  onUsb = hwPower().usbPresent;

  // Světla ze secrets.h, relace KLAP a poslední známé stavy z RTC paměti
  bool haveLast = (g_last.magic == LAST_MAGIC);
  for (uint8_t i = 0; i < DEV_COUNT; i++) {
    DevView& d = devs[i];
    d.klap.setHost(TAPO_DEVICES[i].host);
    bool hashOk = TAPO_AUTH_HASH[0] && d.klap.setAuthHashHex(TAPO_AUTH_HASH);
    if (!hashOk) d.klap.setAuth(TAPO_USER, TAPO_PASS);
    d.klap.restore(g_sess[i]);
    d.stale = true;
    d.busy = false;
    d.err[0] = 0;
    d.label[0] = 0;
    if (haveLast && g_last.known[i]) {
      d.st.valid = true;
      d.st.on = g_last.on[i] != 0;
      d.st.isPlug = g_last.isPlug[i] != 0;
      d.st.brightness = g_last.bri[i];
      d.st.hasColor = g_last.color[i] != 0;
      d.st.hue = g_last.hue[i];
      d.st.sat = g_last.sat[i];
      // Časy ukládá rememberLast() vždy (0 = půlnoc je platný čas, ne „nevyplněno“)
      d.st.schedOnEn = g_last.schedOnEn[i] != 0;
      d.st.schedOnMin = g_last.schedOnMin[i] % 1440;
      d.st.schedOffEn = g_last.schedOffEn[i] != 0;
      d.st.schedOffMin = g_last.schedOffMin[i] % 1440;
      strncpy(d.st.nickname, g_last.label[i], sizeof(d.st.nickname) - 1);
      d.st.nickname[sizeof(d.st.nickname) - 1] = 0;
    }
    setLabel(i);
  }
  if (!haveLast) {
    memset(&g_last, 0, sizeof(g_last));
    g_last.magic = LAST_MAGIC;
  }
  Serial.printf("[boot] wake=%u usb=%d devices=%u\n", (unsigned)cause, onUsb ? 1 : 0, (unsigned)DEV_COUNT);

  bool splashByBoot = false;
  if (cause == WAKE_POWER_ON) {
    drawSplash();
    PLAY(SND_BOOT);
    const uint32_t splashEnd = millis() + 5000;
    while ((int32_t)(millis() - splashEnd) < 0) {
      hwUpdate();
      if (hwBtnBoot().wasPressed()) { splashByBoot = true; break; }
      if (hwTouch().type == TOUCH_TAP || hwBtnUser().wasPressed()) break;
      delay(20);
    }
  }

  // Prst nebo tlačítko, které krabičku probudilo (nebo zkrátilo úvodní
  // obrazovku), se nepočítá jako povel. BOOT reaguje až na puštění, proto se
  // jeho puštění spolkne i když už stisk skončil.
  touchArmed = false;
  touchArmUntil = millis() + ((cause == WAKE_TOUCH) ? 600 : 250);
  swallowA = splashByBoot || hwBtnBoot().isPressed();
  swallowB = hwBtnUser().isPressed();
  hwTakePalmCover();

  lastInteractMs = millis();
  tapoSetYield(hwYield);   // klepnutí během čekání na pásek se nesmí ztratit
  startWifi();
  render();
}

// Klepnutí nebo podržení prstu (z hlavní smyčky i z fronty během síťových volání)
static void handleTouchEvent(const TouchEvent& te) {
  lastInteractMs = millis();
  dirty = true;
  if (te.type == TOUCH_LONG_PRESS) {
    // V detailu platí podržení jako pomalé klepnutí na prvek pod prstem
    if (detailOpen && !demoOn) {
      TouchEvent t = te;
      t.type = TOUCH_TAP;
      handleTouchEvent(t);
      return;
    }
    // Podržení prstu: v ukázce ji ukončí (jako podržení BOOT), jinak nastavení
    click();
    if (demoOn) stopDemo();
    else if (settingsOpen) closeSettings();
    else openSettings();
    return;
  }
  if (te.type != TOUCH_TAP) return;
  int tx = te.x, ty = te.y;
  if (demoOn) {
    click();
    if (hitRect(tx, ty, NAV2_X, NAV_Y, NAV_W, NAV_H)) stopDemo();
    else nextDemoStep();
    return;
  }
  if (!settingsOpen && !detailOpen && hitRect(tx, ty, NAV1_X, NAV_Y, NAV_W, NAV_H)) {
    click();
    refreshAll();
  } else if (hitRect(tx, ty, NAV2_X, NAV_Y, NAV_W, NAV_H)) {
    click();
    if (settingsOpen) closeSettings();
    else if (detailOpen) closeDetail();
    else openSettings();
  } else if (settingsOpen) {
    int row = settingsRowAt(tx, ty);
    if (row >= 0) {
      settingsSel = (uint8_t)row;
      click();
      changeSetting((uint8_t)row);
    }
  } else if (detailOpen) {
    detailTap(tx, ty);
  } else {
    // Pravá část karty (stav a přepínač) přepne, zbytek otevře detail s barvou a jasem
    for (uint8_t i = 0; i < DEV_COUNT; i++) {
      int x, y, w, h;
      cardRect(i, x, y, w, h);
      if (hitRect(tx, ty, x, y, w, h)) {
        click();
        sel = i;
        cardToggleRect(i, DEV_COUNT, x, y, w, h);
        if (hitRect(tx, ty, x, y, w, h)) toggleDevice(i);
        else openDetail(i);
        break;
      }
    }
  }
}

void loop() {
  hwUpdate();
  uint32_t now = millis();
  const TouchEvent& te = hwTouch();
  HwButton& bA = hwBtnBoot();
  HwButton& bB = hwBtnUser();

  // Prst, který krabičku probudil, se nepočítá. Klepnutí vzniká až při zvednutí
  // prstu, proto se dotyk odjistí v průchodu bez události, a klepnutí z doby
  // před odjištěním (i zachycená ve frontě během čekání na síť) se zahodí.
  if (!touchArmed) {
    TouchEvent q;
    while (hwTakeQueuedTouch(q)) {}
    if ((int32_t)(now - touchArmUntil) >= 0 && !te.touching && te.type == TOUCH_NONE) touchArmed = true;
  }
  bool skipA = swallowA, skipB = swallowB;
  if (swallowA && !bA.isPressed()) swallowA = false;
  if (swallowB && !bB.isPressed()) swallowB = false;
  bool pwrShort = hwTakePowerKeyShortPress();

  // ─── Ukázka: automatické střídání obrazovek
  if (demoOn && (now - demoLastMs >= DEMO_STEP_MS)) nextDemoStep();

  // ─── Dřímání na USB: displej zhasnutý, čeká se na dotyk nebo tlačítko
  if (app == ST_DOZE) {
    const HwPowerInfo& dp = hwPower();
    if (dp.pmuOnline && !dp.usbPresent) {
      // Kabel odpojený během dřímání: dospat pořádně, jinak by baterii vybil běžící procesor
      onUsb = false;
      Serial.println("[app] USB odpojeno, usínám");
      if (settings().fullOff) hwPowerOff();
      hwDeepSleep(settings().wakeTouch);
    }
    bool touched = te.touching || (te.type != TOUCH_NONE);
    bool button = (!skipA && bA.wasPressed()) || (!skipB && bB.wasPressed());
    if (touched || button || pwrShort) {
      if (bA.isPressed()) swallowA = true;
      if (bB.isPressed()) swallowB = true;
      onUsb = hwPower().usbPresent;
      wakeFromDoze();
    } else {
      delay(60);
    }
    return;
  }

  onUsb = hwPower().usbPresent;
  const HwPowerInfo& pw = hwPower();
  // Vybitá baterie: procenta z měřiče AXP2101 nemusí být u nekalibrovaného článku
  // spolehlivá, proto se vyžaduje i nízké napětí a pár sekund běhu po startu
  if (pw.pmuOnline && !pw.usbPresent && pw.batteryConnected && pw.batteryPct <= 2
      && pw.batteryMv > 0 && pw.batteryMv < 3350 && now > 3000) {
    lowBatteryOff();
  }

  // Čas ze sítě se uloží do hardwarových hodin; každá nová minuta překreslí hodiny
  if (netTakeTimeSync()) {
    hwRtcSetUtc(time(nullptr));
    Serial.println("[time] NTP sync");
    dirty = true;
  }
  {
    static long lastMinute = -1;
    time_t t = time(nullptr);
    if (t >= CLOCK_MIN_VALID && (long)(t / 60) != lastMinute) {
      lastMinute = (long)(t / 60);
      dirty = true;
    }
  }

  if (pwrShort) { goSleep(); return; }
  if (touchArmed && hwTakePalmCover()) { goSleep(); return; }

  // ─── Dotyk: zvýraznění karty (levé části nebo přepínače), prvku detailu
  //     nebo tlačítka pod prstem
  int8_t newPressedCard = -1, newPressedNav = -1, newPressedCtl = -1;
  bool newPressedRight = false;
  if (touchArmed && te.touching) {
    if (!settingsOpen && !detailOpen && !demoOn) {
      for (uint8_t i = 0; i < DEV_COUNT; i++) {
        int x, y, w, h;
        cardRect(i, x, y, w, h);
        if (hitRect(te.curX, te.curY, x, y, w, h)) {
          newPressedCard = (int8_t)i;
          cardToggleRect(i, DEV_COUNT, x, y, w, h);
          newPressedRight = hitRect(te.curX, te.curY, x, y, w, h);
        }
      }
    }
    if (detailOpen && !demoOn) newPressedCtl = detailCtlAt(te.curX, te.curY);
    if ((demoOn || (!settingsOpen && !detailOpen)) && hitRect(te.curX, te.curY, NAV1_X, NAV_Y, NAV_W, NAV_H)) newPressedNav = 0;
    if (hitRect(te.curX, te.curY, NAV2_X, NAV_Y, NAV_W, NAV_H)) newPressedNav = 1;
  }
  if (newPressedCard != pressedCard || newPressedRight != pressedRight ||
      newPressedNav != pressedNav || newPressedCtl != pressedCtl) {
    pressedCard = newPressedCard;
    pressedRight = newPressedRight;
    pressedNav = newPressedNav;
    pressedCtl = newPressedCtl;
    dirty = true;
  }

  if (touchArmed && te.type != TOUCH_NONE) handleTouchEvent(te);

  // Klepnutí zachycená během čekání na pásek (hwYield) se vyřídí teď
  {
    TouchEvent q;
    uint8_t guard = 0;
    while (guard++ < 4 && hwTakeQueuedTouch(q)) {
      if (touchArmed) handleTouchEvent(q);
    }
  }

  // ─── BOOT: krátce = posun dolů (další světlo / položka v nastavení / ukázka),
  //           v detailu zpět; podržení = nastavení / konec ukázky
  if (!skipA) {
    if (bA.isPressed() && !aLong && bA.pressedFor(700)) {
      aLong = true;
      swallowA = true;
      click();
      lastInteractMs = now;
      if (demoOn) stopDemo();
      else if (settingsOpen) closeSettings();
      else openSettings();
    } else if (bA.wasReleased()) {
      if (!aLong) {
        click();
        lastInteractMs = now;
        if (demoOn) {
          nextDemoStep();
        } else if (settingsOpen) {
          settingsSel = (uint8_t)((settingsSel + 1) % SET_N);
          dirty = true;
        } else if (detailOpen) {
          closeDetail();
        } else if (DEV_COUNT <= 1) {
          refreshAll();
        } else {
          if (!selVisible) {
            sel = 0;
            selVisible = true;
          } else {
            sel = (uint8_t)((sel + 1) % DEV_COUNT);
          }
          dirty = true;
        }
      }
      aLong = false;
    }
  } else if (bA.wasReleased()) {
    aLong = false;
  }

  // ─── KEY: výběr / akce (přepnout vybrané světlo nebo světlo v detailu /
  //          změnit hodnotu / konec ukázky)
  if (!skipB && bB.wasPressed()) {
    click();
    lastInteractMs = now;
    if (demoOn) {
      stopDemo();
    } else if (settingsOpen) {
      changeSetting(settingsSel);
    } else if (detailOpen) {
      toggleDevice(detailIdx);
    } else {
      selVisible = true;
      dirty = true;
      toggleDevice(sel);
    }
  }

  // ─── Připojení a načtení stavu
  if (app == ST_WIFI) {
    NetState ns = netPoll();
    if (ns == NET_CONNECTED) {
      Serial.printf("[net] connected to %s, rssi %d\n", netSsid(), netRssi());
      app = ST_QUERY;
      queryIdx = 0;
      lastInteractMs = millis();
      dirty = true;
    } else if (ns == NET_FAILED) {
      Serial.println("[net] no Wi-Fi");
      wifiFailed = true;
      pendingToggle = -1;
      edit = LightEdit();
      app = ST_IDLE;
      lastInteractMs = millis();
      dirty = true;
      PLAY(SND_ERR);
    } else {
      uint32_t tick = now / 300;
      if (tick != wifiAnimTick) {
        wifiAnimTick = tick;
        dirty = true;
      }
    }
  } else if (app == ST_QUERY) {
    if (queryIdx < DEV_COUNT) {
      if (dirty) render();
      netSetPowerSave(false);
      queryDevice(queryIdx);
      queryIdx++;
    } else {
      app = ST_IDLE;
      lastInteractMs = millis();
      dirty = true;
      if (pendingToggle >= 0) {
        uint8_t t = (uint8_t)pendingToggle;
        pendingToggle = -1;
        toggleDevice(t);
      }
      netSetPowerSave(true);   // stav načtený: do usnutí stačí rádio v úsporném režimu
    }
  }

  // ─── Prodleva bez dotyku: zhasnout a usnout
  if (app == ST_IDLE) {
    uint32_t limit = idleLimitMs();
    // Čerstvý čas: síťová volání výše (přepnutí, načtení stavu) blokují a nastaví
    // lastInteractMs na pozdější čas než `now` ze začátku smyčky; rozdíl
    // `now - lastInteractMs` by v uint32_t podtekl a krabička by hned usnula.
    if (millis() - lastInteractMs >= limit) {
      goSleep();
      return;
    }
    // Ztlumení na posledních pár sekund; prst na skle ho hned zruší
    bool wantDim = !demoOn && !te.touching && limit >= 2 * DIM_BEFORE_MS
                   && millis() - lastInteractMs >= limit - DIM_BEFORE_MS;
    if (wantDim != dimmed) {
      dimmed = wantDim;
      applyBrightness();
    }
    uint32_t remSec = remainingSec();
    if (remSec != lastSecShown) {
      lastSecShown = remSec;
      dirty = true;
    }
  } else if (dimmed) {
    dimmed = false;   // připojování nebo načítání po klepnutí: plný jas
    applyBrightness();
  }

  if (dirty) render();
  // Barva nebo jas z detailu: až po překreslení, hodnota se ukáže hned a pásek
  // ji dostane vzápětí. Klepnutí během odesílání se vyřídí v dalším průchodu.
  sendLightEdit();
  delay(16);
}
