#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Nastavení panelu uložené v NVS (přežije vypnutí i přehrání firmwaru)
struct Settings {
  uint8_t brightness;   // 0..4 → jas AMOLED 30..255, výchozí 3
  uint8_t sleepSec;     // zhasnutí na baterii po N s bez dotyku (10/20/30/60/120)
  bool    wakeTouch;    // probuzení z hlubokého spánku i dotykem (ne jen tlačítky)
  bool    sound;        // cvaknutí a tóny při přepnutí
  bool    fullOff;      // místo hlubokého spánku úplné vypnutí (zapne jen PWR)
};

static const uint8_t SLEEP_OPTIONS[] = { 10, 20, 30, 60, 120 };
static const uint8_t SLEEP_OPTION_COUNT = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);

static Settings    _settings = { 3, 20, true, true, false };
static Preferences _prefs;

inline Settings& settings() { return _settings; }

inline void settingsLoad() {
  if (!_prefs.begin("panel", true)) return;   // první start: namespace ještě neexistuje
  _settings.brightness = _prefs.getUChar("bri", 3);
  if (_settings.brightness > 4) _settings.brightness = 3;
  _settings.sleepSec   = _prefs.getUChar("slp", 20);
  _settings.wakeTouch  = _prefs.getBool("wtouch", true);
  _settings.sound      = _prefs.getBool("snd", true);
  _settings.fullOff    = _prefs.getBool("off", false);
  _prefs.end();

  bool known = false;
  for (uint8_t i = 0; i < SLEEP_OPTION_COUNT; i++) {
    if (SLEEP_OPTIONS[i] == _settings.sleepSec) known = true;
  }
  if (!known) _settings.sleepSec = 20;
}

inline void settingsSave() {
  if (!_prefs.begin("panel", false)) return;
  _prefs.putUChar("bri",   _settings.brightness);
  _prefs.putUChar("slp",   _settings.sleepSec);
  _prefs.putBool("wtouch", _settings.wakeTouch);
  _prefs.putBool("snd",    _settings.sound);
  _prefs.putBool("off",    _settings.fullOff);
  _prefs.end();
}
