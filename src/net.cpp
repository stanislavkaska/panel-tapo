#include "net.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <string.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Chybí include/secrets.h – zkopíruj include/secrets.example.h na include/secrets.h a vyplň údaje"
#endif

static const uint8_t NET_COUNT = (uint8_t)(sizeof(WIFI_NETS) / sizeof(WIFI_NETS[0]));
static const uint32_t NET_MAGIC = 0x4E455431;   // "NET1"
static const uint32_t SCAN_MS_PER_CHAN = 120;   // aktivní sken 13 kanálů trvá asi 1,5 s

// Rychlé připojení: kanál a BSSID z minula přežijí hluboký spánek
struct NetCache {
  uint32_t magic;
  uint8_t  netIdx;
  uint8_t  channel;
  uint8_t  bssid[6];
};
RTC_DATA_ATTR static NetCache g_netCache;

// Postup připojení (netBegin, pak netPoll v každém průchodu smyčky):
//   PH_FAST   síť z minula napřímo na zapamatovaném kanálu a přístupovém bodu
//   PH_SCAN   jedno prohledání všech kanálů (bez sítě z minula hned na začátku)
//   PH_JOIN   viditelné sítě ze seznamu popořadě, rovnou na nalezeném kanálu;
//             u sítě s víc přístupovými body ten nejsilnější
//   PH_BLIND  sítě, které sken neviděl (třeba se skrytým SSID), hledáním podle jména
// Celé připojení má časový limit z netBegin(), potom NET_FAILED.
enum Phase : uint8_t { PH_FAST, PH_SCAN, PH_JOIN, PH_BLIND };

struct Candidate {
  uint8_t idx;        // index ve WIFI_NETS
  uint8_t channel;
  int8_t  rssi;
  uint8_t bssid[6];
};

static NetState  _state = NET_IDLE;
static Phase     _phase = PH_FAST;
static int8_t    _cur = -1;            // index ve WIFI_NETS
static Candidate _cand[NET_COUNT];     // viditelné sítě ze seznamu v pořadí seznamu
static uint8_t   _candN = 0, _candPos = 0;
static bool      _seen[NET_COUNT];     // síť ze seznamu, kterou sken našel
static uint8_t   _blindPos = 0;
static bool      _scanRunning = false;
static uint32_t  _deadline = 0;
static uint32_t  _phaseStart = 0;
static uint32_t  _phaseBudget = 0;
static uint32_t  _phaseMin = 0;        // dřív se chybový stav nebere vážně (viz startAttempt)
static volatile bool _timeSynced = false;   // nastavuje úloha SNTP, čte hlavní smyčka
static bool      _powerSave = false;   // rádio v úsporném režimu (modem sleep)

static void onTimeSync(struct timeval*) { _timeSynced = true; }

static void applyStaticIp() {
  if (PANEL_STATIC_IP[0] == 0) return;
  IPAddress ip, gw, mask, dns;
  if (!ip.fromString(PANEL_STATIC_IP)) return;
  gw.fromString(PANEL_GATEWAY);
  mask.fromString(PANEL_SUBNET);
  if (!dns.fromString(PANEL_DNS)) dns = gw;
  WiFi.config(ip, gw, mask, dns);
}

// Pokus o připojení k síti idx; channel 0 = hledat podle jména na všech kanálech
static void startAttempt(Phase ph, uint8_t idx, uint8_t channel, const uint8_t* bssid, uint32_t budget) {
  _phase = ph;
  _cur = (int8_t)idx;
  _phaseStart = millis();
  _phaseBudget = budget;
  // WiFi.status() mění jen událost ovladače; po neúspěšném pokusu v něm zůstane
  // chyba, dokud se nový pokus nepřidruží k přístupovému bodu (WL_IDLE_STATUS).
  // Chybový stav se proto uznává až po minimální době: přímé připojení na známý
  // kanál se přidruží do pár set ms, hledání podle jména prochází všechny kanály.
  _phaseMin = (ph == PH_FAST) ? 1200 : (ph == PH_JOIN ? 2000 : 2500);
  const WifiNet& n = WIFI_NETS[idx];
  if (channel) {
    WiFi.begin(n.ssid, n.pass, channel, bssid, true);
  } else {
    WiFi.begin(n.ssid, n.pass);
  }
}

// Jedno prohledání všech kanálů. Když ho ovladač hned nepřijme (ještě ukončuje
// předchozí pokus), netPoll() ho zkouší spustit znovu.
static void startScan() {
  _phase = PH_SCAN;
  _phaseStart = millis();
  _phaseBudget = 4000;   // pojistka, sken trvá asi 1,5 s
  _scanRunning = WiFi.scanNetworks(true, false, false, SCAN_MS_PER_CHAN) == WIFI_SCAN_RUNNING;
}

// Z výsledků skenu vybere sítě ze seznamu: v pořadí seznamu, u každého SSID
// nejsilnější přístupový bod. found < 0 = sken selhal, zbudou pokusy podle jména.
static void takeScanResults(int16_t found) {
  _candN = 0;
  _candPos = 0;
  _blindPos = 0;
  if (found > 255) found = 255;
  for (uint8_t k = 0; k < NET_COUNT; k++) {
    _seen[k] = false;
    int16_t best = -1;
    int32_t bestRssi = -1000;
    for (int16_t j = 0; j < found; j++) {
      int32_t rssi = WiFi.RSSI((uint8_t)j);
      if (rssi <= bestRssi || WiFi.SSID((uint8_t)j) != WIFI_NETS[k].ssid) continue;
      best = j;
      bestRssi = rssi;
    }
    const uint8_t* b = (best >= 0) ? WiFi.BSSID((uint8_t)best) : nullptr;
    if (!b) continue;
    _seen[k] = true;
    Candidate& c = _cand[_candN++];
    c.idx = k;
    c.channel = (uint8_t)WiFi.channel((uint8_t)best);
    c.rssi = (int8_t)bestRssi;
    memcpy(c.bssid, b, 6);
  }
  Serial.printf("[net] sken: %d sítí, ze seznamu %u\n", (int)found, (unsigned)_candN);
  WiFi.scanDelete();
}

// Další pokus po skenu: nejdřív viditelné sítě, pak zbylé podle jména.
// false = nic dalšího nezbývá.
static bool nextAttempt() {
  int32_t left = (int32_t)(_deadline - millis());
  uint32_t remaining = left > 0 ? (uint32_t)left : 0;   // po limitu skončí pokus hned
  if (_candPos < _candN) {
    const Candidate& c = _cand[_candPos++];
    Serial.printf("[net] %s: kanál %u, %d dBm\n", WIFI_NETS[c.idx].ssid, (unsigned)c.channel, (int)c.rssi);
    startAttempt(PH_JOIN, c.idx, c.channel, c.bssid, remaining < 5000 ? remaining : 5000);
    return true;
  }
  while (_blindPos < NET_COUNT && _seen[_blindPos]) _blindPos++;
  if (_blindPos >= NET_COUNT) return false;
  Serial.printf("[net] %s: hledám podle jména\n", WIFI_NETS[_blindPos].ssid);
  startAttempt(PH_BLIND, _blindPos++, 0, nullptr, remaining < 6000 ? remaining : 6000);
  return true;
}

static NetState fail() {
  if (_scanRunning) esp_wifi_scan_stop();
  _scanRunning = false;
  WiFi.scanDelete();
  WiFi.disconnect(true, false);
  _state = NET_FAILED;
  return _state;
}

void netBegin(uint32_t timeoutMs) {
  _powerSave = false;
  if (NET_COUNT == 0) {
    _state = NET_FAILED;
    return;
  }
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setHostname("panel-svetel");   // před spuštěním stanice, jinak DHCP uvidí výchozí jméno
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // připojování a první čtení stavu naplno, úsporný režim až potom
  applyStaticIp();

  _deadline = millis() + timeoutMs;
  _state = NET_CONNECTING;
  _candN = _candPos = _blindPos = 0;
  _scanRunning = false;
  for (uint8_t k = 0; k < NET_COUNT; k++) _seen[k] = false;

  if (g_netCache.magic == NET_MAGIC && g_netCache.netIdx < NET_COUNT) {
    Serial.printf("[net] %s: z minula, kanál %u\n", WIFI_NETS[g_netCache.netIdx].ssid, (unsigned)g_netCache.channel);
    startAttempt(PH_FAST, g_netCache.netIdx, g_netCache.channel, g_netCache.bssid, 3000);
  } else {
    startScan();
  }
}

NetState netPoll() {
  if (_state != NET_CONNECTING) return _state;
  uint32_t now = millis();
  bool late = (int32_t)(now - _deadline) >= 0;

  if (_phase == PH_SCAN) {
    if (!_scanRunning) {
      // Ovladač sken ještě nepřijal: zkoušet znovu, po půl sekundě hledat podle jména
      _scanRunning = WiFi.scanNetworks(true, false, false, SCAN_MS_PER_CHAN) == WIFI_SCAN_RUNNING;
      if (!_scanRunning && now - _phaseStart < 500 && !late) return _state;
    }
    int16_t found = _scanRunning ? WiFi.scanComplete() : (int16_t)WIFI_SCAN_FAILED;
    if (found == WIFI_SCAN_RUNNING) {
      if (now - _phaseStart < _phaseBudget && !late) return _state;
      esp_wifi_scan_stop();   // sken nedoběhl: dál bez jeho výsledků
      found = WIFI_SCAN_FAILED;
    }
    _scanRunning = false;
    takeScanResults(found);
    if (late || !nextAttempt()) return fail();
    return _state;
  }

  wl_status_t s = WiFi.status();
  if (s == WL_CONNECTED) {
    const uint8_t* b = WiFi.BSSID();
    if (b) memcpy(g_netCache.bssid, b, 6);
    g_netCache.channel = (uint8_t)WiFi.channel();
    g_netCache.netIdx = (uint8_t)_cur;
    g_netCache.magic = NET_MAGIC;
    _state = NET_CONNECTED;
    // Čas ze sítě; hotovou synchronizaci si hlavní smyčka vyzvedne přes netTakeTimeSync()
    sntp_set_time_sync_notification_cb(onTimeSync);
    configTzTime(PANEL_TZ, "pool.ntp.org", "time.google.com");
    return _state;
  }

  // Špatné heslo hlásí ovladač často jako vypršení handshaku (WL_CONNECTION_LOST)
  uint32_t elapsed = now - _phaseStart;
  bool failed = (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL || s == WL_CONNECTION_LOST);
  bool phaseOver = elapsed >= _phaseBudget || (failed && elapsed >= _phaseMin);
  if (!phaseOver) return _state;
  if (late) return fail();

  WiFi.disconnect(false, false);
  if (_phase == PH_FAST) {
    // Síť z minula na svém kanálu není (jiné místo nebo router změnil kanál):
    // jedno prohledání okolí rozhodne, ke které síti ze seznamu se připojit
    g_netCache.magic = 0;
    startScan();
    return _state;
  }
  if (!nextAttempt()) return fail();
  return _state;
}

NetState netState() {
  // Výpadek sítě během bdění: další pokus o přepnutí spustí nové připojení
  if (_state == NET_CONNECTED && WiFi.status() != WL_CONNECTED) _state = NET_FAILED;
  return _state;
}

const char* netSsid() {
  if (_cur < 0 || _cur >= (int8_t)NET_COUNT) return "";
  return WIFI_NETS[_cur].ssid;
}

int8_t netRssi() {
  if (_state != NET_CONNECTED) return 0;
  return (int8_t)WiFi.RSSI();
}

bool netTakeTimeSync() {
  bool v = _timeSynced;
  _timeSynced = false;
  return v;
}

void netOff() {
  if (esp_sntp_enabled()) esp_sntp_stop();
  if (_scanRunning) esp_wifi_scan_stop();
  _scanRunning = false;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  _state = NET_IDLE;
  _powerSave = false;
}

void netSetPowerSave(bool on) {
  if (on == _powerSave) return;
  if (on && _state != NET_CONNECTED) return;
  // Vypnutí úsporného režimu ohlásí přístupovému bodu, že stanice bdí, a ten jí
  // hned pošle zadržené pakety; odpověď pásku tak nečeká na další maják.
  if (esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) == ESP_OK) _powerSave = on;
}
