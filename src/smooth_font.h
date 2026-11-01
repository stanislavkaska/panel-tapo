#pragma once
#include <Arduino.h>
#include "hw_waveshare.h"

// Hladká písma s 4-bitovým anti-aliasingem pro všechny prvky displeje.
// Vygenerováno pomocí tools/gen_smooth_fonts.py.

struct SmoothGlyph {
  uint16_t unicode;       // Unicode kód znaku
  uint8_t  w;             // Šířka bitmapy v px
  uint8_t  h;             // Výška bitmapy v px
  int8_t   left;          // Posun X od kurzoru k levému okraji bitmapy
  int8_t   top;           // Posun Y od horního okraje řádku k bitmapě
  uint8_t  advance;       // Posun kurzoru pro další znak
  uint32_t dataOffset;    // Bajtový offset v poli bitmap
};

struct SmoothFont {
  uint8_t            lineHeight;
  uint16_t           glyphCount;
  const SmoothGlyph* glyphs;
  const uint8_t*     bitmaps;
};

extern const SmoothFont fontState32;
extern const SmoothFont fontTitle28;
extern const SmoothFont fontTitle24;
extern const SmoothFont fontButton22;
extern const SmoothFont fontSub18;
extern const SmoothFont fontMenu19;
extern const SmoothFont fontBadge16;
extern const SmoothFont fontSmall16;
extern const SmoothFont fontTiny12;

// Vykreslovací funkce
const SmoothGlyph* smoothFindGlyph(const SmoothFont& font, uint32_t cp);
int  smoothTextWidth(const SmoothFont& font, const char* text, size_t maxChars = 0xFFFF);
int  smoothDrawText(Arduino_Canvas* cv, const SmoothFont& font, const char* text,
                    int x, int y, uint16_t fg, size_t maxChars = 0xFFFF);
void smoothDrawTextCentered(Arduino_Canvas* cv, const SmoothFont& font, const char* text,
                            int cx, int y, uint16_t fg);
void smoothDrawTextRight(Arduino_Canvas* cv, const SmoothFont& font, const char* text,
                         int rx, int y, uint16_t fg);
