#pragma once
// Přihlašovací údaje panelu. Zkopíruj tento soubor na include/secrets.h (ten je
// v .gitignore, do repozitáře se nedostane) a vyplň vlastní hodnoty.

// ─── Wi-Fi ────────────────────────────────────────────────────────────────
// Sítí může být víc, pořadí je priorita. Naposledy úspěšnou síť zkusí panel
// po probuzení jako první a bez skenování; když tam není, jednou prohledá okolí
// a připojí se k první síti ze seznamu, kterou vidí. Jen pásmo 2,4 GHz.
// Pásky jsou v síti 192.168.1.x, panel musí být ve stejném rozsahu adres.
struct WifiNet { const char* ssid; const char* pass; };
static const WifiNet WIFI_NETS[] = {
  { "domov",   "HESLO-WIFI" },
  { "obyvak",  "HESLO-WIFI" },
};

// ─── Účet Tapo ────────────────────────────────────────────────────────────
// Stejný e-mail a heslo jako v aplikaci Tapo. Z nich se počítá přihlašovací
// hash protokolu KLAP, kterým se panel hlásí přímo pásku v místní síti.
// V aplikaci Tapo musí být zapnuto: Já → Nastavení → Kompatibilita s třetími
// stranami (jinak pásek s novým firmwarem místní přihlášení odmítne).
#define TAPO_USER "email@example.com"
#define TAPO_PASS "HESLO-TAPO"
// Alternativa bez hesla ve firmwaru: 64 hex znaků z tools/tapo_hash.py.
// Když není prázdné, má přednost před TAPO_USER/TAPO_PASS.
#define TAPO_AUTH_HASH ""

// ─── Ovládaná světla a zásuvky (nejvýše 12, na displeji po 5) ─────────────
// Pořadí = pořadí karet na displeji (při více než 5 zařízeních se seznam posouvá).
// name = popisek na displeji (prázdný = jméno, které má zařízení v aplikaci Tapo),
// host = IP adresa pásku nebo zásuvky (prázdná "" = zatím nezapojeno). V routeru
// adresu zařízení rezervuj, aby se neměnila. Po změně tohoto souboru je potřeba firmware znovu nahrát.
struct TapoDev { const char* name; const char* host; };
static const TapoDev TAPO_DEVICES[] = {
  { "Obývák",       "192.168.1.57" },
  { "Dětský pokoj", "192.168.1.60" },
  // Zásuvku Tapo P100 zapiš stejně jako světlo, v detailu má místo barev časovač zap/vyp:
  // { "Dvůr",        "192.168.1.61" },
  // { "3D tiskárna", "192.168.1.62" },
};

// ─── Pevná IP panelu (volitelné) ──────────────────────────────────────────
// Pevná adresa zrychlí připojení po probuzení (odpadá čekání na DHCP).
// Prázdná = DHCP.
#define PANEL_STATIC_IP ""            // např. "192.168.1.250"
#define PANEL_GATEWAY   "192.168.1.1"
#define PANEL_SUBNET    "255.255.255.0"
#define PANEL_DNS       "192.168.1.1"
