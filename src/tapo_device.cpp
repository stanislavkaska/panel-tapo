#include "tapo_device.h"
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <time.h>

// Čas požadavku v ms: po synchronizaci času epocha, jinak aspoň uptime.
// Zařízení hodnotu nekontroluje, posílá se kvůli shodě s aplikací.
static unsigned long long requestTimeMs() {
  time_t t = time(nullptr);
  if (t > 1600000000) return (unsigned long long)t * 1000ULL;
  return (unsigned long long)millis();
}

// Z odpovědi vytáhne error_code; vrací OK nebo ERR_DEVICE / ERR_PROTOCOL
static TapoKlap::Err checkErrorCode(TapoKlap& klap, JsonDocument& doc) {
  if (!doc["error_code"].is<int>()) return TapoKlap::ERR_PROTOCOL;
  int code = doc["error_code"].as<int>();
  if (code != 0) {
    klap.setDeviceError(code);
    return TapoKlap::ERR_DEVICE;
  }
  return TapoKlap::OK;
}

static void decodeNickname(const char* b64, char* out, size_t outLen) {
  out[0] = 0;
  if (!b64 || !*b64) return;
  size_t n = 0;
  unsigned char buf[96];
  if (mbedtls_base64_decode(buf, sizeof(buf) - 1, &n, (const unsigned char*)b64, strlen(b64)) != 0) return;
  buf[n] = 0;
  // Useknutí na hranici znaku UTF-8
  if (n >= outLen) {
    n = outLen - 1;
    while (n > 0 && (buf[n] & 0xC0) == 0x80) n--;
  }
  memcpy(out, buf, n);
  out[n] = 0;
}

// Pravidla časovače zásuvky (get_schedule_rules, první stránka) do doc
static TapoKlap::Err readScheduleRules(TapoKlap& klap, JsonDocument& doc) {
  char req[112];
  snprintf(req, sizeof(req),
           "{\"method\":\"get_schedule_rules\",\"params\":{\"start_index\":0},\"requestTimeMils\":%llu}",
           requestTimeMs());
  String out;
  TapoKlap::Err e = klap.request(req, out);
  if (e != TapoKlap::OK) return e;

  JsonDocument filter;
  filter["error_code"] = true;
  filter["result"]["sum"] = true;
  JsonObject rf = filter["result"]["rule_list"].add<JsonObject>();
  rf["id"] = true;
  rf["enable"] = true;
  rf["s_min"] = true;
  rf["desired_states"]["on"] = true;

  if (deserializeJson(doc, out, DeserializationOption::Filter(filter))) return TapoKlap::ERR_PROTOCOL;
  return checkErrorCode(klap, doc);
}

// Je v zásuvce pravidlo s tímto ID? Když to nejde zjistit (chyba čtení, pravidla
// i na další stránce), bere se, že je: raději chyba než pravidlo navíc.
static bool scheduleRuleExists(TapoKlap& klap, const char* id) {
  JsonDocument doc;
  if (readScheduleRules(klap, doc) != TapoKlap::OK) return true;
  int n = 0;
  JsonArray list = doc["result"]["rule_list"].as<JsonArray>();
  if (!list.isNull()) {
    for (JsonObject rule : list) {
      n++;
      if (strcmp(rule["id"] | "", id) == 0) return true;
    }
  }
  return (doc["result"]["sum"] | n) > n;
}

TapoKlap::Err tapoGetSchedule(TapoKlap& klap, TapoState& st) {
  JsonDocument doc;
  TapoKlap::Err e = readScheduleRules(klap, doc);
  if (e != TapoKlap::OK) return e;

  st.schedOnEn = false;
  st.schedOnId[0] = 0;
  st.schedOffEn = false;
  st.schedOffId[0] = 0;

  JsonArray list = doc["result"]["rule_list"].as<JsonArray>();
  if (!list.isNull()) {
    for (JsonObject rule : list) {
      const char* id = rule["id"] | "";
      if (!id[0]) continue;
      bool wantOn = rule["desired_states"]["on"] | false;
      bool en = rule["enable"] | false;
      uint16_t mins = (uint16_t)((rule["s_min"] | (wantOn ? 420 : 1320)) % 1440);
      if (wantOn) {
        if (!st.schedOnId[0] || (en && !st.schedOnEn)) {
          strncpy(st.schedOnId, id, sizeof(st.schedOnId) - 1);
          st.schedOnId[sizeof(st.schedOnId) - 1] = 0;
          st.schedOnEn = en;
          st.schedOnMin = mins;
        }
      } else {
        if (!st.schedOffId[0] || (en && !st.schedOffEn)) {
          strncpy(st.schedOffId, id, sizeof(st.schedOffId) - 1);
          st.schedOffId[sizeof(st.schedOffId) - 1] = 0;
          st.schedOffEn = en;
          st.schedOffMin = mins;
        }
      }
    }
  }
  st.schedValid = true;
  return TapoKlap::OK;
}

TapoKlap::Err tapoGetInfo(TapoKlap& klap, TapoState& st) {
  char req[96];
  snprintf(req, sizeof(req),
           "{\"method\":\"get_device_info\",\"params\":{},\"requestTimeMils\":%llu}", requestTimeMs());
  String out;
  TapoKlap::Err e = klap.request(req, out);
  if (e != TapoKlap::OK) return e;

  // Filtr: z odpovědi (přes 1 kB) se parsuje jen pár polí
  JsonDocument filter;
  filter["error_code"] = true;
  filter["result"]["device_on"] = true;
  filter["result"]["brightness"] = true;
  filter["result"]["hue"] = true;
  filter["result"]["saturation"] = true;
  filter["result"]["overheated"] = true;
  filter["result"]["on_time"] = true;
  filter["result"]["nickname"] = true;
  filter["result"]["model"] = true;

  JsonDocument doc;
  DeserializationError de = deserializeJson(doc, out, DeserializationOption::Filter(filter));
  if (de) return TapoKlap::ERR_PROTOCOL;
  e = checkErrorCode(klap, doc);
  if (e != TapoKlap::OK) return e;

  JsonObject r = doc["result"];
  if (r.isNull()) return TapoKlap::ERR_PROTOCOL;
  st.on = r["device_on"] | false;
  st.isPlug = !r["brightness"].is<int>();
  st.brightness = (uint8_t)(r["brightness"] | 0);
  // Barvu hlásí jen barevná světla; L900 nemá teplotu bílé, bílá = sytost 0
  st.hasColor = r["hue"].is<int>() && r["saturation"].is<int>();
  st.hue = (uint16_t)((r["hue"] | 0) % 360);
  st.sat = (uint8_t)(r["saturation"] | 0);
  st.overheated = r["overheated"] | false;
  st.onTime = (uint32_t)(r["on_time"] | 0);
  decodeNickname(r["nickname"] | "", st.nickname, sizeof(st.nickname));
  strncpy(st.model, r["model"] | "", sizeof(st.model) - 1);
  st.model[sizeof(st.model) - 1] = 0;
  st.valid = true;

  if (st.isPlug && !st.schedValid) {
    tapoGetSchedule(klap, st);
  }
  return TapoKlap::OK;
}

static TapoKlap::Err setDeviceInfo(TapoKlap& klap, const char* paramsJson) {
  char req[224];
  snprintf(req, sizeof(req),
           "{\"method\":\"set_device_info\",\"params\":%s,\"requestTimeMils\":%llu}",
           paramsJson, requestTimeMs());
  String out;
  TapoKlap::Err e = klap.request(req, out);
  if (e != TapoKlap::OK) return e;
  JsonDocument doc;
  if (deserializeJson(doc, out)) return TapoKlap::ERR_PROTOCOL;
  return checkErrorCode(klap, doc);
}

TapoKlap::Err tapoSetOn(TapoKlap& klap, bool on) {
  return setDeviceInfo(klap, on ? "{\"device_on\":true}" : "{\"device_on\":false}");
}

// Jas i barva s device_on: úprava zhasnutého světla ho rozsvítí. color_temp 0
// přepne barevné žárovky z bílé do barvy (stejně jako set_hsv v python-kasa).
TapoKlap::Err tapoSetLight(TapoKlap& klap, uint8_t bri, int16_t hue, uint8_t sat) {
  char p[112];
  int n = snprintf(p, sizeof(p), "{\"device_on\":true");
  if (bri) {
    if (bri > 100) bri = 100;
    n += snprintf(p + n, sizeof(p) - n, ",\"brightness\":%u", (unsigned)bri);
  }
  if (hue >= 0) {
    if (sat > 100) sat = 100;
    n += snprintf(p + n, sizeof(p) - n, ",\"hue\":%d,\"saturation\":%u,\"color_temp\":0",
                  hue % 360, (unsigned)sat);
  }
  snprintf(p + n, sizeof(p) - n, "}");
  return setDeviceInfo(klap, p);
}

static TapoKlap::Err sendScheduleRule(TapoKlap& klap, bool isEdit, bool wantOn, bool enable,
                                      uint16_t minOfDay, char* ruleId, size_t idCap) {
  minOfDay %= 1440;
  char req[320];
  if (isEdit) {
    snprintf(req, sizeof(req),
             "{\"method\":\"edit_schedule_rule\",\"params\":{\"id\":\"%s\",\"enable\":%s,"
             "\"s_type\":\"normal\",\"s_min\":%u,\"e_type\":\"normal\",\"e_min\":0,"
             "\"e_action\":\"none\",\"week_day\":127,\"mode\":\"repeat\","
             "\"year\":0,\"month\":0,\"day\":0,\"desired_states\":{\"on\":%s},"
             "\"time_offset\":0},\"requestTimeMils\":%llu}",
             ruleId, enable ? "true" : "false", (unsigned)minOfDay,
             wantOn ? "true" : "false", requestTimeMs());
  } else {
    snprintf(req, sizeof(req),
             "{\"method\":\"add_schedule_rule\",\"params\":{\"enable\":%s,"
             "\"s_type\":\"normal\",\"s_min\":%u,\"e_type\":\"normal\",\"e_min\":0,"
             "\"e_action\":\"none\",\"week_day\":127,\"mode\":\"repeat\","
             "\"year\":0,\"month\":0,\"day\":0,\"desired_states\":{\"on\":%s},"
             "\"time_offset\":0},\"requestTimeMils\":%llu}",
             enable ? "true" : "false", (unsigned)minOfDay,
             wantOn ? "true" : "false", requestTimeMs());
  }
  String out;
  TapoKlap::Err e = klap.request(req, out);
  if (e != TapoKlap::OK) return e;
  JsonDocument doc;
  if (deserializeJson(doc, out)) return TapoKlap::ERR_PROTOCOL;
  e = checkErrorCode(klap, doc);
  if (e != TapoKlap::OK) return e;
  if (!isEdit && ruleId && idCap > 0) {
    const char* newId = doc["result"]["id"] | "";
    strncpy(ruleId, newId, idCap - 1);
    ruleId[idCap - 1] = 0;
  }
  return TapoKlap::OK;
}

TapoKlap::Err tapoSetSchedule(TapoKlap& klap, bool wantOn, bool enable, uint16_t minOfDay,
                              char* ruleId, size_t idCap) {
  bool isEdit = (ruleId && ruleId[0] != 0);
  TapoKlap::Err e = sendScheduleRule(klap, isEdit, wantOn, enable, minOfDay, ruleId, idCap);
  if (isEdit && e == TapoKlap::ERR_DEVICE && klap.lastDeviceCode() != -1803) {
    // Pravidlo mohl mezitím někdo smazat v aplikaci. Nové se založí, jen když
    // v zásuvce opravdu chybí; jinak (třeba zásuvka zaneprázdněná) zůstane
    // původní chyba, aby nevznikla dvě pravidla na stejnou věc.
    int code = klap.lastDeviceCode();
    if (!scheduleRuleExists(klap, ruleId)) {
      ruleId[0] = 0;
      e = sendScheduleRule(klap, false, wantOn, enable, minOfDay, ruleId, idCap);
    } else {
      klap.setDeviceError(code);
    }
  }
  return e;
}
