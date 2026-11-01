#include "hw_waveshare.h"
#include <Wire.h>
#include <math.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <soc/soc_caps.h>

#include "XPowersLib.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorPCF85063.hpp"

#ifndef PANEL_AUDIO
#define PANEL_AUDIO 1
#endif
#ifndef PANEL_SLEEP_AUDIO_RAIL_OFF
#define PANEL_SLEEP_AUDIO_RAIL_OFF 0
#endif

#if PANEL_AUDIO
#include <ESP_I2S.h>
#include "es8311.h"
#endif

// ─── Displej a framebuffer v PSRAM ─────────────────────────────────────────
static Arduino_DataBus* _bus = nullptr;
static Arduino_CO5300*  _gfx = nullptr;
static Arduino_Canvas*  _canvas = nullptr;
static bool             _dispOn = true;

// ─── Periferie na sdílené I2C (SDA=15, SCL=14) ─────────────────────────────
static XPowersPMU       _pmu;
static TouchDrvCST92xx  _touch;
static SensorPCF85063   _rtc;

static bool _pmuOk   = false;
static bool _touchOk = false;
static bool _rtcOk   = false;
static bool _audioOk = false;

static HwButton    _btnBoot;
static HwButton    _btnUser;
static portMUX_TYPE _btnMux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t _btnTimer = nullptr;
static TouchEvent  _touchEvt;
static HwPowerInfo _pwrInfo;
static HwWakeCause _wakeCause = WAKE_POWER_ON;

static volatile bool _tpIrqPending = false;
static volatile bool _palmCovered  = false;
static bool          _pwrKeyShort  = false;

static void IRAM_ATTR _onTouchIrq() {
  _tpIrqPending = true;
}

static bool _takeTouchIrq() {
  noInterrupts();
  bool p = _tpIrqPending;
  _tpIrqPending = false;
  interrupts();
  return p;
}

// ─── I2C pomocné funkce s C vazbou pro es8311.c ────────────────────────────
extern "C" esp_err_t hwI2cWrite(uint8_t addr, const uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(buf, len);
  return (Wire.endTransmission() == 0) ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t hwI2cWriteRead(uint8_t addr, uint8_t reg, uint8_t* val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return ESP_FAIL;
  if (Wire.requestFrom((int)addr, 1) != 1) return ESP_FAIL;
  *val = (uint8_t)Wire.read();
  return ESP_OK;
}

// ─── Zvuk (ES8311 + ESP_I2S) ───────────────────────────────────────────────
#if PANEL_AUDIO
static const int AUDIO_SAMPLE_RATE = 16000;
static es8311_handle_t _esHandle = nullptr;
static I2SClass _i2s;

// Fronta tónů a úloha, která je přehrává na jádru 0 (Arduino loop běží na jádru 1)
struct ToneReq {
  uint16_t freq;   // Hz, 0 = pauza
  uint16_t durMs;
  uint16_t amp;
};
static QueueHandle_t _toneQ = nullptr;
static void _audioTask(void*);
static bool _i2sOn = false;

static bool _i2sStart() {
  if (_i2sOn) return true;
  _i2sOn = _i2s.begin(I2S_MODE_STD, AUDIO_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH);
  return _i2sOn;
}

static void _i2sStop() {
  if (!_i2sOn) return;
  _i2s.end();
  _i2sOn = false;
}

static bool _initAudio() {
  pinMode(PA_ENABLE_PIN, OUTPUT);
  digitalWrite(PA_ENABLE_PIN, LOW); // zesilovač jen během tónu

  _i2s.setPins(I2S_BCLK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, -1, I2S_MCLK_PIN);
  // Kodek se nastavuje při běžících hodinách MCLK
  if (!_i2sStart()) {
    return false;
  }

  _esHandle = es8311_create(0, ES8311_ADDRRES_0);
  if (!_esHandle) return false;

  es8311_clock_config_t clk = {};
  clk.mclk_inverted = false;
  clk.sclk_inverted = false;
  clk.mclk_from_mclk_pin = true;
  clk.mclk_frequency = AUDIO_SAMPLE_RATE * 256;
  clk.sample_frequency = AUDIO_SAMPLE_RATE;

  if (es8311_init(_esHandle, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    return false;
  }
  es8311_sample_frequency_config(_esHandle, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(_esHandle, false);
  es8311_voice_volume_set(_esHandle, 72, nullptr);

  _toneQ = xQueueCreate(32, sizeof(ToneReq));
  if (!_toneQ) return false;
  xTaskCreatePinnedToCore(_audioTask, "audio", 4096, nullptr, 2, nullptr, 0);
  // Mezi melodiemi se I2S zastaví: bez hodin MCLK stojí i kodek a DMA nebudí procesor
  _i2sStop();
  return true;
}

// Před hlubokým spánkem: analogová část kodeku (reference, DAC, ADC, PGA) by
// bez hodin dál brala proud z větve ALDO1. Po probuzení ji es8311_init() nastaví
// celou znovu. Pořadí podle es8311_suspend z ESP-ADF, jen bez registru 0x45,
// který inicializace nenastavuje.
static void _audioPowerDown() {
  if (!_esHandle) return;
  static const uint8_t SEQ[][2] = {
    { 0x32, 0x00 },   // hlasitost DAC na nulu
    { 0x17, 0x00 },   // zisk ADC na nulu
    { 0x0E, 0xFF },   // vypnout PGA a modulátor ADC
    { 0x12, 0x02 },   // vypnout DAC
    { 0x14, 0x00 },   // odpojit analogový vstup
    { 0x0D, 0xFA },   // vypnout analogové obvody a reference
  };
  for (auto& r : SEQ) hwI2cWrite(ES8311_ADDRRES_0, r, 2);
}
#endif // PANEL_AUDIO

// Akcelerometr QMI8658 (0x6B) panel nepoužívá: vypnout mu vnitřní oscilátor
// (CTRL1 bit 0), ať nebere proud za bdění ani ve spánku. Když na sběrnici není, nic.
static void _imuOff() {
  const uint8_t addr = 0x6B, CTRL1 = 0x02;
  uint8_t v = 0;
  if (hwI2cWriteRead(addr, CTRL1, &v) != ESP_OK) return;
  uint8_t buf[2] = { CTRL1, (uint8_t)(v | 0x01) };
  hwI2cWrite(addr, buf, 2);
}

// ─── Telemetrie napájení ───────────────────────────────────────────────────
void hwPowerRefreshNow() {
  if (!_pmuOk) {
    _pwrInfo.pmuOnline = false;
    _pwrInfo.usbPresent = true;
    _pwrInfo.batteryPct = 100;
    return;
  }
  _pwrInfo.pmuOnline = true;
  _pwrInfo.usbPresent = _pmu.isVbusIn();
  _pwrInfo.vbusMv = _pwrInfo.usbPresent ? (int)_pmu.getVbusVoltage() : 0;
  _pwrInfo.batteryConnected = _pmu.isBatteryConnect();
  _pwrInfo.batteryMv = _pwrInfo.batteryConnected ? (int)_pmu.getBattVoltage() : 0;
  if (_pwrInfo.batteryConnected) {
    int p = _pmu.getBatteryPercent();
    if (p < 0) {
      p = (_pwrInfo.batteryMv - 3200) / 10;
    }
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    _pwrInfo.batteryPct = p;
  } else {
    _pwrInfo.batteryPct = 100;
  }
  _pwrInfo.charging = _pmu.isCharging();
  _pwrInfo.chargeDone = (_pmu.getChargerStatus() == XPOWERS_AXP2101_CHG_DONE_STATE);
  _pwrInfo.tempC = (int)_pmu.getTemperature();
}

// ─── Důvod startu ──────────────────────────────────────────────────────────
static void _detectWakeCause() {
  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  if (c == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t st = esp_sleep_get_ext1_wakeup_status();
    _wakeCause = (st & (1ULL << TP_INT)) ? WAKE_TOUCH : WAKE_BUTTON;
  } else if (c == ESP_SLEEP_WAKEUP_UNDEFINED) {
    _wakeCause = WAKE_POWER_ON;
  } else {
    _wakeCause = WAKE_OTHER;
  }
}

HwWakeCause hwWakeCause() { return _wakeCause; }

// ─── Inicializace hardwaru ─────────────────────────────────────────────────
bool hwInit() {
  _detectWakeCause();

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  // 1. AXP2101 PMU
  _pmuOk = _pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (_pmuOk) {
    // Větve periferií: ALDO1 = 3,3 V zvuk, ALDO2 = 3,3 V periferie
    _pmu.setALDO1Voltage(3300);
    _pmu.enableALDO1();
    _pmu.setALDO2Voltage(3300);
    _pmu.enableALDO2();

    _pmu.enableTemperatureMeasure();
    _pmu.enableBattDetection();
    _pmu.enableVbusVoltageMeasure();
    _pmu.enableBattVoltageMeasure();
    _pmu.enableSystemVoltageMeasure();

    _pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    _pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    // PWR: zapnutí po 0,5 s, vypnutí podržením 6 s
    _pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_512MS);
    _pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_6S);
    _pmu.clearIrqStatus();
    _pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    hwPowerRefreshNow();
    Serial.println("[hw] AXP2101 PMU online");
  } else {
    Serial.println("[hw] WARN: AXP2101 PMU not detected");
  }

  // 2. Horní tlačítka
  _btnBoot.init(BTN_BOOT_PIN);
  _btnUser.init(BTN_USER_PIN);
  {
    esp_timer_create_args_t ta = {};
    ta.callback = [](void*) { _btnBoot.sample(); _btnUser.sample(); };
    ta.name = "btn";
    if (esp_timer_create(&ta, &_btnTimer) == ESP_OK) {
      esp_timer_start_periodic(_btnTimer, 5000);
    } else {
      _btnTimer = nullptr;
      Serial.println("[hw] WARN: button timer failed");
    }
  }

  // 3. CO5300 480x480 QSPI AMOLED + plátno 480x480 v PSRAM
  _bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  _gfx = new Arduino_CO5300(
    _bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
  _canvas = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, _gfx);

  if (!_canvas->begin()) {
    Serial.println("[hw] ERROR: Canvas/CO5300 begin failed!");
    return false;
  }
  // Orientace panelu Waveshare (MADCTL)
  _bus->writeC8D8(0x36, 0xA0);
  _gfx->setBrightness(200);
  _canvas->fillScreen(0x0000);
  _canvas->flush();
  Serial.printf("[hw] AMOLED 480x480 ready (PSRAM free: %lu KB)\n", (unsigned long)(ESP.getFreePsram() / 1024));

  // 4. Kapacitní dotyk CST9220
  _touch.setPins(TP_RST, TP_INT);
  _touchOk = _touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (_touchOk) {
    _touch.setCoverScreenCallback([](void*) {
      _palmCovered = true;
    }, nullptr);
    _touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    _touch.setSwapXY(true);
    _touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), _onTouchIrq, FALLING);
    Serial.printf("[hw] Touch online: %s\n", _touch.getModelName());
  } else {
    Serial.println("[hw] WARN: CST9220 touch not detected");
  }

  // 5. Hardwarové hodiny PCF85063
  _rtcOk = _rtc.begin(Wire, IIC_SDA, IIC_SCL);
  Serial.printf("[hw] PCF85063 RTC: %s\n", _rtcOk ? "online" : "not detected");

  // 6. Kodek ES8311
#if PANEL_AUDIO
  _audioOk = _initAudio();
  Serial.printf("[hw] ES8311 audio: %s\n", _audioOk ? "online" : "disabled");
#endif

  // 7. Nepoužívaný akcelerometr do úsporného režimu
  _imuOff();

  return true;
}

// ─── Stavový automat gest dotyku ───────────────────────────────────────────
static bool     _wasDown = false;
static int16_t  _downX = 0, _downY = 0;
static int16_t  _lastX = 0, _lastY = 0;
static uint32_t _downMs = 0;
static uint32_t _lastPollMs = 0;
static bool     _longFired = false;

static void _pollTouch() {
  _touchEvt.type = TOUCH_NONE;
  if (!_touchOk) return;

  uint32_t now = millis();
  bool shouldRead = _takeTouchIrq() || (_wasDown && (now - _lastPollMs >= 35));
  if (!shouldRead) {
    if (_wasDown && !_longFired && (now - _downMs >= 650)) {
      int dx = abs(_lastX - _downX);
      int dy = abs(_lastY - _downY);
      if (dx < 28 && dy < 28) {
        _longFired = true;
        _touchEvt.type = TOUCH_LONG_PRESS;
        _touchEvt.x = _downX;
        _touchEvt.y = _downY;
      }
    }
    return;
  }

  _lastPollMs = now;
  int16_t tx[2] = {0}, ty[2] = {0};
  uint8_t n = _touch.getPoint(tx, ty, 2);

  if (n > 0) {
    _lastX = tx[0];
    _lastY = ty[0];
    if (!_wasDown) {
      _wasDown = true;
      _downX = _lastX;
      _downY = _lastY;
      _downMs = now;
      _longFired = false;
    }
    _touchEvt.touching = true;
    _touchEvt.curX = _lastX;
    _touchEvt.curY = _lastY;

    if (!_longFired && (now - _downMs >= 650)) {
      int dx = abs(_lastX - _downX);
      int dy = abs(_lastY - _downY);
      if (dx < 28 && dy < 28) {
        _longFired = true;
        _touchEvt.type = TOUCH_LONG_PRESS;
        _touchEvt.x = _downX;
        _touchEvt.y = _downY;
      }
    }
  } else {
    if (_wasDown) {
      _wasDown = false;
      _touchEvt.touching = false;
      if (!_longFired) {
        int dx = _lastX - _downX;
        int dy = _lastY - _downY;
        int adx = abs(dx), ady = abs(dy);
        if (adx > 64 && adx > ady) {
          _touchEvt.type = (dx < 0) ? TOUCH_SWIPE_LEFT : TOUCH_SWIPE_RIGHT;
          _touchEvt.x = _downX;
          _touchEvt.y = _downY;
        } else if (ady > 64 && ady > adx) {
          _touchEvt.type = (dy < 0) ? TOUCH_SWIPE_UP : TOUCH_SWIPE_DOWN;
          _touchEvt.x = _downX;
          _touchEvt.y = _downY;
        } else if ((now - _downMs) < 700) {
          _touchEvt.type = TOUCH_TAP;
          _touchEvt.x = _lastX;
          _touchEvt.y = _lastY;
        }
      }
    }
  }
}

// Fronta klepnutí zachycených během blokujících volání (hwYield)
static TouchEvent _touchQ[4];
static uint8_t    _tqHead = 0;
static uint8_t    _tqCount = 0;

void hwYield() {
  _pollTouch();
  if ((_touchEvt.type == TOUCH_TAP || _touchEvt.type == TOUCH_LONG_PRESS) && _tqCount < 4) {
    _touchQ[(_tqHead + _tqCount) % 4] = _touchEvt;
    _tqCount++;
  }
}

bool hwTakeQueuedTouch(TouchEvent& out) {
  if (_tqCount == 0) return false;
  out = _touchQ[_tqHead];
  out.touching = false;
  _tqHead = (uint8_t)((_tqHead + 1) % 4);
  _tqCount--;
  return true;
}

void hwUpdate() {
  _btnBoot.update();
  _btnUser.update();
  _pollTouch();

  uint32_t now = millis();

  // Krátký stisk PWR přes AXP2101 každých 60 ms, baterie každou sekundu
  static uint32_t lastPmuIrqMs = 0;
  static uint32_t lastPmuBatMs = 0;
  if (_pmuOk && (now - lastPmuIrqMs >= 60)) {
    lastPmuIrqMs = now;
    _pmu.getIrqStatus();
    if (_pmu.isPekeyShortPressIrq()) {
      _pwrKeyShort = true;
    }
    _pmu.clearIrqStatus();
  }
  if (_pmuOk && (now - lastPmuBatMs >= 1000)) {
    lastPmuBatMs = now;
    hwPowerRefreshNow();
  }
}

Arduino_Canvas* hwCanvas()  { return _canvas; }
Arduino_CO5300* hwDisplay() { return _gfx; }

void hwFlush() {
  if (_canvas && _dispOn) _canvas->flush();
}

void hwSetBrightness(uint8_t val) {
  if (_gfx) _gfx->setBrightness(val);
}

void hwSetDisplayPower(bool on) {
  if (!_gfx || _dispOn == on) return;
  _dispOn = on;
  if (on) _gfx->displayOn();
  else    _gfx->displayOff();
}

bool hwDisplayIsOn() { return _dispOn; }

// ─── Dekódování UTF-8 (pro písma v smooth_font.cpp) ────────────────────────
uint32_t hwNextUtf8(const char*& p) {
  if (!p || !*p) return 0;
  uint8_t b0 = (uint8_t)*p++;
  if (b0 < 0x80) return b0;
  if ((b0 & 0xE0) == 0xC0) {
    uint8_t b1 = (uint8_t)*p;
    if ((b1 & 0xC0) != 0x80) return '?';
    p++;
    return ((uint32_t)(b0 & 0x1F) << 6) | (b1 & 0x3F);
  }
  if ((b0 & 0xF0) == 0xE0) {
    uint8_t b1 = (uint8_t)*p;
    if ((b1 & 0xC0) != 0x80) return '?';
    p++;
    uint8_t b2 = (uint8_t)*p;
    if ((b2 & 0xC0) != 0x80) return '?';
    p++;
    return ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (b2 & 0x3F);
  }
  if ((b0 & 0xF8) == 0xF0) {
    uint32_t cp = (uint32_t)(b0 & 0x07) << 18;
    for (int s = 12; s >= 0; s -= 6) {
      uint8_t bx = (uint8_t)*p;
      if ((bx & 0xC0) != 0x80) return '?';
      p++;
      cp |= (uint32_t)(bx & 0x3F) << s;
    }
    return cp;
  }
  return '?';
}

const TouchEvent& hwTouch() { return _touchEvt; }

bool hwTakePalmCover() {
  noInterrupts();
  bool c = _palmCovered;
  _palmCovered = false;
  interrupts();
  return c;
}

void HwButton::init(uint8_t gpioPin) {
  pin = gpioPin;
  pinMode(pin, INPUT_PULLUP);
  rawLast = stable = cur = (digitalRead(pin) == LOW);
  rawChangeMs = stableSinceMs = pressStartMs = millis();
}

void HwButton::sample() {
  bool r = (digitalRead(pin) == LOW);
  uint32_t now = millis();
  portENTER_CRITICAL(&_btnMux);
  if (r != rawLast) {
    rawLast = r;
    rawChangeMs = now;
  } else if (r != stable && (now - rawChangeMs) >= 15) {
    stable = r;
    stableSinceMs = now;
    if (r) pressCnt++;
    else   releaseCnt++;
  }
  portEXIT_CRITICAL(&_btnMux);
}

void HwButton::update() {
  // Bez časovače (nepovedl se založit) se vzorkuje aspoň tady
  if (!_btnTimer) sample();
  portENTER_CRITICAL(&_btnMux);
  cur = stable;
  pressStartMs = stableSinceMs;
  pressedEdge = (pressCnt != seenPress);
  releasedEdge = (releaseCnt != seenRelease);
  seenPress = pressCnt;
  seenRelease = releaseCnt;
  portEXIT_CRITICAL(&_btnMux);
}

HwButton& hwBtnBoot() { return _btnBoot; }
HwButton& hwBtnUser() { return _btnUser; }

bool hwTakePowerKeyShortPress() {
  bool v = _pwrKeyShort;
  _pwrKeyShort = false;
  return v;
}

const HwPowerInfo& hwPower() { return _pwrInfo; }

// ─── Hardwarové hodiny PCF85063 (čas v UTC) ────────────────────────────────
// Dny od 1. 1. 1970 pro gregoriánské datum (algoritmus days_from_civil)
static long _daysFromCivil(int y, unsigned m, unsigned d) {
  y -= (m <= 2) ? 1 : 0;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097L + (long)doe - 719468L;
}

bool hwRtcGetUtc(time_t& t) {
  if (!_rtcOk) return false;
  RTC_DateTime r = _rtc.getDateTime();
  int y = r.getYear();
  unsigned mo = r.getMonth(), d = r.getDay();
  // Po výpadku napájení hodiny začnou od roku 2000, takový čas se nebere
  if (y < 2025 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
  t = (time_t)(_daysFromCivil(y, mo, d) * 86400L + r.getHour() * 3600L + r.getMinute() * 60L + r.getSecond());
  return true;
}

void hwRtcSetUtc(time_t t) {
  if (!_rtcOk) return;
  struct tm u;
  gmtime_r(&t, &u);
  _rtc.setDateTime(RTC_DateTime(u));
}

void hwPowerOff() {
  hwSetDisplayPower(false);
  Serial.flush();
  if (_pmuOk) {
    delay(80);
    _pmu.shutdown();
  }
  // Bez PMU aspoň hluboký spánek bez probuzení
  esp_deep_sleep_start();
}

// ─── Hluboký spánek ────────────────────────────────────────────────────────
// Probuzení: tlačítka BOOT (GPIO 0) a KEY (GPIO 18) jsou aktivní v nule s externími
// pull-upy, INT dotyku (GPIO 11) stáhne řadič CST9220 při dotyku k nule.
// Po probuzení se procesor spouští znovu od setup(); hwWakeCause() řekne proč.
static void _prepareWakePin(gpio_num_t pin) {
  rtc_gpio_init(pin);
  rtc_gpio_set_direction(pin, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_dis(pin);
  rtc_gpio_pullup_en(pin);
}

static bool _touchDown() {
  if (!_touchOk) return false;
  int16_t x[1] = {0}, y[1] = {0};
  return _touch.getPoint(x, y, 1) > 0;
}

void hwDeepSleep(bool wakeByTouch) {
  hwSetDisplayPower(false);

  // Budicí piny jsou úrovňové: kdyby při usínání ležel prst na skle (zakrytí
  // dlaní) nebo bylo stisknuté tlačítko, chip by se probudil hned. Počká se,
  // nejvýš 3 s, až jsou všechny v klidu aspoň 150 ms.
  {
    uint32_t until = millis() + 3000;
    uint32_t quietSince = millis();
    while ((int32_t)(millis() - until) < 0) {
      bool busy = (digitalRead(BTN_BOOT_PIN) == LOW) || (digitalRead(BTN_USER_PIN) == LOW);
      if (wakeByTouch && _touchOk) {
        if (_takeTouchIrq()) busy = true;
        if (_touchDown()) busy = true;
      }
      if (busy) quietSince = millis();
      else if (millis() - quietSince >= 150) break;
      delay(20);
    }
  }

  if (_btnTimer) esp_timer_stop(_btnTimer);
  if (_touchOk) {
    detachInterrupt(digitalPinToInterrupt(TP_INT));
    // Bez probouzení dotykem se řadič uspí, aby nebral proud
    if (!wakeByTouch) _touch.sleep();
  }

#if PANEL_AUDIO
  digitalWrite(PA_ENABLE_PIN, LOW);
  _audioPowerDown();
#endif
#if PANEL_SLEEP_AUDIO_RAIL_OFF
  if (_pmuOk) _pmu.disableALDO1();
#endif
  // Měření AXP2101, která ve spánku nikdo nečte (napětí baterie zůstává kvůli
  // měřiči nabití); po probuzení je hwInit() zapne znovu
  if (_pmuOk) {
    _pmu.disableTemperatureMeasure();
    _pmu.disableVbusVoltageMeasure();
    _pmu.disableSystemVoltageMeasure();
  }

  uint64_t mask = (1ULL << BTN_BOOT_PIN) | (1ULL << BTN_USER_PIN);
  _prepareWakePin((gpio_num_t)BTN_BOOT_PIN);
  _prepareWakePin((gpio_num_t)BTN_USER_PIN);
  if (wakeByTouch && _touchOk) {
    mask |= (1ULL << TP_INT);
    _prepareWakePin((gpio_num_t)TP_INT);
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
    // Vnitřní pull-up na INT drží jen zapnutá doména RTC periferií; s buzením
    // jen přes EXT1 by ji ESP-IDF ve spánku vypnulo. Tlačítka mají pull-upy vnější.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
#endif
  }
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.println("[hw] deep sleep");
  Serial.flush();
  delay(30);
  esp_deep_sleep_start();
}

// ─── Zvuk ──────────────────────────────────────────────────────────────────
#if PANEL_AUDIO
// Vygeneruje jeden tón (nebo ticho) do I2S; blokuje jen audio úlohu
static void _playTone(const ToneReq& t) {
  int totalSamples = (AUDIO_SAMPLE_RATE * (int)t.durMs) / 1000;
  int16_t frameBuf[128 * 2]; // stereo
  float phase = 0.0f;
  float step = (2.0f * 3.14159265f * (float)t.freq) / (float)AUDIO_SAMPLE_RATE;
  float amp = (float)t.amp;

  int writtenSamples = 0;
  while (writtenSamples < totalSamples) {
    int chunk = totalSamples - writtenSamples;
    if (chunk > 128) chunk = 128;
    for (int i = 0; i < chunk; i++) {
      int16_t s = 0;
      if (t.freq) {
        // Měkký náběh a doběh proti lupnutí
        int idx = writtenSamples + i;
        float env = 1.0f;
        if (idx < 64) env = (float)idx / 64.0f;
        else if (totalSamples - idx < 64) env = (float)(totalSamples - idx) / 64.0f;
        s = (int16_t)(sinf(phase) * amp * env);
        phase += step;
        if (phase > 6.2831853f) phase -= 6.2831853f;
      }
      frameBuf[i * 2]     = s;
      frameBuf[i * 2 + 1] = s;
    }
    _i2s.write((const uint8_t*)frameBuf, chunk * 4);
    writtenSamples += chunk;
  }
}

static void _audioTask(void*) {
  ToneReq t;
  int16_t silence[128 * 2];
  memset(silence, 0, sizeof(silence));
  for (;;) {
    // Úloha spí, dokud nepřijde tón
    if (xQueueReceive(_toneQ, &t, portMAX_DELAY) != pdTRUE) continue;
    if (!_i2sStart()) continue;
    // Krátké ticho po rozběhu hodin, teprve pak zesilovač (bez lupnutí)
    _i2s.write((const uint8_t*)silence, sizeof(silence));
    digitalWrite(PA_ENABLE_PIN, HIGH);
    do {
      _playTone(t);
    } while (xQueueReceive(_toneQ, &t, pdMS_TO_TICKS(25)) == pdTRUE);
    // write() se vrací po zkopírování do DMA, ne po přehrání; tichem se buffer dohraje
    for (int i = 0; i < 16; i++) _i2s.write((const uint8_t*)silence, sizeof(silence));
    digitalWrite(PA_ENABLE_PIN, LOW);
    _i2sStop();
  }
}

void hwBeep(uint16_t freqHz, uint16_t durMs, uint16_t amplitude) {
  if (!_audioOk || !_toneQ || durMs == 0) return;
  if (freqHz != 0 && freqHz < 80) return;
  if (durMs > 600) durMs = 600;
  ToneReq t = { freqHz, durMs, amplitude };
  xQueueSend(_toneQ, &t, 0);   // plná fronta = tón se zahodí, smyčka nečeká
}
#else
void hwBeep(uint16_t, uint16_t, uint16_t) {}
#endif
