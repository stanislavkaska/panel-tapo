"""
Náhledy obrazovek Panelu světel (480 × 480, RGB565) bez hardwaru.

Kreslí stejně jako firmware: rozvržení a barvy ze src/main.cpp, primitiva
Arduino_GFX (zaoblené obdélníky, kruhy) převedená 1:1 a písma přímo z dat
v src/smooth_font.cpp včetně míchání 4bitové průhlednosti do RGB565. Výsledek
tak odpovídá displeji pixel po pixelu. Při změně rozvržení v main.cpp upravit
i tento soubor a docs/simulator.js.

    python tools/preview.py   → docs/screens/*.png, docs/screens/prehled.png
                                a docs/smooth_font.js (písma pro simulátor na webu)

Potřebuje Pillow (python -m pip install pillow).
"""

import base64
import json
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "docs" / "screens"
W = H = 480

# ─── Barvy (RGB565 jako ve firmwaru) ────────────────────────────────────────
COL_BG, COL_CARD, COL_CARD_HI, COL_BORDER = 0x0000, 0x0884, 0x1108, 0x19CD
COL_BLUE, COL_BLUE_LT, COL_BLUE_DIM = 0x3C5F, 0x8E3F, 0x4BD7
COL_ON_TRACK, COL_ON_TRACK_D = 0x094B, 0x00C7
COL_OFF_KNOB, COL_OFF_KNOB_D = 0x3A6D, 0x21A9
COL_TEXT, COL_DIM, COL_HOT, COL_RED_BG = 0xFFFF, 0x7C97, 0xFB88, 0x3841

# ─── Rozvržení ──────────────────────────────────────────────────────────────
CARD_X, CARD_W, CARDS_Y0, CARDS_Y1, CARD_GAP = 24, 432, 48, 376, 10
STATUS_Y, STATUS_H = 386, 28
NAV_Y, NAV_H, NAV_W, NAV1_X, NAV2_X = 424, 44, 200, 36, 244
MODAL_X, MODAL_W, MODAL_Y = 24, 432, CARDS_Y0
MODAL_H = CARDS_Y1 - CARDS_Y0
ROW_Y0, ROW_H = 48, 38
# Detail světla (barva a jas)
DET_HEAD_H, DET_SW_W, DET_SW_H = 54, 84, 40
TILE_X0, TILE_Y0 = MODAL_X + 20, MODAL_Y + 64
TILE_W, TILE_H, TILE_GAP, TILE_ROW_GAP = 68, 54, 13, 10
BRI_Y = MODAL_Y + 188
BAR_Y, BAR_H, BAR_W = MODAL_Y + 234, 10, 392
STEP_Y, STEP_H, STEP_W, STEP_GAP = MODAL_Y + 256, 56, 89, 12
BRI_STEP_LBL = ["–10", "–1", "+1", "+10"]
CTL_STEP0, CTL_SWITCH = 10, 14
# Detail zásuvky (časovač): jako CTL_ON_*, CTL_OFF_*, PLUG_* v main.cpp
SCHED_STEP_LBL = ["–1 h", "–5 m", "+5 m", "+1 h"]
CTL_ON_SW, CTL_ON_STEP0, CTL_OFF_SW, CTL_OFF_STEP0 = 20, 21, 25, 26
PLUG_SW_W, PLUG_SW_H = 72, 36
PLUG_ON_Y, PLUG_ON_BTN_Y = MODAL_Y + 62, MODAL_Y + 106
PLUG_DIV_Y = MODAL_Y + 162
PLUG_OFF_Y, PLUG_OFF_BTN_Y = MODAL_Y + 172, MODAL_Y + 216
PLUG_BTN_H = 46

# ─── Základní barvy pásku (odstín, sytost) jako COLOR_PRESETS v main.cpp ────
COLOR_PRESETS = [
    ("bílá", 0, 0), ("teplá bílá", 30, 45), ("červená", 0, 100), ("oranžová", 25, 100),
    ("žlutá", 50, 100), ("zelená", 120, 100), ("tyrkysová", 175, 100), ("modrá", 240, 100),
    ("fialová", 275, 100), ("růžová", 330, 70),
]


def hsv_to_565(hue, sat):
    """Jako hsvTo565() ve firmwaru: barva pásku při plném jasu, celočíselně."""
    h, s = hue % 360, min(sat, 100)
    f = h % 60
    p = 255 * (100 - s) // 100
    q = 255 * (6000 - s * f) // 6000
    t = 255 * (6000 - s * (60 - f)) // 6000
    r, g, b = [(255, t, p), (q, 255, p), (p, 255, t), (p, q, 255), (t, p, 255), (255, p, q)][h // 60]
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def half_color(c):
    return (c >> 1) & 0x7BEF


def preset_index(hue, sat):
    """Základní barva, které odpovídá barva pásku (±2), jinak -1."""
    for k, (_, ph, ps) in enumerate(COLOR_PRESETS):
        if abs(sat - ps) > 2:
            continue
        if ps == 0:
            return k
        dh = abs(hue % 360 - ph)
        if dh > 180:
            dh = 360 - dh
        if dh <= 2:
            return k
    return -1


def rgb(c: int):
    return (((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


# ─── Písma ze src/smooth_font.cpp ───────────────────────────────────────────
class SmoothFont:
    def __init__(self, name, line_height, glyphs, bitmap):
        self.name = name
        self.line_height = line_height
        self.glyphs = glyphs        # kód znaku → (w, h, left, top, advance, offset)
        self.bitmap = bitmap


def _load_fonts():
    src = (REPO / "src" / "smooth_font.cpp").read_text(encoding="utf-8")
    fonts = {}
    for m in re.finditer(r"const SmoothFont (\w+) = \{\s*(\d+), (\d+),", src):
        name, lh = m.group(1), int(m.group(2))
        bm = re.search(r"static const uint8_t " + name + r"_bitmaps\[\d+\] PROGMEM = \{(.*?)\};", src, re.S)
        gl = re.search(r"static const SmoothGlyph " + name + r"_glyphs\[\d+\] PROGMEM = \{(.*?)\};", src, re.S)
        bitmap = bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", bm.group(1)))
        glyphs = {}
        for t in re.findall(r"\{ 0x([0-9A-Fa-f]+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (-?\d+), (\d+) \}", gl.group(1)):
            glyphs[int(t[0], 16)] = tuple(int(v) for v in t[1:])
        fonts[name] = SmoothFont(name, lh, glyphs, bitmap)
    return fonts


FONTS = _load_fonts()
F_STATE, F_T28, F_TITLE, F_BTN = FONTS["fontState32"], FONTS["fontTitle28"], FONTS["fontTitle24"], FONTS["fontButton22"]
F_SUB18, F_MENU, F_BADGE, F_SMALL, F_TINY = FONTS["fontSub18"], FONTS["fontMenu19"], FONTS["fontBadge16"], FONTS["fontSmall16"], FONTS["fontTiny12"]


def text_width(font, s):
    w = 0
    for ch in s:
        g = font.glyphs.get(ord(ch))
        w += g[4] if g else font.line_height // 3
    return w


def fit_text(font, s, max_w):
    """Jako fitText() ve firmwaru: zkrátí text na šířku a přidá „…“."""
    s = s.encode("utf-8")[:47].decode("utf-8", "ignore")
    if text_width(font, s) <= max_w:
        return s
    ell_w = text_width(font, "…")
    while s:
        s = s[:-1]
        if text_width(font, s) + ell_w <= max_w:
            break
    return s.rstrip(" ") + "…"


class Canvas:
    """Framebuffer RGB565 s primitivy Arduino_GFX a písmy smooth_font."""

    def __init__(self):
        self.fb = [COL_BG] * (W * H)

    # ─── Primitiva Arduino_GFX ──────────────────────────────────────────────
    def fill_rect(self, x, y, w, h, c):
        if not w or not h:
            return
        if w < 0:
            x, w = x + w + 1, -w
        if h < 0:
            y, h = y + h + 1, -h
        x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, W), min(y + h, H)
        if x0 >= x1 or y0 >= y1:
            return
        row = [c] * (x1 - x0)
        for yy in range(y0, y1):
            self.fb[yy * W + x0:yy * W + x1] = row

    def fill_screen(self, c):
        self.fb = [c] * (W * H)

    def hline(self, x, y, w, c):
        self.fill_rect(x, y, w, 1, c)

    def vline(self, x, y, h, c):
        self.fill_rect(x, y, 1, h, c)

    def _ellipse_helper(self, x, y, r, corner, c):
        rx2 = ry2 = r * r
        i, xt, yt = -1, 0, r
        s = (ry2 << 1) + rx2 * (1 - (r << 1))
        while True:
            while s < 0:
                xt += 1
                s += ry2 * ((xt << 2) + 2)
            if corner & 1: self.hline(x - xt, y - yt, xt - i, c)
            if corner & 2: self.hline(x + i + 1, y - yt, xt - i, c)
            if corner & 4: self.hline(x + i + 1, y + yt, xt - i, c)
            if corner & 8: self.hline(x - xt, y + yt, xt - i, c)
            i = xt
            yt -= 1
            s -= yt * rx2 << 2
            if not ry2 * xt <= rx2 * yt:
                break
        i, yt, xt = -1, 0, r
        s = (rx2 << 1) + ry2 * (1 - (r << 1))
        while True:
            while s < 0:
                yt += 1
                s += rx2 * ((yt << 2) + 2)
            if corner & 1: self.vline(x - xt, y - yt, yt - i, c)
            if corner & 2: self.vline(x + xt, y - yt, yt - i, c)
            if corner & 4: self.vline(x + xt, y + i + 1, yt - i, c)
            if corner & 8: self.vline(x - xt, y + i + 1, yt - i, c)
            i = yt
            xt -= 1
            s -= xt * ry2 << 2
            if not rx2 * yt <= ry2 * xt:
                break

    def _fill_ellipse_helper(self, x, y, r, corners, delta, c):
        if r <= 0:
            return
        rx2 = ry2 = r * r
        self.hline(x - r, y, (r << 1) + 1, c)
        i, yt, xt = 0, 0, r
        s = (rx2 << 1) + ry2 * (1 - (r << 1))
        while True:
            while s < 0:
                yt += 1
                s += rx2 * ((yt << 2) + 2)
            if corners & 1: self.fill_rect(x - xt, y - yt, (xt << 1) + 1 + delta, yt - i, c)
            if corners & 2: self.fill_rect(x - xt, y + i + 1, (xt << 1) + 1 + delta, yt - i, c)
            i = yt
            xt -= 1
            s -= xt * ry2 << 2
            if not rx2 * yt <= ry2 * xt:
                break
        xt, yt = 0, r
        s = (ry2 << 1) + rx2 * (1 - (r << 1))
        while True:
            while s < 0:
                xt += 1
                s += ry2 * ((xt << 2) + 2)
            if corners & 1: self.hline(x - xt, y - yt, (xt << 1) + 1 + delta, c)
            if corners & 2: self.hline(x - xt, y + yt, (xt << 1) + 1 + delta, c)
            yt -= 1
            s -= yt * rx2 << 2
            if not ry2 * xt <= rx2 * yt:
                break

    def draw_round_rect(self, x, y, w, h, r, c):
        r = min(r, min(w, h) // 2)
        self.hline(x + r, y, w - 2 * r, c)
        self.hline(x + r, y + h - 1, w - 2 * r, c)
        self.vline(x, y + r, h - 2 * r, c)
        self.vline(x + w - 1, y + r, h - 2 * r, c)
        self._ellipse_helper(x + r, y + r, r, 1, c)
        self._ellipse_helper(x + w - r - 1, y + r, r, 2, c)
        self._ellipse_helper(x + w - r - 1, y + h - r - 1, r, 4, c)
        self._ellipse_helper(x + r, y + h - r - 1, r, 8, c)

    def fill_round_rect(self, x, y, w, h, r, c):
        r = min(r, min(w, h) // 2)
        self.fill_rect(x, y + r, w, h - (r << 1), c)
        self._fill_ellipse_helper(x + r, y + r, r, 1, w - 2 * r - 1, c)
        self._fill_ellipse_helper(x + r, y + h - r - 1, r, 2, w - 2 * r - 1, c)

    def fill_circle(self, x, y, r, c):
        self._fill_ellipse_helper(x, y, r, 3, 0, c)

    # ─── Písmo (smoothDrawText) ─────────────────────────────────────────────
    def text(self, font, s, x, y, fg):
        fr, fgg, fb_ = (fg >> 11) & 0x1F, (fg >> 5) & 0x3F, fg & 0x1F
        cx = x
        for ch in s:
            g = font.glyphs.get(ord(ch)) or font.glyphs.get(ord("?"))
            if not g:
                cx += font.line_height // 3
                continue
            gw, gh, left, top, adv, off = g
            bit = 0
            for r in range(gh):
                py = y + top + r
                for col in range(gw):
                    byte = font.bitmap[off + (bit >> 3)]
                    a4 = (byte & 0x0F) if (bit & 4) else (byte >> 4)
                    bit += 4
                    px = cx + left + col
                    if a4 and 0 <= px < W and 0 <= py < H:
                        idx = py * W + px
                        if a4 == 15:
                            self.fb[idx] = fg
                        else:
                            bg = self.fb[idx]
                            a = a4 * 17
                            ia = 255 - a
                            nr = (fr * a + ((bg >> 11) & 0x1F) * ia + 127) // 255
                            ng = (fgg * a + ((bg >> 5) & 0x3F) * ia + 127) // 255
                            nb = (fb_ * a + (bg & 0x1F) * ia + 127) // 255
                            self.fb[idx] = (nr << 11) | (ng << 5) | nb
            cx += adv

    def text_centered(self, font, s, cx, y, fg):
        self.text(font, s, cx - text_width(font, s) // 2, y, fg)

    def text_right(self, font, s, rx, y, fg):
        self.text(font, s, rx - text_width(font, s), y, fg)

    def image(self):
        img = Image.new("RGB", (W, H))
        img.putdata([rgb(c) for c in self.fb])
        return img


# ─── Model obrazovky ────────────────────────────────────────────────────────
class Light:
    """on = None je neznámý stav, stale = stav z minula (šedě), hue a sat = barva pásku
    (bílá = sytost 0), has_color = False u světla bez barvy."""

    def __init__(self, label, on=None, bri=0, stale=False, busy=False, err="", hue=0, sat=0, has_color=True,
                 is_plug=False, on_time=0, sched_on_en=False, sched_on_min=420, sched_off_en=False, sched_off_min=1320):
        self.label, self.on, self.bri, self.stale, self.busy, self.err = label, on, bri, stale, busy, err
        self.hue, self.sat, self.has_color = hue, sat, has_color
        # Zásuvka P100: bez jasu a barvy, s dobou provozu (s) a denním časovačem zap/vyp (minuta dne)
        self.is_plug, self.on_time = is_plug, on_time
        self.sched_on_en, self.sched_on_min = sched_on_en, sched_on_min
        self.sched_off_en, self.sched_off_min = sched_off_en, sched_off_min


def hhmm(m):
    m %= 1440
    return f"{m // 60:02d}:{m % 60:02d}"


def plug_sub(lt, on, compact=False):
    """Jako doplňkový řádek zásuvky v drawCardView(): doba provozu a časovač."""
    run = ""
    if on and lt.on_time > 0:
        h, m = lt.on_time // 3600, (lt.on_time % 3600) // 60
        pfx = "" if (compact and lt.sched_off_en) else "běží "
        run = f"{pfx}{h} h {m} m" if h > 0 else (f"{pfx}{m} min" if m > 0 else f"{pfx}< 1 min")
    if on and lt.sched_off_en:
        if run:
            return f"{run} · {hhmm(lt.sched_off_min)}" if compact else f"{run} · vyp {hhmm(lt.sched_off_min)}"
        return f"vyp v {hhmm(lt.sched_off_min)}"
    if run:
        return run
    if not on and lt.sched_on_en and lt.sched_off_en:
        if compact:
            return f"{hhmm(lt.sched_on_min)}–{hhmm(lt.sched_off_min)}"
        return f"zap {hhmm(lt.sched_on_min)} · vyp {hhmm(lt.sched_off_min)}"
    if not on and lt.sched_on_en:
        return f"zap v {hhmm(lt.sched_on_min)}"
    if not on and lt.sched_off_en:
        return f"vyp v {hhmm(lt.sched_off_min)}"
    return ""


def card_rect(i, n):
    n = max(n, 1)
    h = ((CARDS_Y1 - CARDS_Y0) - CARD_GAP * (n - 1)) // n
    return CARD_X, CARDS_Y0 + i * (h + CARD_GAP), CARD_W, h


def draw_switch(cv, x, y, w, h, on, dim, pressed=False):
    if on:
        track = COL_ON_TRACK_D if dim else COL_ON_TRACK
        border = COL_BORDER if dim else COL_BLUE
        knob = COL_BLUE_DIM if dim else COL_BLUE_LT
    else:
        track = COL_CARD_HI
        border = COL_CARD_HI if dim else COL_BORDER
        knob = COL_OFF_KNOB_D if dim else COL_OFF_KNOB
    if pressed:
        track = 0x1210 if on else 0x198A
    r = h // 2
    cv.fill_round_rect(x, y, w, h, r, track)
    cv.draw_round_rect(x, y, w, h, r, border)
    kd = h - 8
    kx = (x + w - 4 - kd // 2) if on else (x + 4 + kd // 2)
    cv.fill_circle(kx, y + h // 2, kd // 2, knob)
    if on and not dim:
        cv.fill_circle(kx, y + h // 2, kd // 4, 0xDF7F)


def draw_clock(cv, date, hm, left, right):
    """Zrcadlí drawClock() v main.cpp: datum šedomodře, čas bíle, jinak jen čas."""
    wd, ws, wt = text_width(F_BADGE, date), text_width(F_BADGE, " "), text_width(F_BADGE, hm)
    with_date = wd + ws + wt <= right - left
    total = wd + ws + wt if with_date else wt
    if total > right - left:
        return
    x = 240 - total // 2
    if x < left or x + total > right:
        x = left + (right - left - total) // 2
    if with_date:
        cv.text(F_BADGE, date, x, 10, COL_DIM)
        x += wd + ws
    cv.text(F_BADGE, hm, x, 10, COL_TEXT)


def top_bar(cv, mode="connected", ssid="domov", bat=78, charging=False, usb=False, clock=("30.10.2026", "23:34")):
    cv.fill_rect(0, 0, W, 50, COL_BG)
    if mode == "connecting":
        txt, pill, fg = "Wi-Fi··", COL_CARD_HI, COL_BLUE_DIM
    elif mode == "offline":
        txt, pill, fg = "OFFLINE", COL_RED_BG, COL_HOT
    else:
        txt, pill, fg = fit_text(F_BADGE, ssid, 200), COL_ON_TRACK, COL_BLUE_LT
    pw = text_width(F_BADGE, txt) + 24
    cv.fill_round_rect(40, 10, pw, 28, 8, pill)
    cv.draw_round_rect(40, 10, pw, 28, 8, fg)
    cv.text_centered(F_BADGE, txt, 40 + pw // 2, 11, fg)

    bb = "USB" if usb else f"{bat}%"
    pct = 100 if usb else bat
    bat_col = COL_DIM if usb else (COL_BLUE if charging else (COL_HOT if bat <= 20 else COL_DIM))
    cv.text_right(F_BADGE, bb, 398, 10, bat_col)
    cv.draw_round_rect(408, 14, 26, 18, 3, bat_col)
    cv.fill_rect(434, 19, 3, 8, bat_col)
    cv.fill_rect(411, 17, max(2, pct * 20 // 100), 12, bat_col)
    if clock:
        draw_clock(cv, clock[0], clock[1], 40 + pw + 12, 398 - text_width(F_BADGE, bb) - 12)


def short_state_w(font):
    return max(text_width(font, "ZAP"), text_width(font, "VYP"))


def card_split_x(h):
    """Jako cardSplitX(): hranice mezi levou částí karty (detail) a pravou (přepínač)."""
    r = CARD_X + CARD_W
    if h >= 140:
        return r - 22 - 140 - 24
    if h >= 85:
        return r - 18 - 88 - 16 - short_state_w(F_BTN) - 14
    if h >= 65:
        return r - 16 - 76 - 14 - short_state_w(F_BTN) - 12
    return r - 12 - 74 - 12


def draw_color_dot(cv, cx, cy, r, known, on, dim, has_color, hue, sat, bg):
    """Jako drawColorDot(): svítí = plná tečka v barvě pásku, zhasnuto = kroužek."""
    c = hsv_to_565(hue, sat) if has_color else COL_TEXT
    if not known:
        cv.fill_circle(cx, cy, r, COL_BORDER)
        cv.fill_circle(cx, cy, r - 2, bg)
    elif on:
        cv.fill_circle(cx, cy, r, half_color(c) if dim else c)
    else:
        cv.fill_circle(cx, cy, r, half_color(c))
        cv.fill_circle(cx, cy, r - 2, bg)


def draw_card(cv, i, n, lt: Light, pressed=False, selected=False, pressed_sw=False):
    """Zrcadlí drawCardView() v main.cpp (pressed = levá část, pressed_sw = přepínač)."""
    x, y, w, h = card_rect(i, n)
    known = lt.on is not None
    on = bool(known and lt.on)
    dim = lt.stale or lt.busy or not known
    bg = COL_CARD_HI if pressed else COL_CARD
    border = COL_HOT if lt.err else (COL_BLUE if (on and not dim) else COL_BORDER)
    rad = 14 if h >= 60 else 12
    cv.fill_round_rect(x, y, w, h, rad, bg)
    cv.draw_round_rect(x, y, w, h, rad, border)
    if selected and n > 1:
        cv.draw_round_rect(x + 1, y + 1, w - 2, h - 2, rad - 1, COL_BLUE_LT)
        cv.draw_round_rect(x + 2, y + 2, w - 4, h - 4, rad - 2, COL_BLUE_LT)

    name_col = COL_DIM if dim else COL_TEXT
    if not known:
        state_col = COL_DIM
    elif on:
        state_col = COL_BLUE_DIM if dim else COL_BLUE_LT
    else:
        state_col = COL_OFF_KNOB if dim else COL_DIM
    state_txt = "…" if not known else ("ZAPNUTO" if on else "VYPNUTO")
    if lt.err:
        sub, sub_col = lt.err, COL_HOT
    elif lt.is_plug:
        sub, sub_col = plug_sub(lt, on, compact=h < 65), COL_DIM
    elif on and lt.bri:
        sub, sub_col = f"jas {lt.bri} %", COL_DIM
    else:
        sub, sub_col = "", COL_DIM

    def dot(cx, cy, r):
        draw_color_dot(cv, cx, cy, r, known, lt.on, dim, lt.has_color, lt.hue, lt.sat, bg)

    if n == 1:
        name = fit_text(F_T28, lt.label, w - 48 - 26)
        nx = 240 - (text_width(F_T28, name) + 26) // 2
        dot(nx + 8, y + 57, 8)
        cv.text(F_T28, name, nx + 26, y + 36, name_col)
        draw_switch(cv, 240 - 100, y + h // 2 - 60, 200, 90, on, dim, pressed_sw)
        cv.text_centered(F_STATE, state_txt, 240, y + h // 2 + 52, state_col)
        if sub:
            cv.text_centered(F_SUB18, sub, 240, y + h // 2 + 100, sub_col)
    elif h >= 140:
        sw, sh = 140, 64
        sx = x + w - 22 - sw
        split = card_split_x(h)
        nx = x + 48
        text_w = split - 12 - nx
        dot(x + 30, y + 37, 8)
        cv.text(F_T28, fit_text(F_T28, lt.label, text_w), nx, y + 16, name_col)
        cv.text(F_STATE, state_txt, nx, y + 64, state_col)
        if sub:
            cv.text(F_SUB18, fit_text(F_SUB18, sub, text_w), nx, y + 118, sub_col)
        cv.vline(split, y + 14, h - 28, COL_BORDER)
        draw_switch(cv, sx, y + h // 2 - sh // 2, sw, sh, on, dim, pressed_sw)
    elif h >= 85:
        sw, sh = 88, 42
        sx = x + w - 18 - sw
        draw_switch(cv, sx, y + (h - sh) // 2, sw, sh, on, dim, pressed_sw)
        state_r = sx - 16
        ty_state = y + h // 2 - 16
        cv.text_right(F_BTN, "…" if not known else ("ZAP" if on else "VYP"), state_r, ty_state, state_col)
        split = card_split_x(h)
        cv.vline(split, y + 12, h - 24, COL_BORDER)
        nx = x + 44
        text_w = split - 12 - nx
        name = fit_text(F_T28, lt.label, text_w)
        if sub:
            ty1 = y + (16 if h >= 100 else 13)
            ty2 = y + (51 if h >= 100 else 44)
            dot(x + 27, ty1 + 21, 7)
            cv.text(F_T28, name, nx, ty1, name_col)
            cv.text(F_SUB18, fit_text(F_SUB18, sub, text_w), nx, ty2, sub_col)
        else:
            ty = y + h // 2 - 22
            dot(x + 27, ty + 21, 7)
            cv.text(F_T28, name, nx, ty, name_col)
    elif h >= 65:
        sw, sh = 76, 34
        sx = x + w - 16 - sw
        draw_switch(cv, sx, y + (h - sh) // 2, sw, sh, on, dim, pressed_sw)
        state_r = sx - 14
        ty_state = y + h // 2 - 14
        cv.text_right(F_BTN, "…" if not known else ("ZAP" if on else "VYP"), state_r, ty_state, state_col)
        split = card_split_x(h)
        cv.vline(split, y + 10, h - 20, COL_BORDER)
        nx = x + 38
        text_w = split - 10 - nx
        name = fit_text(F_TITLE, lt.label, text_w)
        if sub:
            ty1 = y + 8
            ty2 = y + 36
            dot(x + 24, ty1 + 18, 6)
            cv.text(F_TITLE, name, nx, ty1, name_col)
            cv.text(F_SUB18, fit_text(F_SUB18, sub, text_w), nx, ty2, sub_col)
        else:
            ty = y + h // 2 - 19
            dot(x + 24, ty + 18, 6)
            cv.text(F_TITLE, name, nx, ty, name_col)
    else:
        sw, sh = 74, (34 if h >= 52 else 30)
        sx = x + w - 12 - sw
        draw_switch(cv, sx, y + (h - sh) // 2, sw, sh, on, dim, pressed_sw)
        split = card_split_x(h)
        cv.vline(split, y + 8, h - 16, COL_BORDER)
        nx = x + 34
        text_w = split - 8 - nx
        ty_name = y + h // 2 - 21
        dot(x + 20, y + h // 2, 7 if h >= 52 else 6)
        if sub:
            fits22 = text_width(F_T28, lt.label) + 8 + text_width(F_BTN, sub) <= text_w
            sf = F_BTN if fits22 else F_MENU
            ty_sub = ty_name + (6 if fits22 else 8)
            sub_w = text_width(sf, sub)
            max_sub_w = text_w * 46 // 100
            name_max_w = text_w - (min(sub_w, max_sub_w) + 8)
            name = fit_text(F_T28, lt.label, name_max_w)
            cv.text(F_T28, name, nx, ty_name, name_col)
            rem_w = text_w - text_width(F_T28, name) - 8
            cv.text_right(sf, fit_text(sf, sub, rem_w), split - 8, ty_sub, sub_col)
        else:
            name = fit_text(F_T28, lt.label, text_w)
            cv.text(F_T28, name, nx, ty_name, name_col)


def tile_rect(k):
    return TILE_X0 + (k % 5) * (TILE_W + TILE_GAP), TILE_Y0 + (k // 5) * (TILE_H + TILE_ROW_GAP)


def detail_split_x():
    return MODAL_X + MODAL_W - 20 - DET_SW_W - 14 - short_state_w(F_BTN) - 14


def draw_plug_block(cv, title, en, minutes, row_y, btn_y, sw_ctl, step0_ctl, pressed, dim):
    """Jako drawPlugScheduleBlock(): řádek „Zapnout v / Vypnout v“ s časem a čtyřmi tlačítky."""
    sw_x = MODAL_X + MODAL_W - 20 - PLUG_SW_W
    draw_switch(cv, sw_x, row_y, PLUG_SW_W, PLUG_SW_H, en, dim, pressed == sw_ctl)
    time_col = (COL_BLUE_DIM if dim else COL_BLUE_LT) if en else COL_DIM
    cv.text_right(F_T28, hhmm(minutes), sw_x - 16, row_y - 2, time_col)
    cv.text(F_MENU, title, MODAL_X + 20, row_y + 5, COL_TEXT if en else COL_DIM)
    for b, lbl in enumerate(SCHED_STEP_LBL):
        bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP)
        pr = pressed == step0_ctl + b
        cv.fill_round_rect(bx, btn_y, STEP_W, PLUG_BTN_H, 12, COL_BORDER if pr else COL_CARD_HI)
        cv.draw_round_rect(bx, btn_y, STEP_W, PLUG_BTN_H, 12, COL_BLUE if pr else COL_BORDER)
        cv.text_centered(F_BTN, lbl, bx + STEP_W // 2, btn_y + 8, COL_TEXT)


def draw_detail(cv, lt: Light, pressed=-1):
    """Zrcadlí drawDetailView() v main.cpp: záhlaví s přepínačem, 10 barev, jas."""
    cv.fill_rect(0, 50, W, NAV_Y - 4 - 50, COL_BG)
    cv.fill_round_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_CARD)
    cv.draw_round_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_BLUE)
    cv.draw_round_rect(MODAL_X + 1, MODAL_Y + 1, MODAL_W - 2, MODAL_H - 2, 15, COL_BLUE)
    known = lt.on is not None
    on = bool(known and lt.on)
    dim = lt.stale or lt.busy or not known

    sx = MODAL_X + MODAL_W - 20 - DET_SW_W
    draw_switch(cv, sx, MODAL_Y + 7, DET_SW_W, DET_SW_H, on, dim, pressed == CTL_SWITCH)
    if not known:
        state_col = COL_DIM
    elif on:
        state_col = COL_BLUE_DIM if dim else COL_BLUE_LT
    else:
        state_col = COL_OFF_KNOB if dim else COL_DIM
    cv.text_right(F_BTN, "…" if not known else ("ZAP" if on else "VYP"), sx - 14, MODAL_Y + 11, state_col)
    name = fit_text(F_T28, lt.label, detail_split_x() - 12 - (MODAL_X + 20))
    cv.text(F_T28, name, MODAL_X + 20, MODAL_Y + 6, COL_DIM if dim else COL_TEXT)
    cv.hline(MODAL_X + 16, MODAL_Y + DET_HEAD_H, MODAL_W - 32, COL_BORDER)

    if lt.is_plug:
        draw_plug_block(cv, "Zapnout v", lt.sched_on_en, lt.sched_on_min, PLUG_ON_Y, PLUG_ON_BTN_Y,
                        CTL_ON_SW, CTL_ON_STEP0, pressed, dim)
        cv.hline(MODAL_X + 16, PLUG_DIV_Y, MODAL_W - 32, COL_BORDER)
        draw_plug_block(cv, "Vypnout v", lt.sched_off_en, lt.sched_off_min, PLUG_OFF_Y, PLUG_OFF_BTN_Y,
                        CTL_OFF_SW, CTL_OFF_STEP0, pressed, dim)
        if on and lt.on_time > 0:
            h, m = lt.on_time // 3600, (lt.on_time % 3600) // 60
            if h > 0:
                info = f"V provozu: {h} h {m} min · časovač v zásuvce"
            elif m > 0:
                info = f"V provozu: {m} min · časovač v zásuvce"
            else:
                info = "V provozu: < 1 min · časovač v zásuvce"
        else:
            info = "Časovač běží přímo v zásuvce (denně)"
        cv.text_centered(F_SMALL, info, 240, MODAL_Y + 282, COL_DIM)
        return

    has_color = known and lt.has_color
    if known and not lt.has_color:
        cv.text_centered(F_SUB18, "Toto světlo barvu nemá", 240, TILE_Y0 + 50, COL_DIM)
    else:
        cur = preset_index(lt.hue, lt.sat) if has_color else -1
        for k, (_, ph, ps) in enumerate(COLOR_PRESETS):
            tx, ty = tile_rect(k)
            c = hsv_to_565(ph, ps)
            if k == cur or pressed == k:
                ring = COL_TEXT if k == cur else COL_BLUE_LT
                cv.draw_round_rect(tx, ty, TILE_W, TILE_H, 12, ring)
                cv.draw_round_rect(tx + 1, ty + 1, TILE_W - 2, TILE_H - 2, 11, ring)
                cv.fill_round_rect(tx + 5, ty + 5, TILE_W - 10, TILE_H - 10, 8, c)
            else:
                cv.fill_round_rect(tx, ty, TILE_W, TILE_H, 12, c)

    bri = lt.bri if known else 0
    cv.text(F_MENU, "Jas", MODAL_X + 20, BRI_Y + 14, COL_DIM)
    cv.text_right(F_STATE, f"{bri} %" if bri else "…", MODAL_X + MODAL_W - 20, BRI_Y, COL_TEXT if on else COL_DIM)
    cv.fill_round_rect(MODAL_X + 20, BAR_Y, BAR_W, BAR_H, BAR_H // 2, COL_CARD_HI)
    if bri:
        fw = max(bri * BAR_W // 100, BAR_H)
        fc = (COL_BLUE_DIM if dim else COL_BLUE) if on else COL_OFF_KNOB
        cv.fill_round_rect(MODAL_X + 20, BAR_Y, fw, BAR_H, BAR_H // 2, fc)
    for b, lbl in enumerate(BRI_STEP_LBL):
        bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP)
        pr = pressed == CTL_STEP0 + b
        cv.fill_round_rect(bx, STEP_Y, STEP_W, STEP_H, 12, COL_BORDER if pr else COL_CARD_HI)
        cv.draw_round_rect(bx, STEP_Y, STEP_W, STEP_H, 12, COL_BLUE if pr else COL_BORDER)
        cv.text_centered(F_TITLE, lbl, bx + STEP_W // 2, STEP_Y + 11, COL_TEXT)


def status_line(cv, s, col=COL_DIM):
    cv.fill_rect(0, CARDS_Y1, W, NAV_Y - 4 - CARDS_Y1, COL_BG)
    if s:
        t = fit_text(F_SUB18, s, 432 - 32)
        pw = text_width(F_SUB18, t) + 32
        px = 240 - pw // 2
        bd = COL_BORDER if col == COL_DIM else col
        bg = COL_RED_BG if col == COL_HOT else COL_CARD
        cv.fill_round_rect(px, STATUS_Y, pw, STATUS_H, 8, bg)
        cv.draw_round_rect(px, STATUS_Y, pw, STATUS_H, 8, bd)
        cv.text_centered(F_SUB18, t, 240, STATUS_Y + 1, COL_TEXT if col == COL_DIM else col)


def nav(cv, settings_open=False, demo=False, pressed=-1, detail_open=False):
    cv.fill_rect(0, NAV_Y - 4, W, H - (NAV_Y - 4), COL_BG)
    if demo:
        lbl = ["Další", "Ukončit"]
    else:
        lbl = ["Obnovit", "Zavřít" if settings_open else ("Zpět" if detail_open else "Nastavení")]
    modal = settings_open or detail_open
    for i, bx in enumerate((NAV1_X, NAV2_X)):
        if i == 0 and modal and not demo:
            continue
        active = pressed == i or (i == 1 and (modal or demo))
        bg = COL_CARD_HI if active else COL_CARD
        bd = COL_BLUE if active else COL_BORDER
        fg = COL_TEXT if active else COL_DIM
        cv.fill_round_rect(bx, NAV_Y, NAV_W, NAV_H, 10, bg)
        cv.draw_round_rect(bx, NAV_Y, NAV_W, NAV_H, 10, bd)
        cv.text_centered(F_BTN, lbl[i], bx + NAV_W // 2, NAV_Y + 6, fg)


SET_ITEMS = ["Jas displeje", "Zhasnout po", "Probuzení dotykem", "Zvuk", "Demo režim", "Spánek", "Zpět"]
SET_DESC = [
    "Nižší jas = delší výdrž baterie",
    "Prodleva zhasnutí a uspání na baterii",
    "VYP = probudí jen tlačítka BOOT a KEY",
    "Cvaknutí a tóny při přepnutí",
    "Ukázka všech podob obrazovky",
    "VYPNOUT = zapne jen podržení PWR",
    "Klepnutím zavřeš nastavení",
]


def settings(cv, sel=1, bri=3, sleep=20, wake_touch=True, sound=True, full_off=False):
    cv.fill_rect(0, 50, W, NAV_Y - 4 - 50, COL_BG)
    cv.fill_round_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_CARD)
    cv.draw_round_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL_BLUE)
    cv.draw_round_rect(MODAL_X + 1, MODAL_Y + 1, MODAL_W - 2, MODAL_H - 2, 15, COL_BLUE)
    cv.text_centered(F_TITLE, "NASTAVENÍ", 240, MODAL_Y + 5, COL_BLUE_LT)
    cv.hline(MODAL_X + 16, MODAL_Y + 42, MODAL_W - 32, COL_BORDER)
    rights = [
        (f"{bri}/4", COL_BLUE_LT),
        (f"{sleep} s", COL_BLUE_LT),
        ("ZAP" if wake_touch else "VYP", COL_BLUE if wake_touch else COL_DIM),
        ("ZAP" if sound else "VYP", COL_BLUE if sound else COL_DIM),
        ("SPUSTIT", COL_BLUE_LT),
        ("VYPNOUT" if full_off else "HLUBOKÝ", COL_HOT if full_off else COL_BLUE_LT),
        ("", COL_DIM),
    ]
    for i, name in enumerate(SET_ITEMS):
        ry = MODAL_Y + ROW_Y0 + i * ROW_H
        is_sel = i == sel
        if is_sel:
            cv.fill_round_rect(MODAL_X + 10, ry, MODAL_W - 20, ROW_H - 4, 8, COL_CARD_HI)
        ty = ry + 2
        cv.text(F_MENU, name, MODAL_X + 24, ty, COL_TEXT if is_sel else COL_DIM)
        right, rcol = rights[i]
        if right:
            cv.text_right(F_MENU, right, MODAL_X + MODAL_W - 24, ty, rcol)


def splash(cv, n=2):
    cv.fill_screen(COL_BG)
    cols = [0x094B, 0x11CD, 0x1A6F, 0x2B3F, 0x3C5F, 0x5D1F, 0x8E3F, 0xDF7F]
    x0 = 240 - (8 * 30) // 2
    cv.draw_round_rect(x0 - 10, 95, 8 * 30 + 20, 44, 12, COL_BORDER)
    for i, c in enumerate(cols):
        cv.fill_round_rect(x0 + i * 30 + 3, 104, 24, 26, 6, c)
    cv.text_centered(F_STATE, "PANEL SVĚTEL", 240, 168, COL_TEXT)
    cv.text_centered(F_BTN, "Tapo · Wi-Fi", 240, 224, COL_DIM)
    word = "světlo" if n == 1 else ("světla" if 2 <= n <= 4 else "světel")
    cv.text_centered(F_TITLE, f"{n} {word}", 240, 264, COL_BLUE_LT)
    cv.text_centered(F_SUB18, "klepnutím pokračovat", 240, 415, COL_DIM)


# ─── Scény ──────────────────────────────────────────────────────────────────
def scene_main(lights, mode="connected", status=None, status_col=COL_DIM, bat=78, pressed=-1, selected=-1):
    cv = Canvas()
    top_bar(cv, mode=mode, bat=bat)
    for i, lt in enumerate(lights):
        draw_card(cv, i, len(lights), lt, pressed=(pressed == i), selected=(selected == i))
    if status is None:
        status = "Klepni na jméno: barva a jas · zhasnu za 14 s"
    status_line(cv, status, status_col)
    nav(cv)
    return cv


def scene_detail(lt, pressed=-1, status="Změny platí hned · zhasnu za 14 s", status_col=COL_DIM):
    cv = Canvas()
    top_bar(cv)
    draw_detail(cv, lt, pressed)
    status_line(cv, status, status_col)
    nav(cv, detail_open=True)
    return cv


def scene_settings():
    cv = Canvas()
    top_bar(cv)
    settings(cv, sel=1)
    status_line(cv, SET_DESC[1], COL_BLUE_LT)
    nav(cv, settings_open=True)
    return cv


def scene_splash():
    cv = Canvas()
    splash(cv)
    return cv


TEPLA, FIALOVA, TYRKYS, ZELENA, MODRA = (30, 45), (275, 100), (175, 100), (120, 100), (240, 100)


def P(label, on=None, **kw):
    """Chytrá zásuvka Tapo P100 (bez jasu a barvy)."""
    return Light(label, on, has_color=False, is_plug=True, **kw)


def L(label, on=None, bri=0, color=(0, 0), **kw):
    """Světlo s barvou (odstín, sytost); bez barvy = bílá."""
    return Light(label, on, bri, hue=color[0], sat=color[1], **kw)


# Pořadí = pořadí v přehledu (README) i v galerii na webu
SCENES = [
    ("1_dve_svetla", "Dvě světla, jedno svítí",
     lambda: scene_main([L("Obývák", True, 80, TEPLA), L("Kuchyň", False)])),
    ("10_barva_jas", "Barva a jas: klepnutí na jméno",
     lambda: scene_detail(L("Obývák", True, 60, TEPLA))),
    ("12_jas_po_jedne", "Jas po 1 %, prst na –1",
     lambda: scene_detail(L("Ložnice", True, 4, FIALOVA), pressed=CTL_STEP0 + 1)),
    ("13_zasuvky", "Světlo a dvě zásuvky P100",
     lambda: scene_main([L("Obývák", True, 80, TEPLA),
                         P("LED dvůr", True, on_time=18239, sched_off_en=True, sched_off_min=1320),
                         P("3D tiskárna", False, sched_on_en=True, sched_on_min=420, sched_off_en=True,
                           sched_off_min=1320)])),
    ("14_zasuvka_casovac", "Zásuvka: časovač zapnutí a vypnutí",
     lambda: scene_detail(P("LED dvůr", True, on_time=18239, sched_on_en=True, sched_on_min=1080,
                            sched_off_en=True, sched_off_min=1320))),
    ("15_zasuvka_vypnuta", "Zásuvka vypnutá, prst na +5 m",
     lambda: scene_detail(P("3D tiskárna", False), pressed=CTL_ON_STEP0 + 2)),
    ("2_jedno_svetlo", "Jedno světlo s velkým přepínačem",
     lambda: scene_main([L("LED pásek", True, 100, MODRA)])),
    ("11_tri_svetla", "Tři světla, barva u jména",
     lambda: scene_main([L("Ložnice", True, 35, FIALOVA), L("Dětský pokoj", True, 60, ZELENA),
                         L("Obývák", False, 0, TEPLA)])),
    ("5_ctyri_svetla", "Čtyři světla, výběr tlačítky",
     lambda: scene_main([L("Obývák", True, 80, TEPLA), L("Kuchyň", False), L("Ložnice", True, 35, FIALOVA),
                         L("Chodba", False)], selected=2)),
    ("8_sest_svetel", "Šest světel v řádcích",
     lambda: scene_main([L("Obývák", True, 80, TEPLA), L("Kuchyň", True, 60), L("Ložnice", False, 0, FIALOVA),
                         L("Pracovna", True, 100), L("Koupelna", False, 0, TYRKYS), L("Terasa", False, 0, ZELENA)])),
    ("3_pripojuji", "Po probuzení: stav z minula šedě",
     lambda: scene_main([L("Obývák", True, 80, TEPLA, stale=True), L("Kuchyň", False, stale=True)],
                        mode="connecting", status="Připojuji k Wi-Fi…", status_col=COL_BLUE_LT)),
    ("4_chyba", "Světlo mimo síť",
     lambda: scene_main([L("Obývák", True, 80, TEPLA), L("Kuchyň", False, stale=True, err="není v síti")])),
    ("9_bez_wifi", "Wi-Fi nedostupná",
     lambda: scene_main([L("Obývák", True, 80, TEPLA, stale=True), L("Kuchyň", False, stale=True)],
                        mode="offline", status="Wi-Fi nedostupná · klepni na Obnovit", status_col=COL_HOT,
                        bat=14)),
    ("6_nastaveni", "Nastavení",
     scene_settings),
    ("7_uvod", "Úvodní obrazovka, jen po zapnutí",
     scene_splash),
]


def rounded(img, r=56):
    """Zaoblené rohy jako má panel krabičky (jen pro přehledový list)."""
    mask = Image.new("L", img.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, img.size[0] - 1, img.size[1] - 1], radius=r, fill=255)
    out = Image.new("RGB", img.size, (18, 18, 18))
    out.paste(img, (0, 0), mask)
    return out


def _caption_font():
    """Písmo s diakritikou pro popisky v přehledu (výchozí písmo PIL češtinu neumí)."""
    for path in (REPO / "tools" / "fonts" / "selawk.ttf", "C:/Windows/Fonts/arial.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(str(path), 16)
        except OSError:
            continue
    return ImageFont.load_default()


def export_web_fonts():
    """Písma pro simulátor na webu: stejná data jako firmware, bitmapy v base64."""
    out = {}
    for name, f in FONTS.items():
        glyphs = [[cp, *g] for cp, g in sorted(f.glyphs.items())]
        out[name] = {"lh": f.line_height, "glyphs": glyphs, "bitmap": base64.b64encode(f.bitmap).decode("ascii")}
    js = ("// Písma displeje pro simulátor (docs/simulator.js). Vygenerováno tools/preview.py\n"
          "// ze src/smooth_font.cpp, ručně needitovat. glyphs: [kód, šířka, výška, vlevo,\n"
          "// nahoře, posun, offset], bitmap: 4 bity průhlednosti na pixel (base64).\n"
          "window.PANEL_FONTS = " + json.dumps(out, separators=(",", ":")) + ";\n")
    (REPO / "docs" / "smooth_font.js").write_text(js, encoding="utf-8", newline="\n")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    thumbs = OUT / "thumbs"
    thumbs.mkdir(exist_ok=True)
    rendered = []
    for name, title, make in SCENES:
        img = make().image()
        img.save(OUT / f"{name}.png")
        # Náhled pro web: přesná polovina (240 px) box filtrem, aby jednopixelové
        # linky zůstaly rovnoměrné; prohlížeč ho pak nemusí převzorkovávat
        img.resize((W // 2, H // 2), Image.BOX).save(thumbs / f"{name}.png")
        rendered.append((title, img))

    # Přehledový list: 3 sloupce, zmenšené náhledy s popiskem
    cols, scale, pad, cap = 3, 0.75, 24, 34
    tw = int(W * scale)
    rows = (len(rendered) + cols - 1) // cols
    sheet = Image.new("RGB", (pad + cols * (tw + pad), pad + rows * (tw + cap + pad)), (18, 18, 18))
    d = ImageDraw.Draw(sheet)
    font = _caption_font()
    for idx, (title, img) in enumerate(rendered):
        r, c = divmod(idx, cols)
        x = pad + c * (tw + pad)
        y = pad + r * (tw + cap + pad)
        sheet.paste(rounded(img).resize((tw, tw), Image.LANCZOS), (x, y))
        d.text((x + 4, y + tw + 8), title, fill=(190, 200, 220), font=font)
    sheet.save(OUT / "prehled.png")
    export_web_fonts()
    print(f"Ulozeno {len(rendered)} obrazovek + prehled.png do {OUT} a docs/smooth_font.js")


if __name__ == "__main__":
    main()
