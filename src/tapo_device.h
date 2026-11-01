#pragma once
#include <Arduino.h>
#include "tapo_klap.h"

// Stav světla nebo chytré zásuvky Tapo (L900, P100 apod.)
struct TapoState {
  bool     valid = false;
  bool     on = false;
  bool     isPlug = false;     // zásuvka (P100 apod., nemá jas ani barvu)
  uint8_t  brightness = 0;     // 1..100
  bool     hasColor = false;   // zařízení hlásí barvu (hue, saturation)
  uint16_t hue = 0;            // odstín 0..359°
  uint8_t  sat = 0;            // sytost 0..100 %, 0 = bílá
  bool     overheated = false;
  uint32_t onTime = 0;         // doba běhu v sekundách (on_time u zásuvek)
  bool     schedValid = false; // rozvrh (schedule_rules) byl načten
  bool     schedOnEn = false;  // automatické zapnutí povoleno
  uint16_t schedOnMin = 420;   // čas zapnutí v minutách od půlnoci (výchozí 7:00)
  char     schedOnId[8] = "";  // ID pravidla zapnutí (např. "S1")
  bool     schedOffEn = false; // automatické vypnutí povoleno
  uint16_t schedOffMin = 1320; // čas vypnutí v minutách od půlnoci (výchozí 22:00)
  char     schedOffId[8] = ""; // ID pravidla vypnutí (např. "S2")
  char     nickname[32] = "";  // jméno z aplikace Tapo (UTF-8)
  char     model[16] = "";
};

// Přečte stav zařízení (get_device_info; u zásuvky i get_schedule_rules)
TapoKlap::Err tapoGetInfo(TapoKlap& klap, TapoState& st);
// Přečte rozvrh automatického zapnutí/vypnutí (get_schedule_rules)
TapoKlap::Err tapoGetSchedule(TapoKlap& klap, TapoState& st);
// Zapne/vypne (set_device_info device_on)
TapoKlap::Err tapoSetOn(TapoKlap& klap, bool on);
// Rozsvítí světlo a jedním požadavkem nastaví jas 1..100 (0 = neměnit)
// a barvu: odstín 0..359°, sytost 0..100 % (hue < 0 = neměnit)
TapoKlap::Err tapoSetLight(TapoKlap& klap, uint8_t bri, int16_t hue, uint8_t sat);
// Nastaví nebo přidá denní pravidlo zapnutí/vypnutí v zásuvce (edit/add_schedule_rule)
TapoKlap::Err tapoSetSchedule(TapoKlap& klap, bool wantOn, bool enable, uint16_t minOfDay,
                              char* ruleId, size_t idCap);
