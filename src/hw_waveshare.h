#pragma once

#include <Arduino.h>
#include <time.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"

// Hardwarová vrstva krabičky Waveshare ESP32-S3-Touch-AMOLED-2.16, převzatá
// z Rozhodovače. Panel nepotřebuje IMU, ten tu není. Navíc umí hluboký spánek
// s probuzením tlačítkem nebo dotykem.

// Tlačítko s ošetřením zákmitů (aktivní v nule)
// Pin vzorkuje časovač každých 5 ms nezávisle na hlavní smyčce, která kvůli
// kreslení celé obrazovky trvá desítky ms. Stisk i puštění se zapamatují, takže
// krátké cvaknutí ani rychlé opakované stisky mezi dvěma průchody smyčky nezmizí.
struct HwButton {
  uint8_t  pin = 0;

  // Stav vzorkování (zapisuje časovač, čte update() pod zámkem)
  bool     rawLast = false;
  bool     stable = false;
  uint32_t rawChangeMs = 0;
  uint32_t stableSinceMs = 0;
  uint8_t  pressCnt = 0;
  uint8_t  releaseCnt = 0;

  // Stav pro jeden průchod smyčky
  bool     cur = false;
  bool     pressedEdge = false;
  bool     releasedEdge = false;
  uint32_t pressStartMs = 0;
  uint8_t  seenPress = 0;
  uint8_t  seenRelease = 0;

  void init(uint8_t gpioPin);
  void sample();   // volá časovač
  void update();   // volá hwUpdate() jednou za průchod smyčky

  bool isPressed() const   { return cur; }
  bool wasPressed() const  { return pressedEdge; }
  bool wasReleased() const { return releasedEdge; }
  bool pressedFor(uint32_t ms) const { return cur && (millis() - pressStartMs) >= ms; }
};

enum TouchEventType : uint8_t {
  TOUCH_NONE = 0,
  TOUCH_TAP,
  TOUCH_LONG_PRESS,
  TOUCH_SWIPE_LEFT,
  TOUCH_SWIPE_RIGHT,
  TOUCH_SWIPE_UP,
  TOUCH_SWIPE_DOWN
};

struct TouchEvent {
  TouchEventType type = TOUCH_NONE;
  int16_t x = 0;          // místo klepnutí / začátek gesta
  int16_t y = 0;
  bool    touching = false;
  int16_t curX = 0;       // poloha prstu během dotyku
  int16_t curY = 0;
};

struct HwPowerInfo {
  bool     pmuOnline = false;
  int      batteryPct = 100;
  int      batteryMv = 4150;
  int      vbusMv = 5000;
  bool     usbPresent = true;
  bool     batteryConnected = false;
  bool     charging = false;
  bool     chargeDone = false;
  int      tempC = 25;
};

// Proč krabička běží
enum HwWakeCause : uint8_t {
  WAKE_POWER_ON = 0,   // studený start (zapnutí, reset, nahrání firmwaru)
  WAKE_BUTTON,         // probuzení z hlubokého spánku tlačítkem BOOT nebo KEY
  WAKE_TOUCH,          // probuzení dotykem displeje
  WAKE_OTHER
};

// Životní cyklus
bool hwInit();
void hwUpdate();
HwWakeCause hwWakeCause();

// Displej a plátno v PSRAM (480x480)
Arduino_Canvas* hwCanvas();
Arduino_CO5300* hwDisplay();
void hwFlush();
void hwSetBrightness(uint8_t val);    // 0..255 jas AMOLED
void hwSetDisplayPower(bool on);      // CO5300 displayOn / displayOff
bool hwDisplayIsOn();

// Další znak UTF-8 z p (posune p), 0 na konci textu; neplatný bajt = '?'
uint32_t hwNextUtf8(const char*& p);

// Dotyk a tlačítka
const TouchEvent& hwTouch();
bool hwTakePalmCover();
// Během blokujících síťových volání volá firmware hwYield(): vzorkuje dotyk a
// hotová klepnutí ukládá do fronty, hlavní smyčka je pak vybere přes
// hwTakeQueuedTouch(). Tlačítka se o nic nestarají, ta vzorkuje časovač.
void hwYield();
bool hwTakeQueuedTouch(TouchEvent& out);
HwButton& hwBtnBoot();    // horní tlačítko BOOT (GPIO 0)
HwButton& hwBtnUser();    // horní tlačítko KEY (GPIO 18)
bool hwTakePowerKeyShortPress();   // krátký stisk PWR (přes AXP2101)

// Hardwarové hodiny PCF85063: běží i v hlubokém spánku, čas se v nich drží v UTC
bool hwRtcGetUtc(time_t& t);   // false = hodiny chybí nebo čas nedává smysl (po výpadku napájení)
void hwRtcSetUtc(time_t t);

// Napájení (AXP2101)
const HwPowerInfo& hwPower();
void hwPowerRefreshNow();
void hwPowerOff();                      // úplné vypnutí, zapne jen PWR
void hwDeepSleep(bool wakeByTouch);     // hluboký spánek, nevrací se; probudí BOOT, KEY (a dotyk)

// Reproduktor (ES8311 + NS4150B). Tóny jdou do fronty a přehrává je úloha na
// pozadí, volání neblokuje. freqHz = 0 je pauza.
void hwBeep(uint16_t freqHz, uint16_t durMs, uint16_t amplitude = 5500);
