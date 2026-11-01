#pragma once
#include <Arduino.h>

// Připojení k Wi-Fi bez blokování hlavní smyčky. Po úspěchu si pamatuje
// kanál a BSSID přístupového bodu v RTC paměti, takže po probuzení z hlubokého
// spánku se připojí bez skenování (řádově za stovky ms místo sekund).

enum NetState : uint8_t {
  NET_IDLE = 0,
  NET_CONNECTING,
  NET_CONNECTED,
  NET_FAILED
};

void        netBegin(uint32_t timeoutMs = 12000);   // spustí připojování
NetState    netPoll();                              // volat v každém průchodu smyčky
NetState    netState();
const char* netSsid();                              // SSID právě zkoušené / připojené sítě
int8_t      netRssi();
void        netOff();                               // odpojí a vypne rádio (úspora před spánkem)
// Úsporný režim rádia za připojení: true = rádio mezi majáky přístupového bodu
// spí (modem sleep), false = poslouchá pořád. Bez připojení nedělá nic.
void        netSetPowerSave(bool on);

// Čas ze sítě (NTP) se spouští po připojení; pásmo Praha (SEČ/SELČ)
#define PANEL_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
bool        netTakeTimeSync();                      // true jednou po každé synchronizaci času
