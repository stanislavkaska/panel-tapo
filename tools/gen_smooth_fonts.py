"""
Generates C++ source and header files containing smooth anti-aliased bitmap fonts (4-bit alpha)
for the Panel UI (title, state, small info, and tiny status line/hints).

Písmo: Selawik (Microsoft, SIL Open Font License 1.1) z tools/fonts, náhrada Segoe UI s téměř stejnými
šířkami znaků. Účaří ("baseline") je pevné na pozici, kde bylo u Segoe UI, takže rozvržení
obrazovek zůstává stejné. Spouštět z kořene repozitáře: python tools/gen_smooth_fonts.py
"""

import sys
from pathlib import Path
from PIL import Image, ImageFont

FONTS_DIR = Path(__file__).resolve().parent / "fonts"

# Characters to include
CHARS = [chr(c) for c in range(0x20, 0x7F)]  # ASCII printable
EXTRA = (
    "áéíóúýůčďěňřšťž"
    "ÁÉÍÓÚÝŮČĎĚŇŘŠŤŽ"
    "äöüôÄÖÜÔ"
    "ĺŕľĹŔĽ"
    "°…–—·•%"
)
for c in EXTRA:
    if c not in CHARS:
        CHARS.append(c)

CHARS = sorted(CHARS, key=lambda c: ord(c))

FONT_CONFIGS = [
    {
        "var_name": "fontState32",
        "font_path": FONTS_DIR / "selawkb.ttf",
        "size": 32,
        "baseline": 35,
        "line_height": 36,
        "desc": "Selawik Bold 32px (for ZAPNUTO / VYPNUTO, splash)"
    },
    {
        "var_name": "fontTitle28",
        "font_path": FONTS_DIR / "selawkb.ttf",
        "size": 28,
        "baseline": 31,
        "line_height": 32,
        "desc": "Selawik Bold 28px (for card light names)"
    },
    {
        "var_name": "fontTitle24",
        "font_path": FONTS_DIR / "selawkb.ttf",
        "size": 24,
        "baseline": 26,
        "line_height": 28,
        "desc": "Selawik Bold 24px (for light names / titles)"
    },
    {
        "var_name": "fontButton22",
        "font_path": FONTS_DIR / "selawkb.ttf",
        "size": 22,
        "baseline": 24,
        "line_height": 26,
        "desc": "Selawik Bold 22px (for navigation buttons, ZAP/VYP)"
    },
    {
        "var_name": "fontSub18",
        "font_path": FONTS_DIR / "selawk.ttf",
        "size": 18,
        "baseline": 20,
        "line_height": 22,
        "desc": "Selawik 18px (for brightness and status line)"
    },
    {
        "var_name": "fontMenu19",
        "font_path": FONTS_DIR / "selawk.ttf",
        "size": 19,
        "baseline": 21,
        "line_height": 23,
        "desc": "Selawik 19px (for settings menu items and values)"
    },
    {
        "var_name": "fontBadge16",
        "font_path": FONTS_DIR / "selawkb.ttf",
        "size": 16,
        "baseline": 18,
        "line_height": 20,
        "desc": "Selawik Bold 16px (for top bar Wi-Fi/OFFLINE badge and battery %)"
    },
    {
        "var_name": "fontSmall16",
        "font_path": FONTS_DIR / "selawk.ttf",
        "size": 16,
        "baseline": 18,
        "line_height": 20,
        "desc": "Selawik 16px (for rows, nav buttons, top bar, menu items)"
    },
    {
        "var_name": "fontTiny12",
        "font_path": FONTS_DIR / "selawk.ttf",
        "size": 12,
        "baseline": 13,
        "line_height": 15,
        "desc": "Selawik 12px (for descriptions, hints and errors)"
    }
]

def generate():
    header_content = """#pragma once
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
"""

    cpp_lines = [
        '#include "smooth_font.h"',
        '',
        '// ─── Vyhledání glyfu (binární vyhledávání) ───────────────────────────────────',
        'const SmoothGlyph* smoothFindGlyph(const SmoothFont& font, uint32_t cp) {',
        '  int low = 0, high = (int)font.glyphCount - 1;',
        '  while (low <= high) {',
        '    int mid = (low + high) / 2;',
        '    uint32_t gcp = font.glyphs[mid].unicode;',
        '    if (gcp == cp) return &font.glyphs[mid];',
        '    if (gcp < cp) low = mid + 1;',
        '    else high = mid - 1;',
        '  }',
        '  return nullptr;',
        '}',
        '',
        '// ─── Šířka textu v px ────────────────────────────────────────────────────────',
        'int smoothTextWidth(const SmoothFont& font, const char* text, size_t maxChars) {',
        '  if (!text) return 0;',
        '  int w = 0;',
        '  size_t count = 0;',
        '  const char* p = text;',
        '  while (*p && count < maxChars) {',
        '    uint32_t cp = hwNextUtf8(p);',
        '    if (!cp) break;',
        '    const SmoothGlyph* g = smoothFindGlyph(font, cp);',
        '    if (g) w += g->advance;',
        '    else w += font.lineHeight / 3;',
        '    count++;',
        '  }',
        '  return w;',
        '}',
        '',
        '// ─── Kreslení textu s vyhlazením hran (alpha blend) ──────────────────────────',
        'int smoothDrawText(Arduino_Canvas* cv, const SmoothFont& font, const char* text,',
        '                   int x, int y, uint16_t fg, size_t maxChars) {',
        '  if (!cv || !text) return x;',
        '  uint16_t* fb = cv->getFramebuffer();',
        '  if (!fb) return x;',
        '',
        '  uint32_t fg_r = (fg >> 11) & 0x1F;',
        '  uint32_t fg_g = (fg >> 5) & 0x3F;',
        '  uint32_t fg_b = fg & 0x1F;',
        '',
        '  int cur_x = x;',
        '  size_t drawn = 0;',
        '  const char* p = text;',
        '',
        '  while (*p && drawn < maxChars) {',
        '    uint32_t cp = hwNextUtf8(p);',
        '    if (!cp) break;',
        '',
        '    const SmoothGlyph* g = smoothFindGlyph(font, cp);',
        '    if (!g) g = smoothFindGlyph(font, \'?\');',
        '    if (!g) {',
        '      cur_x += font.lineHeight / 3;',
        '      drawn++;',
        '      continue;',
        '    }',
        '',
        '    if (g->w > 0 && g->h > 0) {',
        '      int bx = cur_x + g->left;',
        '      int by = y + g->top;',
        '      const uint8_t* bmp = font.bitmaps + g->dataOffset;',
        '      uint32_t bitIdx = 0;',
        '',
        '      for (int r = 0; r < g->h; r++) {',
        '        int py = by + r;',
        '        for (int c = 0; c < g->w; c++, bitIdx += 4) {',
        '          int px = bx + c;',
        '          uint8_t byteVal = bmp[bitIdx >> 3];',
        '          uint8_t a4 = (bitIdx & 4) ? (byteVal & 0x0F) : (byteVal >> 4);',
        '',
        '          if (a4 > 0 && px >= 0 && px < LCD_WIDTH && py >= 0 && py < LCD_HEIGHT) {',
        '            if (a4 == 15) {',
        '              fb[py * LCD_WIDTH + px] = fg;',
        '            } else {',
        '              uint16_t bg = fb[py * LCD_WIDTH + px];',
        '              uint32_t a = a4 * 17;',
        '              uint32_t inv_a = 255 - a;',
        '              uint32_t bg_r = (bg >> 11) & 0x1F;',
        '              uint32_t bg_g = (bg >> 5) & 0x3F;',
        '              uint32_t bg_b = bg & 0x1F;',
        '              uint32_t nr = (fg_r * a + bg_r * inv_a + 127) / 255;',
        '              uint32_t ng = (fg_g * a + bg_g * inv_a + 127) / 255;',
        '              uint32_t nb = (fg_b * a + bg_b * inv_a + 127) / 255;',
        '              fb[py * LCD_WIDTH + px] = (uint16_t)((nr << 11) | (ng << 5) | nb);',
        '            }',
        '          }',
        '        }',
        '      }',
        '    }',
        '    cur_x += g->advance;',
        '    drawn++;',
        '  }',
        '  return cur_x;',
        '}',
        '',
        'void smoothDrawTextCentered(Arduino_Canvas* cv, const SmoothFont& font, const char* text,',
        '                            int cx, int y, uint16_t fg) {',
        '  if (!cv || !text) return;',
        '  int w = smoothTextWidth(font, text);',
        '  smoothDrawText(cv, font, text, cx - w / 2, y, fg);',
        '}',
        '',
        'void smoothDrawTextRight(Arduino_Canvas* cv, const SmoothFont& font, const char* text,',
        '                         int rx, int y, uint16_t fg) {',
        '  if (!cv || !text) return;',
        '  int w = smoothTextWidth(font, text);',
        '  smoothDrawText(cv, font, text, rx - w, y, fg);',
        '}',
        ''
    ]

    for cfg in FONT_CONFIGS:
        var_name = cfg["var_name"]
        font_path = cfg["font_path"]
        size = cfg["size"]
        line_height = cfg["line_height"]
        font = ImageFont.truetype(str(font_path), size)

        ascent, descent = font.getmetrics()
        # posun, aby účaří leželo na cfg["baseline"] px od horní hrany řádku
        shift = cfg["baseline"] - ascent
        
        glyphs = []
        bitmap_bytes = bytearray()
        bit_buffer = 0
        bit_count = 0

        for ch in CHARS:
            cp = ord(ch)
            bbox = font.getbbox(ch)
            adv = round(font.getlength(ch))
            mask = font.getmask(ch, mode='L')
            mw, mh = mask.size

            if mw == 0 or mh == 0:
                glyphs.append({
                    "cp": cp, "w": 0, "h": 0, "left": 0, "top": 0,
                    "adv": adv, "offset": len(bitmap_bytes)
                })
                continue

            left, top, right, bottom = bbox
            top += shift

            if bit_count > 0:
                bitmap_bytes.append(bit_buffer << (8 - bit_count))
                bit_buffer = 0
                bit_count = 0
            offset = len(bitmap_bytes)

            for y in range(mh):
                for x in range(mw):
                    val = mask.getpixel((x, y))
                    nib = (val + 8) >> 4
                    if nib > 15: nib = 15
                    bit_buffer = (bit_buffer << 4) | nib
                    bit_count += 4
                    if bit_count == 8:
                        bitmap_bytes.append(bit_buffer)
                        bit_buffer = 0
                        bit_count = 0

            if bit_count > 0:
                bitmap_bytes.append(bit_buffer << (8 - bit_count))
                bit_buffer = 0
                bit_count = 0

            glyphs.append({
                "cp": cp, "w": mw, "h": mh, "left": left, "top": top,
                "adv": adv, "offset": offset
            })

        print(f"Font {var_name}: {len(glyphs)} glyphs, {len(bitmap_bytes)} bytes of bitmap data.")

        cpp_lines.append(f"// ─── {var_name} ({cfg['desc']}) ───")
        cpp_lines.append(f"static const uint8_t {var_name}_bitmaps[{len(bitmap_bytes)}] PROGMEM = {{")
        for i in range(0, len(bitmap_bytes), 16):
            chunk = bitmap_bytes[i:i+16]
            hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
            cpp_lines.append(f"  {hex_str},")
        cpp_lines.append("};")
        cpp_lines.append("")

        cpp_lines.append(f"static const SmoothGlyph {var_name}_glyphs[{len(glyphs)}] PROGMEM = {{")
        for g in glyphs:
            cpp_lines.append(
                f"  {{ 0x{g['cp']:04X}, {g['w']}, {g['h']}, {g['left']}, {g['top']}, {g['adv']}, {g['offset']} }},"
            )
        cpp_lines.append("};")
        cpp_lines.append("")

        cpp_lines.append(f"const SmoothFont {var_name} = {{")
        cpp_lines.append(f"  {line_height}, {len(glyphs)}, {var_name}_glyphs, {var_name}_bitmaps")
        cpp_lines.append("};")
        cpp_lines.append("")

    Path("src/smooth_font.h").write_text(header_content, encoding="utf-8")
    Path("src/smooth_font.cpp").write_text("\n".join(cpp_lines), encoding="utf-8")
    print("Generated src/smooth_font.h and src/smooth_font.cpp successfully.")

if __name__ == "__main__":
    generate()
