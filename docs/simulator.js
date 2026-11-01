/**
 * Panel světel — simulátor displeje v prohlížeči.
 * Kreslí stejně jako firmware: framebuffer RGB565, primitiva Arduino_GFX
 * převedená 1:1 a písma z docs/smooth_font.js (stejná data jako
 * src/smooth_font.cpp). Rozvržení, barvy a chování zrcadlí src/main.cpp:
 * probuzení, připojení k Wi-Fi, načtení stavu, přepnutí klepnutím, detail
 * světla s barvou a jasem, nastavení, ukázka, odpočet, zhasnutí a spánek.
 * Síť je jen zpožděná náhrada, žádný pásek se neovládá.
 */
(function () {
  'use strict';

  const W = 480, H = 480;

  // ─── Barvy (RGB565 jako ve firmwaru) ───────────────────────────────────
  const COL = {
    BG: 0x0000, CARD: 0x0884, CARD_HI: 0x1108, BORDER: 0x19CD,
    BLUE: 0x3C5F, BLUE_LT: 0x8E3F, BLUE_DIM: 0x4BD7,
    ON_TRACK: 0x094B, ON_TRACK_D: 0x00C7, OFF_KNOB: 0x3A6D, OFF_KNOB_D: 0x21A9,
    TEXT: 0xFFFF, DIM: 0x7C97, HOT: 0xFB88, RED_BG: 0x3841,
  };

  // ─── Základní barvy pásku (odstín, sytost) jako COLOR_PRESETS v main.cpp ─
  const COLOR_PRESETS = [
    ['bílá', 0, 0], ['teplá bílá', 30, 45], ['červená', 0, 100], ['oranžová', 25, 100],
    ['žlutá', 50, 100], ['zelená', 120, 100], ['tyrkysová', 175, 100], ['modrá', 240, 100],
    ['fialová', 275, 100], ['růžová', 330, 70],
  ];

  // Jako hsvTo565() ve firmwaru: barva pásku při plném jasu, celočíselně
  function hsvTo565(hue, sat) {
    const h = hue % 360, s = Math.min(sat, 100), f = h % 60;
    const p = Math.floor(255 * (100 - s) / 100);
    const q = Math.floor(255 * (6000 - s * f) / 6000);
    const t = Math.floor(255 * (6000 - s * (60 - f)) / 6000);
    const [r, g, b] = [[255, t, p], [q, 255, p], [p, 255, t], [p, q, 255], [t, p, 255], [255, p, q]][Math.floor(h / 60)];
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  }
  const halfColor = (c) => (c >> 1) & 0x7BEF;

  // Základní barva, které odpovídá barva pásku (±2), jinak -1
  function presetIndex(hue, sat) {
    for (let k = 0; k < COLOR_PRESETS.length; k++) {
      const [, ph, ps] = COLOR_PRESETS[k];
      if (Math.abs(sat - ps) > 2) continue;
      if (ps === 0) return k;
      let dh = Math.abs(hue % 360 - ph);
      if (dh > 180) dh = 360 - dh;
      if (dh <= 2) return k;
    }
    return -1;
  }

  // ─── Rozvržení ─────────────────────────────────────────────────────────
  const CARD_X = 24, CARD_W = 432, CARDS_Y0 = 48, CARDS_Y1 = 376, CARD_GAP = 10;
  const STATUS_Y = 386, STATUS_H = 28;
  const NAV_Y = 424, NAV_H = 44, NAV_W = 200, NAV1_X = 36, NAV2_X = 244;
  const MODAL_X = 24, MODAL_W = 432, MODAL_Y = CARDS_Y0, MODAL_H = CARDS_Y1 - CARDS_Y0;
  const ROW_Y0 = 48, ROW_H = 38;
  // Detail světla (barva a jas)
  const DET_HEAD_H = 54, DET_SW_W = 84, DET_SW_H = 40;
  const TILE_X0 = MODAL_X + 20, TILE_Y0 = MODAL_Y + 64, TILE_W = 68, TILE_H = 54, TILE_GAP = 13, TILE_ROW_GAP = 10;
  const BRI_Y = MODAL_Y + 188, BAR_Y = MODAL_Y + 234, BAR_H = 10, BAR_W = 392;
  const STEP_Y = MODAL_Y + 256, STEP_H = 56, STEP_W = 89, STEP_GAP = 12;
  const BRI_STEPS = [-10, -1, 1, 10], BRI_STEP_LBL = ['–10', '–1', '+1', '+10'];
  const CTL_STEP0 = 10, CTL_SWITCH = 14;   // prvky detailu: 0–9 barvy, 10–13 jas, 14 přepínač
  // Detail zásuvky (časovač): jako CTL_ON_*, CTL_OFF_*, PLUG_* v main.cpp
  const SCHED_STEPS = [-60, -5, 5, 60], SCHED_STEP_LBL = ['–1 h', '–5 m', '+5 m', '+1 h'];
  const CTL_ON_SW = 20, CTL_ON_STEP0 = 21, CTL_OFF_SW = 25, CTL_OFF_STEP0 = 26;
  const PLUG_SW_W = 72, PLUG_SW_H = 36, PLUG_BTN_H = 46;
  const PLUG_ON_Y = MODAL_Y + 62, PLUG_ON_BTN_Y = MODAL_Y + 106, PLUG_DIV_Y = MODAL_Y + 162;
  const PLUG_OFF_Y = MODAL_Y + 172, PLUG_OFF_BTN_Y = MODAL_Y + 216;
  const hhmm = (m) => { m = ((m % 1440) + 1440) % 1440; return String(Math.floor(m / 60)).padStart(2, '0') + ':' + String(m % 60).padStart(2, '0'); };

  // ─── Písma (smooth_font) ───────────────────────────────────────────────
  function loadFont(f) {
    const bin = atob(f.bitmap), bitmap = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bitmap[i] = bin.charCodeAt(i);
    const glyphs = new Map();
    f.glyphs.forEach(g => glyphs.set(g[0], g.slice(1)));   // [w, h, left, top, advance, offset]
    return { lh: f.lh, glyphs, bitmap };
  }
  const FONTS = window.PANEL_FONTS || {};
  const F = {};
  Object.keys(FONTS).forEach(k => { F[k] = loadFont(FONTS[k]); });
  const F_STATE = F.fontState32, F_T28 = F.fontTitle28, F_TITLE = F.fontTitle24, F_BTN = F.fontButton22;
  const F_SUB18 = F.fontSub18, F_MENU = F.fontMenu19, F_BADGE = F.fontBadge16, F_SMALL = F.fontSmall16, F_TINY = F.fontTiny12;

  function textWidth(font, s) {
    let w = 0;
    for (const ch of s) {
      const g = font.glyphs.get(ch.codePointAt(0));
      w += g ? g[4] : Math.floor(font.lh / 3);
    }
    return w;
  }

  // Jako fitText() ve firmwaru: zkrátí text na šířku a přidá „…“
  function fitText(font, s, maxW) {
    let chars = Array.from(s || '');
    if (textWidth(font, chars.join('')) <= maxW) return chars.join('');
    const ellW = textWidth(font, '…');
    while (chars.length) {
      chars.pop();
      if (textWidth(font, chars.join('')) + ellW <= maxW) break;
    }
    return chars.join('').replace(/ +$/, '') + '…';
  }

  // ─── Framebuffer RGB565 s primitivy Arduino_GFX ────────────────────────
  const canvas = document.getElementById('sim-canvas');
  if (!canvas || !F_SMALL) return;
  const ctx = canvas.getContext('2d');
  const img = ctx.createImageData(W, H);
  const px32 = new Uint32Array(img.data.buffer);
  const fb = new Uint16Array(W * H);
  const LUT = new Uint32Array(65536);
  const littleEndian = new Uint8Array(new Uint32Array([1]).buffer)[0] === 1;
  for (let c = 0; c < 65536; c++) {
    const r = ((c >> 11) & 31) * 255 / 31 | 0, g = ((c >> 5) & 63) * 255 / 63 | 0, b = (c & 31) * 255 / 31 | 0;
    LUT[c] = littleEndian ? ((255 << 24) | (b << 16) | (g << 8) | r) >>> 0 : ((r << 24) | (g << 16) | (b << 8) | 255) >>> 0;
  }

  const cv = {
    fillRect(x, y, w, h, c) {
      if (!w || !h) return;
      if (w < 0) { x += w + 1; w = -w; }
      if (h < 0) { y += h + 1; h = -h; }
      const x0 = Math.max(x, 0), y0 = Math.max(y, 0), x1 = Math.min(x + w, W), y1 = Math.min(y + h, H);
      for (let yy = y0; yy < y1; yy++) fb.fill(c, yy * W + x0, yy * W + x1);
    },
    fillScreen(c) { fb.fill(c); },
    hline(x, y, w, c) { this.fillRect(x, y, w, 1, c); },
    vline(x, y, h, c) { this.fillRect(x, y, 1, h, c); },
    _ell(x, y, r, corner, c) {
      const r2 = r * r;
      let i = -1, xt = 0, yt = r, s = (r2 << 1) + r2 * (1 - (r << 1));
      do {
        while (s < 0) s += r2 * ((++xt << 2) + 2);
        if (corner & 1) this.hline(x - xt, y - yt, xt - i, c);
        if (corner & 2) this.hline(x + i + 1, y - yt, xt - i, c);
        if (corner & 4) this.hline(x + i + 1, y + yt, xt - i, c);
        if (corner & 8) this.hline(x - xt, y + yt, xt - i, c);
        i = xt;
        s -= (--yt) * r2 << 2;
      } while (r2 * xt <= r2 * yt);
      i = -1; yt = 0; xt = r; s = (r2 << 1) + r2 * (1 - (r << 1));
      do {
        while (s < 0) s += r2 * ((++yt << 2) + 2);
        if (corner & 1) this.vline(x - xt, y - yt, yt - i, c);
        if (corner & 2) this.vline(x + xt, y - yt, yt - i, c);
        if (corner & 4) this.vline(x + xt, y + i + 1, yt - i, c);
        if (corner & 8) this.vline(x - xt, y + i + 1, yt - i, c);
        i = yt;
        s -= (--xt) * r2 << 2;
      } while (r2 * yt <= r2 * xt);
    },
    _fillEll(x, y, r, corners, delta, c) {
      if (r <= 0) return;
      const r2 = r * r;
      this.hline(x - r, y, (r << 1) + 1, c);
      let i = 0, yt = 0, xt = r, s = (r2 << 1) + r2 * (1 - (r << 1));
      do {
        while (s < 0) s += r2 * ((++yt << 2) + 2);
        if (corners & 1) this.fillRect(x - xt, y - yt, (xt << 1) + 1 + delta, yt - i, c);
        if (corners & 2) this.fillRect(x - xt, y + i + 1, (xt << 1) + 1 + delta, yt - i, c);
        i = yt;
        s -= (--xt) * r2 << 2;
      } while (r2 * yt <= r2 * xt);
      xt = 0; yt = r; s = (r2 << 1) + r2 * (1 - (r << 1));
      do {
        while (s < 0) s += r2 * ((++xt << 2) + 2);
        if (corners & 1) this.hline(x - xt, y - yt, (xt << 1) + 1 + delta, c);
        if (corners & 2) this.hline(x - xt, y + yt, (xt << 1) + 1 + delta, c);
        s -= (--yt) * r2 << 2;
      } while (r2 * xt <= r2 * yt);
    },
    drawRoundRect(x, y, w, h, r, c) {
      r = Math.min(r, Math.floor(Math.min(w, h) / 2));
      this.hline(x + r, y, w - 2 * r, c); this.hline(x + r, y + h - 1, w - 2 * r, c);
      this.vline(x, y + r, h - 2 * r, c); this.vline(x + w - 1, y + r, h - 2 * r, c);
      this._ell(x + r, y + r, r, 1, c); this._ell(x + w - r - 1, y + r, r, 2, c);
      this._ell(x + w - r - 1, y + h - r - 1, r, 4, c); this._ell(x + r, y + h - r - 1, r, 8, c);
    },
    fillRoundRect(x, y, w, h, r, c) {
      r = Math.min(r, Math.floor(Math.min(w, h) / 2));
      this.fillRect(x, y + r, w, h - (r << 1), c);
      this._fillEll(x + r, y + r, r, 1, w - 2 * r - 1, c);
      this._fillEll(x + r, y + h - r - 1, r, 2, w - 2 * r - 1, c);
    },
    fillCircle(x, y, r, c) { this._fillEll(x, y, r, 3, 0, c); },
    // smoothDrawText: 4 bity průhlednosti smíchané do RGB565
    text(font, s, x, y, fg) {
      const fr = (fg >> 11) & 0x1F, fgr = (fg >> 5) & 0x3F, fbl = fg & 0x1F;
      let cx = x;
      for (const ch of s) {
        const g = font.glyphs.get(ch.codePointAt(0)) || font.glyphs.get(63);
        if (!g) { cx += Math.floor(font.lh / 3); continue; }
        const [gw, gh, left, top, adv, off] = g;
        let bit = 0;
        for (let r = 0; r < gh; r++) {
          const py = y + top + r;
          for (let col = 0; col < gw; col++, bit += 4) {
            const byte = font.bitmap[off + (bit >> 3)];
            const a4 = (bit & 4) ? (byte & 0x0F) : (byte >> 4);
            const px = cx + left + col;
            if (a4 && px >= 0 && px < W && py >= 0 && py < H) {
              const idx = py * W + px;
              if (a4 === 15) { fb[idx] = fg; continue; }
              const bg = fb[idx], a = a4 * 17, ia = 255 - a;
              const nr = ((fr * a + ((bg >> 11) & 0x1F) * ia + 127) / 255) | 0;
              const ng = ((fgr * a + ((bg >> 5) & 0x3F) * ia + 127) / 255) | 0;
              const nb = ((fbl * a + (bg & 0x1F) * ia + 127) / 255) | 0;
              fb[idx] = (nr << 11) | (ng << 5) | nb;
            }
          }
        }
        cx += adv;
      }
    },
    textCentered(font, s, cx, y, fg) { this.text(font, s, cx - (textWidth(font, s) >> 1), y, fg); },
    textRight(font, s, rx, y, fg) { this.text(font, s, rx - textWidth(font, s), y, fg); },
    flush() {
      for (let i = 0; i < fb.length; i++) px32[i] = LUT[fb[i]];
      ctx.putImageData(img, 0, 0);
    },
  };

  // ─── Zvuk (ES8311 jako Web Audio) ──────────────────────────────────────
  let audioCtx = null;
  function tone(freq, ms, when = 0) {
    if (!state.settings.sound) return;
    try {
      if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      if (audioCtx.state === 'suspended') audioCtx.resume();
      const t0 = audioCtx.currentTime + when / 1000;
      const osc = audioCtx.createOscillator(), g = audioCtx.createGain();
      osc.type = 'sine'; osc.frequency.value = freq;
      g.gain.setValueAtTime(0.0001, t0);
      g.gain.exponentialRampToValueAtTime(0.18, t0 + 0.008);
      g.gain.exponentialRampToValueAtTime(0.0001, t0 + ms / 1000);
      osc.connect(g); g.connect(audioCtx.destination);
      osc.start(t0); osc.stop(t0 + ms / 1000 + 0.02);
    } catch (e) { /* bez zvuku */ }
  }
  const click = () => tone(2200, 25);
  const melody = (notes) => { let t = 0; notes.forEach(([f, ms]) => { if (f) tone(f, ms, t); t += ms; }); };
  const SND_ON = [[1500, 45], [2000, 70]], SND_OFF = [[2000, 45], [1500, 70]], SND_ERR = [[500, 110], [0, 40], [500, 110]];
  const SND_BOOT = [[1047, 60], [1568, 90]];

  // ─── Ukázka (stejné kroky jako DEMO_STEPS v main.cpp) ──────────────────
  // Světlo: [popisek, zapnuto (1/0/-1 neznámo), jas, neověřené, chyba, odstín, sytost]
  // Zásuvka P100 má navíc: [..., sytost, true, doba provozu (s), čas zap (min), zap povoleno, čas vyp (min), vyp povoleno]
  const DEMO_STEPS = [
    { name: '1 světlo zapnuté', lights: [['LED pásek', 1, 100, false, '', 240, 100]] },
    { name: '1 světlo vypnuté', lights: [['LED pásek', 0, 0, false, '', 240, 100]] },
    { name: '2 světla', lights: [['Obývák', 1, 80, false, '', 30, 45], ['Kuchyň', 0, 0, false, '', 0, 0]] },
    { name: '3 světla', lights: [['Obývák', 1, 70, false, '', 30, 45], ['Kuchyň', 0, 0, false, '', 0, 0], ['Chodba', 1, 100, false, '', 0, 0]] },
    { name: '4 světla, výběr tlačítky', selected: 2,
      lights: [['Obývák', 1, 80, false, '', 30, 45], ['Kuchyň', 0, 0, false, '', 0, 0], ['Ložnice', 0, 0, false, '', 275, 100], ['Pracovna', 1, 45, false, '', 0, 0]] },
    { name: '6 světel', lights: [['Obývák', 1, 80, false, '', 30, 45], ['Kuchyň', 1, 60, false, '', 0, 0], ['Ložnice', 0, 0, false, '', 275, 100],
      ['Pracovna', 1, 100, false, '', 0, 0], ['Koupelna', 0, 0, false, '', 175, 100], ['Terasa', 0, 0, false, '', 120, 100]] },
    { name: 'Barva a jas', detail: true, lights: [['Obývák', 1, 60, false, '', 30, 45]] },
    { name: 'Světlo a zásuvky P100', lights: [['Obývák', 1, 80, false, '', 30, 45],
      ['LED dvůr', 1, 0, false, '', 0, 0, true, 18239, 420, false, 1320, true],
      ['3D tiskárna', 0, 0, false, '', 0, 0, true, 0, 420, true, 1320, true]] },
    { name: 'Zásuvka: časovač zap/vyp', detail: true, lights: [['LED dvůr', 1, 0, false, '', 0, 0, true, 18239, 1080, true, 1320, true]] },
    { name: 'Po probuzení: stav z minula', top: 'connecting', lights: [['Obývák', 1, 80, true, '', 30, 45], ['Kuchyň', 0, 0, true, '', 0, 0]] },
    { name: 'Světlo mimo síť', lights: [['Obývák', 1, 80, false, '', 30, 45], ['Terasa', 0, 0, true, 'není v síti', 120, 100]] },
    { name: 'Wi-Fi nedostupná, slabá baterie', top: 'offline', battery: 14,
      lights: [['Obývák', 1, 80, true, '', 30, 45], ['Kuchyň', 0, 0, true, '', 0, 0]] },
  ];
  const DEMO_STEP_MS = 3500;

  // ─── Stav simulace ─────────────────────────────────────────────────────
  const SLEEP_OPTIONS = [10, 20, 30, 60, 120];
  const SET_ITEMS = ['Jas displeje', 'Zhasnout po', 'Probuzení dotykem', 'Zvuk', 'Demo režim', 'Spánek', 'Zpět'];
  const SET_DESC = [
    'Nižší jas = delší výdrž baterie',
    'Prodleva zhasnutí a uspání na baterii',
    'VYP = probudí jen tlačítka BOOT a KEY',
    'Cvaknutí a tóny při přepnutí',
    'Ukázka všech podob obrazovky',
    'VYPNOUT = zapne jen podržení PWR',
    'Klepnutím zavřeš nastavení',
  ];
  const LIGHT_NAMES = ['Obývák', 'LED dvůr', 'Ložnice', '3D tiskárna', 'Terasa', 'Pracovna'];
  const PLUG_IDX = [1, 3];   // zásuvky P100: bez jasu a barvy, s časovačem
  const LIGHT_COLORS = [[30, 45], [0, 0], [275, 100], [0, 0], [120, 100], [0, 0]];

  // Zhasnutý pásek si jas pamatuje (hlásí ho i vypnutý), proto 100 a ne 0
  function makeLights(n) {
    return LIGHT_NAMES.slice(0, n).map((label, i) => {
      const plug = PLUG_IDX.includes(i);
      return {
        label, on: plug ? i === 1 : i % 2 === 0, bri: plug ? 0 : (i % 2 === 0 ? 80 - i * 10 : 100),
        hue: LIGHT_COLORS[i][0], sat: LIGHT_COLORS[i][1], stale: true, busy: false, err: '',
        hasColor: !plug, isPlug: plug, onBase: plug && i === 1 ? 18239 : 0, onAt: performance.now(),
        schedOnEn: plug && i === 3, schedOnMin: 420, schedOffEn: plug, schedOffMin: 1320,
      };
    });
  }

  // Doba provozu zásuvky v sekundách (od zapnutí; zásuvka ji hlásí sama)
  const plugRun = (lt) => (lt.on === true && !lt.unknown) ? lt.onBase + Math.floor((performance.now() - lt.onAt) / 1000) : 0;

  const state = {
    lights: makeLights(2),
    app: 'splash',          // splash | wifi | query | idle | sleep
    wifiFailed: false,
    wifiFailSim: false,
    lightFailSim: false,
    onUsb: false,
    battery: 78,
    settings: { brightness: 3, sleepSec: 20, wakeTouch: true, sound: true, fullOff: false },
    settingsOpen: false,
    settingsSel: 0,
    detailOpen: false,      // detail světla: barva a jas
    detailIdx: 0,
    edit: null,             // úprava z detailu čekající na odeslání { dev, bri, hue, sat }
    sending: null,          // úprava, která se právě posílá
    demo: false,
    demoStep: 0,
    demoLast: 0,
    sel: 0,
    selVisible: false,
    pendingToggle: -1,
    lastInteract: 0,
    pressedCard: -1,
    pressedRight: false,
    pressedNav: -1,
    pressedCtl: -1,
    timers: [],
  };

  const logEl = document.getElementById('sim-log');
  const logLines = [];
  function log(msg) {
    const t = new Date();
    logLines.push(`${String(t.getHours()).padStart(2, '0')}:${String(t.getMinutes()).padStart(2, '0')}:${String(t.getSeconds()).padStart(2, '0')}  ${msg}`);
    while (logLines.length > 5) logLines.shift();
    if (logEl) logEl.textContent = logLines.join('\n');
  }
  function later(ms, fn) { const id = setTimeout(() => { state.timers = state.timers.filter(t => t !== id); fn(); }, ms); state.timers.push(id); return id; }
  // Zrušené časovače nedokončí ani odesílání úpravy z detailu
  function clearTimers() { state.timers.forEach(clearTimeout); state.timers = []; state.sending = null; }
  const now = () => performance.now();
  function idleLimitMs() {
    if (state.demo) return 300000;   // ukázka běží bez dotyku nejvýš 5 minut
    const s = state.settings.sleepSec;
    return state.onUsb ? Math.max(60000, s * 3000) : s * 1000;
  }
  // Ztlumení displeje na posledních 5 s před zhasnutím (jako firmware)
  let dimmed = false;
  function applyBrightness() {
    const b = 0.55 + state.settings.brightness * 0.1125;
    canvas.style.filter = `brightness(${dimmed ? b * 0.4 : b})`;
  }
  function remainingSec() { const lim = idleLimitMs(), idle = now() - state.lastInteract; return idle >= lim ? 0 : Math.ceil((lim - idle) / 1000); }
  function touch() { state.lastInteract = now(); }
  const failing = (i) => state.lightFailSim && i === state.lights.length - 1 && state.lights.length > 1;

  // ─── Kreslení (zrcadlí main.cpp) ───────────────────────────────────────
  function cardRectN(i, n) {
    n = Math.max(n, 1);
    const h = Math.floor(((CARDS_Y1 - CARDS_Y0) - CARD_GAP * (n - 1)) / n);
    return { x: CARD_X, y: CARDS_Y0 + i * (h + CARD_GAP), w: CARD_W, h };
  }

  function drawSwitch(x, y, w, h, on, dim, pressed) {
    let track, border, knob;
    if (on) { track = dim ? COL.ON_TRACK_D : COL.ON_TRACK; border = dim ? COL.BORDER : COL.BLUE; knob = dim ? COL.BLUE_DIM : COL.BLUE_LT; }
    else { track = COL.CARD_HI; border = dim ? COL.CARD_HI : COL.BORDER; knob = dim ? COL.OFF_KNOB_D : COL.OFF_KNOB; }
    if (pressed) track = on ? 0x1210 : 0x198A;
    const r = h >> 1;
    cv.fillRoundRect(x, y, w, h, r, track);
    cv.drawRoundRect(x, y, w, h, r, border);
    const kd = h - 8, kx = on ? (x + w - 4 - (kd >> 1)) : (x + 4 + (kd >> 1));
    cv.fillCircle(kx, y + (h >> 1), kd >> 1, knob);
    if (on && !dim) cv.fillCircle(kx, y + (h >> 1), kd >> 2, 0xDF7F);
  }

  // Jako drawClock() ve firmwaru: datum šedomodře, čas bíle, když se nevejde, jen čas
  function drawClock(date, hm, left, right) {
    const wd = textWidth(F_BADGE, date), ws = textWidth(F_BADGE, ' '), wt = textWidth(F_BADGE, hm);
    const withDate = wd + ws + wt <= right - left;
    const total = withDate ? wd + ws + wt : wt;
    if (total > right - left) return;
    let x = 240 - (total >> 1);
    if (x < left || x + total > right) x = left + ((right - left - total) >> 1);
    if (withDate) { cv.text(F_BADGE, date, x, 10, COL.DIM); x += wd + ws; }
    cv.text(F_BADGE, hm, x, 10, COL.TEXT);
  }

  function drawTopBar() {
    cv.fillRect(0, 0, W, 50, COL.BG);
    const step = state.demo ? DEMO_STEPS[state.demoStep] : null;
    let mode;
    if (step) mode = step.top || 'connected';
    else if (state.app === 'wifi') mode = 'connecting';
    else if (state.wifiFailed) mode = 'offline';
    else mode = 'connected';
    let txt, pill, fg;
    if (mode === 'connecting') { txt = ['Wi-Fi', 'Wi-Fi·', 'Wi-Fi··', 'Wi-Fi···'][Math.floor(now() / 300) % 4]; pill = COL.CARD_HI; fg = COL.BLUE_DIM; }
    else if (mode === 'offline') { txt = 'OFFLINE'; pill = COL.RED_BG; fg = COL.HOT; }
    else { txt = fitText(F_BADGE, 'domov', 200); pill = COL.ON_TRACK; fg = COL.BLUE_LT; }
    const pw = textWidth(F_BADGE, txt) + 24;
    cv.fillRoundRect(40, 10, pw, 28, 8, pill); cv.drawRoundRect(40, 10, pw, 28, 8, fg);
    cv.textCentered(F_BADGE, txt, 40 + (pw >> 1), 11, fg);

    // Baterie: na USB se nabíjí (modře), jinak šedě, pod 20 % oranžově
    let pct, batCol;
    if (step && step.battery !== undefined) { pct = step.battery; batCol = pct <= 20 ? COL.HOT : COL.DIM; }
    else { pct = state.battery; batCol = state.onUsb ? COL.BLUE : (pct <= 20 ? COL.HOT : COL.DIM); }
    cv.textRight(F_BADGE, `${pct}%`, 398, 10, batCol);
    cv.drawRoundRect(408, 14, 26, 18, 3, batCol); cv.fillRect(434, 19, 3, 8, batCol);
    cv.fillRect(411, 17, Math.max(2, Math.floor(pct * 20 / 100)), 12, batCol);

    // Datum a čas mezi štítkem Wi-Fi a baterií (čas prohlížeče)
    const d = new Date();
    drawClock(`${d.getDate()}.${d.getMonth() + 1}.${d.getFullYear()}`, `${d.getHours()}:${String(d.getMinutes()).padStart(2, '0')}`,
      40 + pw + 12, 398 - textWidth(F_BADGE, `${pct}%`) - 12);
  }

  const shortStateW = (font) => Math.max(textWidth(font, 'ZAP'), textWidth(font, 'VYP'));

  // Hranice mezi levou částí karty (jméno a jas → detail) a pravou (stav a přepínač → přepne)
  function cardSplitX(h) {
    const R = CARD_X + CARD_W;
    if (h >= 140) return R - 22 - 140 - 24;
    if (h >= 85) return R - 18 - 88 - 16 - shortStateW(F_BTN) - 14;
    if (h >= 65) return R - 16 - 76 - 14 - shortStateW(F_BTN) - 12;
    return R - 12 - 74 - 12;
  }

  // Část karty, která světlo přepíná; u jediného světla jen okolí velkého přepínače
  function cardToggleRect(i, n) {
    const c = cardRectN(i, n);
    if (n <= 1) return { x: 240 - 100 - 16, y: c.y + (c.h >> 1) - 60 - 16, w: 200 + 32, h: 90 + 32 };
    const x = cardSplitX(c.h);
    return { x, y: c.y, w: c.x + c.w - x, h: c.h };
  }

  // Tečka před jménem: svítí = plná v barvě pásku, zhasnuto = kroužek, neznámo = šedý kroužek
  function drawColorDot(cx, cy, r, v, bg) {
    const c = v.hasColor ? hsvTo565(v.hue, v.sat) : COL.TEXT;
    if (!v.known) { cv.fillCircle(cx, cy, r, COL.BORDER); cv.fillCircle(cx, cy, r - 2, bg); }
    else if (v.on) cv.fillCircle(cx, cy, r, v.dim ? halfColor(c) : c);
    else { cv.fillCircle(cx, cy, r, halfColor(c)); cv.fillCircle(cx, cy, r - 2, bg); }
  }

  // v = { label, known, on, dim, bri, err, selected, pressed, pressedSw, hasColor, hue, sat }
  function drawCardView(i, n, v) {
    const { x, y, w, h } = cardRectN(i, n);
    const on = v.known && v.on, hasErr = !!v.err;
    const bg = v.pressed ? COL.CARD_HI : COL.CARD;
    const border = hasErr ? COL.HOT : ((on && !v.dim) ? COL.BLUE : COL.BORDER);
    const rad = h >= 60 ? 14 : 12;
    cv.fillRoundRect(x, y, w, h, rad, bg); cv.drawRoundRect(x, y, w, h, rad, border);
    if (v.selected && n > 1) {
      cv.drawRoundRect(x + 1, y + 1, w - 2, h - 2, rad - 1, COL.BLUE_LT);
      cv.drawRoundRect(x + 2, y + 2, w - 4, h - 4, rad - 2, COL.BLUE_LT);
    }
    const nameCol = v.dim ? COL.DIM : COL.TEXT;
    const stateCol = !v.known ? COL.DIM : (on ? (v.dim ? COL.BLUE_DIM : COL.BLUE_LT) : (v.dim ? COL.OFF_KNOB : COL.DIM));
    const stateTxt = !v.known ? '…' : (on ? 'ZAPNUTO' : 'VYPNUTO');
    let sub = '', subCol = COL.DIM;
    if (hasErr) { sub = v.err; subCol = COL.HOT; }
    else if (v.isPlug) {
      // Doba provozu a časovač zásuvky (jako ve firmwaru)
      const compact = h < 65;
      let run = '';
      if (on && v.onTime > 0) {
        const rh = Math.floor(v.onTime / 3600), rm = Math.floor((v.onTime % 3600) / 60);
        const pfx = compact && v.schedOffEn ? '' : 'běží ';
        run = rh > 0 ? `${pfx}${rh} h ${rm} m` : (rm > 0 ? `${pfx}${rm} min` : `${pfx}< 1 min`);
      }
      if (on && v.schedOffEn) sub = run ? (compact ? `${run} · ${hhmm(v.schedOffMin)}` : `${run} · vyp ${hhmm(v.schedOffMin)}`) : `vyp v ${hhmm(v.schedOffMin)}`;
      else if (run) sub = run;
      else if (!on && v.schedOnEn && v.schedOffEn) sub = compact ? `${hhmm(v.schedOnMin)}–${hhmm(v.schedOffMin)}` : `zap ${hhmm(v.schedOnMin)} · vyp ${hhmm(v.schedOffMin)}`;
      else if (!on && v.schedOnEn) sub = `zap v ${hhmm(v.schedOnMin)}`;
      else if (!on && v.schedOffEn) sub = `vyp v ${hhmm(v.schedOffMin)}`;
    } else if (on && v.bri) sub = `jas ${v.bri} %`;

    const shortTxt = !v.known ? '…' : (on ? 'ZAP' : 'VYP');
    const dot = (cx, cy, r) => drawColorDot(cx, cy, r, v, bg);

    if (n === 1) {
      // Jedno světlo: tečka a jméno nahoře, velký přepínač uprostřed, stav a jas pod ním
      const name = fitText(F_T28, v.label, w - 48 - 26);
      const nx = 240 - ((textWidth(F_T28, name) + 26) >> 1);
      dot(nx + 8, y + 57, 8);
      cv.text(F_T28, name, nx + 26, y + 36, nameCol);
      drawSwitch(240 - 100, y + (h >> 1) - 60, 200, 90, on, v.dim, v.pressedSw);
      cv.textCentered(F_STATE, stateTxt, 240, y + (h >> 1) + 52, stateCol);
      if (sub) cv.textCentered(F_SUB18, sub, 240, y + (h >> 1) + 100, subCol);
    } else if (h >= 140) {
      // Dvě světla: tečka, jméno, stav a jas vlevo, přepínač vpravo za čarou
      const sw = 140, sh = 64, sx = x + w - 22 - sw;
      const split = cardSplitX(h), nx = x + 48, textW = split - 12 - nx;
      dot(x + 30, y + 37, 8);
      cv.text(F_T28, fitText(F_T28, v.label, textW), nx, y + 16, nameCol);
      cv.text(F_STATE, stateTxt, nx, y + 64, stateCol);
      if (sub) cv.text(F_SUB18, fitText(F_SUB18, sub, textW), nx, y + 118, subCol);
      cv.vline(split, y + 14, h - 28, COL.BORDER);
      drawSwitch(sx, y + (h >> 1) - (sh >> 1), sw, sh, on, v.dim, v.pressedSw);
    } else if (h >= 85) {
      // Tři světla: velký řádek, název 28 px, jas nebo chyba pod ním
      const sw = 88, sh = 42, sx = x + w - 18 - sw;
      drawSwitch(sx, y + ((h - sh) >> 1), sw, sh, on, v.dim, v.pressedSw);
      const stateR = sx - 16;
      cv.textRight(F_BTN, shortTxt, stateR, y + (h >> 1) - 16, stateCol);
      const split = cardSplitX(h);
      cv.vline(split, y + 12, h - 24, COL.BORDER);
      const nx = x + 44, textW = split - 12 - nx;
      const name = fitText(F_T28, v.label, textW);
      if (sub) {
        const ty1 = y + (h >= 100 ? 16 : 13);
        dot(x + 27, ty1 + 21, 7);
        cv.text(F_T28, name, nx, ty1, nameCol);
        cv.text(F_SUB18, fitText(F_SUB18, sub, textW), nx, y + (h >= 100 ? 51 : 44), subCol);
      } else {
        const ty = y + (h >> 1) - 22;
        dot(x + 27, ty + 21, 7);
        cv.text(F_T28, name, nx, ty, nameCol);
      }
    } else if (h >= 65) {
      // Čtyři světla
      const sw = 76, sh = 34, sx = x + w - 16 - sw;
      drawSwitch(sx, y + ((h - sh) >> 1), sw, sh, on, v.dim, v.pressedSw);
      const stateR = sx - 14;
      cv.textRight(F_BTN, shortTxt, stateR, y + (h >> 1) - 14, stateCol);
      const split = cardSplitX(h);
      cv.vline(split, y + 10, h - 20, COL.BORDER);
      const nx = x + 38, textW = split - 10 - nx;
      const name = fitText(F_TITLE, v.label, textW);
      if (sub) {
        dot(x + 24, y + 8 + 18, 6);
        cv.text(F_TITLE, name, nx, y + 8, nameCol);
        cv.text(F_SUB18, fitText(F_SUB18, sub, textW), nx, y + 36, subCol);
      } else {
        const ty = y + (h >> 1) - 19;
        dot(x + 24, ty + 18, 6);
        cv.text(F_TITLE, name, nx, ty, nameCol);
      }
    } else {
      // Pět až šest světel: bez textu ZAP/VYP (stačí grafický přepínač), velké jméno 28 px a popis 22 px vedle sebe
      const sw = 74, sh = h >= 52 ? 34 : 30, sx = x + w - 12 - sw;
      drawSwitch(sx, y + ((h - sh) >> 1), sw, sh, on, v.dim, v.pressedSw);
      const split = cardSplitX(h);
      cv.vline(split, y + 8, h - 16, COL.BORDER);
      const nx = x + 34, textW = split - 8 - nx, tyName = y + (h >> 1) - 21;
      dot(x + 20, y + (h >> 1), h >= 52 ? 7 : 6);
      if (sub) {
        const fits22 = textWidth(F_T28, v.label) + 8 + textWidth(F_BTN, sub) <= textW;
        const sf = fits22 ? F_BTN : F_MENU, tySub = tyName + (fits22 ? 6 : 8);
        const subW = textWidth(sf, sub), maxSubW = Math.floor(textW * 0.46);
        const name = fitText(F_T28, v.label, textW - (Math.min(subW, maxSubW) + 8));
        cv.text(F_T28, name, nx, tyName, nameCol);
        const remW = textW - textWidth(F_T28, name) - 8;
        cv.textRight(sf, fitText(sf, sub, remW), split - 8, tySub, subCol);
      } else {
        cv.text(F_T28, fitText(F_T28, v.label, textW), nx, tyName, nameCol);
      }
    }
  }

  // lt.unknown = po zapnutí krabička stav ještě nezná (RTC paměť vypnutí nepřežije)
  function drawCards() {
    const n = state.lights.length;
    state.lights.forEach((lt, i) => drawCardView(i, n, {
      label: lt.label, known: lt.on !== null && !lt.unknown, on: !!lt.on, dim: lt.stale || lt.busy || lt.on === null || !!lt.unknown,
      bri: lt.bri, err: lt.err, selected: state.selVisible && state.sel === i,
      pressed: state.pressedCard === i && !state.pressedRight, pressedSw: state.pressedCard === i && state.pressedRight,
      hasColor: lt.hasColor !== false, hue: lt.hue || 0, sat: lt.sat || 0,
      ...plugView(i, lt),
    }));
  }

  // Zásuvka: doba provozu a časovač; čekající nebo odesílaná úprava časovače se ukáže hned
  function plugView(i, lt) {
    if (!lt.isPlug) return {};
    const o = { isPlug: true, onTime: plugRun(lt), schedOnEn: lt.schedOnEn, schedOnMin: lt.schedOnMin, schedOffEn: lt.schedOffEn, schedOffMin: lt.schedOffMin };
    [state.sending, state.edit].forEach(e => {
      if (!e || e.dev !== i) return;
      if (e.schedOn) { o.schedOnEn = e.schedOnEn; o.schedOnMin = e.schedOnMin; }
      if (e.schedOff) { o.schedOffEn = e.schedOffEn; o.schedOffMin = e.schedOffMin; }
    });
    return o;
  }

  function drawDemoCards() {
    const s = DEMO_STEPS[state.demoStep], n = s.lights.length;
    s.lights.forEach(([label, on, bri, dim, err, hue, sat, plug, onTime, onMin, onEn, offMin, offEn], i) => drawCardView(i, n, {
      label, known: on >= 0, on: on > 0, dim: dim || on < 0, bri, err, selected: s.selected === i,
      pressed: false, pressedSw: false, hasColor: !plug, hue, sat,
      isPlug: !!plug, onTime: onTime || 0, schedOnEn: !!onEn, schedOnMin: onMin || 420, schedOffEn: !!offEn, schedOffMin: offMin || 1320,
    }));
  }

  // ─── Detail světla: základní barvy a jas ───────────────────────────────
  const tileRect = (k) => ({ x: TILE_X0 + (k % 5) * (TILE_W + TILE_GAP), y: TILE_Y0 + Math.floor(k / 5) * (TILE_H + TILE_ROW_GAP) });
  const detailSplitX = () => MODAL_X + MODAL_W - 20 - DET_SW_W - 14 - shortStateW(F_BTN) - 14;

  // Řádek „Zapnout v / Vypnout v“ zásuvky: popisek, čas, přepínač a čtyři tlačítka posunu
  function drawPlugBlock(title, en, minutes, rowY, btnY, swCtl, step0, pressed, dim) {
    const swX = MODAL_X + MODAL_W - 20 - PLUG_SW_W;
    drawSwitch(swX, rowY, PLUG_SW_W, PLUG_SW_H, en, dim, pressed === swCtl);
    cv.textRight(F_T28, hhmm(minutes), swX - 16, rowY - 2, en ? (dim ? COL.BLUE_DIM : COL.BLUE_LT) : COL.DIM);
    cv.text(F_MENU, title, MODAL_X + 20, rowY + 5, en ? COL.TEXT : COL.DIM);
    SCHED_STEP_LBL.forEach((lbl, b) => {
      const bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP), pr = pressed === step0 + b;
      cv.fillRoundRect(bx, btnY, STEP_W, PLUG_BTN_H, 12, pr ? COL.BORDER : COL.CARD_HI);
      cv.drawRoundRect(bx, btnY, STEP_W, PLUG_BTN_H, 12, pr ? COL.BLUE : COL.BORDER);
      cv.textCentered(F_BTN, lbl, bx + (STEP_W >> 1), btnY + 8, COL.TEXT);
    });
  }

  // v = { label, known, on, dim, bri (0 = neznámý), hasColor, colorless, hue, sat, pressed,
  //       isPlug, onTime, schedOnEn, schedOnMin, schedOffEn, schedOffMin }
  function drawDetailView(v) {
    cv.fillRect(0, 50, W, NAV_Y - 4 - 50, COL.BG);
    cv.fillRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL.CARD);
    cv.drawRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL.BLUE);
    cv.drawRoundRect(MODAL_X + 1, MODAL_Y + 1, MODAL_W - 2, MODAL_H - 2, 15, COL.BLUE);
    const on = v.known && v.on;

    // Záhlaví: jméno vlevo, stav a přepínač vpravo (klepnutí přepne)
    const sx = MODAL_X + MODAL_W - 20 - DET_SW_W;
    drawSwitch(sx, MODAL_Y + 7, DET_SW_W, DET_SW_H, on, v.dim, v.pressed === CTL_SWITCH);
    const stateCol = !v.known ? COL.DIM : (on ? (v.dim ? COL.BLUE_DIM : COL.BLUE_LT) : (v.dim ? COL.OFF_KNOB : COL.DIM));
    cv.textRight(F_BTN, !v.known ? '…' : (on ? 'ZAP' : 'VYP'), sx - 14, MODAL_Y + 11, stateCol);
    cv.text(F_T28, fitText(F_T28, v.label, detailSplitX() - 12 - (MODAL_X + 20)), MODAL_X + 20, MODAL_Y + 6, v.dim ? COL.DIM : COL.TEXT);
    cv.hline(MODAL_X + 16, MODAL_Y + DET_HEAD_H, MODAL_W - 32, COL.BORDER);

    if (v.isPlug) {
      drawPlugBlock('Zapnout v', v.schedOnEn, v.schedOnMin, PLUG_ON_Y, PLUG_ON_BTN_Y, CTL_ON_SW, CTL_ON_STEP0, v.pressed, v.dim);
      cv.hline(MODAL_X + 16, PLUG_DIV_Y, MODAL_W - 32, COL.BORDER);
      drawPlugBlock('Vypnout v', v.schedOffEn, v.schedOffMin, PLUG_OFF_Y, PLUG_OFF_BTN_Y, CTL_OFF_SW, CTL_OFF_STEP0, v.pressed, v.dim);
      let info = 'Časovač běží přímo v zásuvce (denně)';
      if (on && v.onTime > 0) {
        const h = Math.floor(v.onTime / 3600), m = Math.floor((v.onTime % 3600) / 60);
        info = `V provozu: ${h > 0 ? h + ' h ' + m + ' min' : (m > 0 ? m + ' min' : '< 1 min')} · časovač v zásuvce`;
      }
      cv.textCentered(F_SMALL, info, 240, MODAL_Y + 282, COL.DIM);
      return;
    }

    // Základní barvy: aktuální má bílý rámeček, dlaždice pod prstem modrý
    if (v.colorless) {
      cv.textCentered(F_SUB18, 'Toto světlo barvu nemá', 240, TILE_Y0 + 50, COL.DIM);
    } else {
      const cur = v.hasColor ? presetIndex(v.hue, v.sat) : -1;
      COLOR_PRESETS.forEach(([, ph, ps], k) => {
        const { x: tx, y: ty } = tileRect(k), c = hsvTo565(ph, ps);
        if (k === cur || v.pressed === k) {
          const ring = k === cur ? COL.TEXT : COL.BLUE_LT;
          cv.drawRoundRect(tx, ty, TILE_W, TILE_H, 12, ring);
          cv.drawRoundRect(tx + 1, ty + 1, TILE_W - 2, TILE_H - 2, 11, ring);
          cv.fillRoundRect(tx + 5, ty + 5, TILE_W - 10, TILE_H - 10, 8, c);
        } else {
          cv.fillRoundRect(tx, ty, TILE_W, TILE_H, 12, c);
        }
      });
    }

    // Jas: popisek, hodnota, proužek a tlačítka po desítkách a jedničkách
    cv.text(F_MENU, 'Jas', MODAL_X + 20, BRI_Y + 14, COL.DIM);
    cv.textRight(F_STATE, v.bri ? `${v.bri} %` : '…', MODAL_X + MODAL_W - 20, BRI_Y, on ? COL.TEXT : COL.DIM);
    cv.fillRoundRect(MODAL_X + 20, BAR_Y, BAR_W, BAR_H, BAR_H >> 1, COL.CARD_HI);
    if (v.bri) {
      const fw = Math.max(Math.floor(v.bri * BAR_W / 100), BAR_H);
      cv.fillRoundRect(MODAL_X + 20, BAR_Y, fw, BAR_H, BAR_H >> 1, on ? (v.dim ? COL.BLUE_DIM : COL.BLUE) : COL.OFF_KNOB);
    }
    BRI_STEP_LBL.forEach((lbl, b) => {
      const bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP), pr = v.pressed === CTL_STEP0 + b;
      cv.fillRoundRect(bx, STEP_Y, STEP_W, STEP_H, 12, pr ? COL.BORDER : COL.CARD_HI);
      cv.drawRoundRect(bx, STEP_Y, STEP_W, STEP_H, 12, pr ? COL.BLUE : COL.BORDER);
      cv.textCentered(F_TITLE, lbl, bx + (STEP_W >> 1), STEP_Y + 11, COL.TEXT);
    });
  }

  // Úprava, která na světlo čeká nebo se právě posílá, se ukáže hned (jako ve firmwaru)
  function detailValues(i) {
    const lt = state.lights[i];
    const known = lt.on !== null && !lt.unknown;
    const v = { known, bri: known ? lt.bri : 0, hasColor: known && lt.hasColor !== false, hue: lt.hue || 0, sat: lt.sat || 0 };
    [state.sending, state.edit].forEach(e => {
      if (!e || e.dev !== i) return;
      if (e.bri) v.bri = e.bri;
      if (e.hue >= 0) { v.hasColor = true; v.hue = e.hue; v.sat = e.sat; }
    });
    return v;
  }

  function drawDetail() {
    const lt = state.lights[state.detailIdx], d = detailValues(state.detailIdx);
    drawDetailView({
      label: lt.label, known: d.known, on: !!lt.on, dim: lt.stale || lt.busy || !d.known, bri: d.bri,
      hasColor: d.hasColor, colorless: d.known && lt.hasColor === false && !d.hasColor, hue: d.hue, sat: d.sat,
      pressed: state.pressedCtl, ...plugView(state.detailIdx, lt),
    });
  }

  function drawDemoDetail() {
    const [label, on, bri, dim, , hue, sat, plug, onTime, onMin, onEn, offMin, offEn] = DEMO_STEPS[state.demoStep].lights[0];
    drawDetailView({
      label, known: on >= 0, on: on > 0, dim: dim || on < 0, bri, hasColor: !plug, colorless: false, hue, sat, pressed: -1,
      isPlug: !!plug, onTime: onTime || 0, schedOnEn: !!onEn, schedOnMin: onMin || 420, schedOffEn: !!offEn, schedOffMin: offMin || 1320,
    });
  }

  // Prvek detailu pod bodem; plochy jsou rozšířené o půl mezery (bez hluchých míst)
  function detailCtlAt(x, y) {
    const split = detailSplitX();
    if (hit(x, y, split, MODAL_Y, MODAL_X + MODAL_W - split, DET_HEAD_H)) return CTL_SWITCH;
    if (!state.demo && state.lights[state.detailIdx] && state.lights[state.detailIdx].isPlug) {
      const stepAt = (btnY, step0) => {
        for (let b = 0; b < 4; b++) {
          const bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP);
          if (hit(x, y, bx - (STEP_GAP >> 1), btnY - 4, STEP_W + STEP_GAP, PLUG_BTN_H + 8)) return step0 + b;
        }
        return -1;
      };
      if (hit(x, y, MODAL_X + 20, PLUG_ON_Y - 4, MODAL_W - 40, PLUG_SW_H + 8)) return CTL_ON_SW;
      let c = stepAt(PLUG_ON_BTN_Y, CTL_ON_STEP0); if (c >= 0) return c;
      if (hit(x, y, MODAL_X + 20, PLUG_OFF_Y - 4, MODAL_W - 40, PLUG_SW_H + 8)) return CTL_OFF_SW;
      return stepAt(PLUG_OFF_BTN_Y, CTL_OFF_STEP0);
    }
    for (let k = 0; k < COLOR_PRESETS.length; k++) {
      const t = tileRect(k);
      if (hit(x, y, t.x - (TILE_GAP >> 1), t.y - (TILE_ROW_GAP >> 1), TILE_W + TILE_GAP, TILE_H + TILE_ROW_GAP)) return k;
    }
    for (let b = 0; b < 4; b++) {
      const bx = MODAL_X + 20 + b * (STEP_W + STEP_GAP);
      if (hit(x, y, bx - (STEP_GAP >> 1), STEP_Y - 8, STEP_W + STEP_GAP, STEP_H + 16)) return CTL_STEP0 + b;
    }
    return -1;
  }

  function drawStatusLine() {
    cv.fillRect(0, CARDS_Y1, W, NAV_Y - 4 - CARDS_Y1, COL.BG);
    let s = '', col = COL.DIM;
    const busy = state.lights.some(l => l.busy);
    const detailErr = state.detailOpen ? state.lights[state.detailIdx].err : '';
    if (state.demo) { s = `Ukázka ${state.demoStep + 1}/${DEMO_STEPS.length}: ${DEMO_STEPS[state.demoStep].name}`; col = COL.BLUE_LT; }
    else if (state.settingsOpen) { s = SET_DESC[state.settingsSel]; col = COL.BLUE_LT; }
    else if (state.app === 'wifi') { s = 'Připojuji k Wi-Fi…'; col = COL.BLUE_LT; }
    else if (state.wifiFailed) { s = state.detailOpen ? 'Wi-Fi nedostupná' : 'Wi-Fi nedostupná · klepni na Obnovit'; col = COL.HOT; }
    else if (state.app === 'query') { s = 'Zjišťuji stav světel…'; col = COL.BLUE_LT; }
    else if (busy) { s = 'Přepínám…'; col = COL.BLUE_LT; }
    else if (detailErr) { s = detailErr; col = COL.HOT; }
    else if (state.detailOpen) s = `Změny platí hned · zhasnu za ${remainingSec()} s`;
    else s = `Klepni na jméno: barva a jas · zhasnu za ${remainingSec()} s`;
    const t = fitText(F_SUB18, s, 432 - 32);
    const pw = textWidth(F_SUB18, t) + 32, px = 240 - (pw >> 1);
    const bd = (col === COL.DIM) ? COL.BORDER : col;
    const bg = (col === COL.HOT) ? COL.RED_BG : COL.CARD;
    cv.fillRoundRect(px, STATUS_Y, pw, STATUS_H, 8, bg);
    cv.drawRoundRect(px, STATUS_Y, pw, STATUS_H, 8, bd);
    cv.textCentered(F_SUB18, t, 240, STATUS_Y + 1, (col === COL.DIM) ? COL.TEXT : col);
  }

  function drawNav() {
    cv.fillRect(0, NAV_Y - 4, W, H - (NAV_Y - 4), COL.BG);
    const lbl = state.demo ? ['Další', 'Ukončit'] : ['Obnovit', state.settingsOpen ? 'Zavřít' : (state.detailOpen ? 'Zpět' : 'Nastavení')];
    const modal = state.settingsOpen || state.detailOpen;
    [NAV1_X, NAV2_X].forEach((bx, i) => {
      if (i === 0 && modal && !state.demo) return;
      const active = state.pressedNav === i || (i === 1 && (modal || state.demo));
      const bg = active ? COL.CARD_HI : COL.CARD, bd = active ? COL.BLUE : COL.BORDER, fg = active ? COL.TEXT : COL.DIM;
      cv.fillRoundRect(bx, NAV_Y, NAV_W, NAV_H, 10, bg); cv.drawRoundRect(bx, NAV_Y, NAV_W, NAV_H, 10, bd);
      cv.textCentered(F_BTN, lbl[i], bx + (NAV_W >> 1), NAV_Y + 6, fg);
    });
  }

  function settingsRow(i) {
    const s = state.settings;
    switch (i) {
      case 0: return [`${s.brightness}/4`, COL.BLUE_LT];
      case 1: return [`${s.sleepSec} s`, COL.BLUE_LT];
      case 2: return [s.wakeTouch ? 'ZAP' : 'VYP', s.wakeTouch ? COL.BLUE : COL.DIM];
      case 3: return [s.sound ? 'ZAP' : 'VYP', s.sound ? COL.BLUE : COL.DIM];
      case 4: return ['SPUSTIT', COL.BLUE_LT];
      case 5: return [s.fullOff ? 'VYPNOUT' : 'HLUBOKÝ', s.fullOff ? COL.HOT : COL.BLUE_LT];
      default: return ['', COL.DIM];
    }
  }

  function drawSettings() {
    cv.fillRect(0, 50, W, NAV_Y - 4 - 50, COL.BG);
    cv.fillRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL.CARD);
    cv.drawRoundRect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 16, COL.BLUE);
    cv.drawRoundRect(MODAL_X + 1, MODAL_Y + 1, MODAL_W - 2, MODAL_H - 2, 15, COL.BLUE);
    cv.textCentered(F_TITLE, 'NASTAVENÍ', 240, MODAL_Y + 5, COL.BLUE_LT);
    cv.hline(MODAL_X + 16, MODAL_Y + 42, MODAL_W - 32, COL.BORDER);
    SET_ITEMS.forEach((name, i) => {
      const ry = MODAL_Y + ROW_Y0 + i * ROW_H, isSel = i === state.settingsSel;
      if (isSel) cv.fillRoundRect(MODAL_X + 10, ry, MODAL_W - 20, ROW_H - 4, 8, COL.CARD_HI);
      const ty = ry + 2;
      cv.text(F_MENU, name, MODAL_X + 24, ty, isSel ? COL.TEXT : COL.DIM);
      const [right, rcol] = settingsRow(i);
      if (right) cv.textRight(F_MENU, right, MODAL_X + MODAL_W - 24, ty, rcol);
    });
  }

  function drawSplash() {
    cv.fillScreen(COL.BG);
    // Pásek LED: osm dílků od tmavě modré po světlou
    const cols = [0x094B, 0x11CD, 0x1A6F, 0x2B3F, 0x3C5F, 0x5D1F, 0x8E3F, 0xDF7F], x0 = 240 - ((8 * 30) >> 1);
    cv.drawRoundRect(x0 - 10, 95, 8 * 30 + 20, 44, 12, COL.BORDER);
    cols.forEach((c, i) => cv.fillRoundRect(x0 + i * 30 + 3, 104, 24, 26, 6, c));
    cv.textCentered(F_STATE, 'PANEL SVĚTEL', 240, 168, COL.TEXT);
    cv.textCentered(F_BTN, 'Tapo · Wi-Fi', 240, 224, COL.DIM);
    const n = state.lights.length;
    cv.textCentered(F_TITLE, `${n} ${n === 1 ? 'světlo' : ((n >= 2 && n <= 4) ? 'světla' : 'světel')}`, 240, 264, COL.BLUE_LT);
    cv.textCentered(F_SUB18, 'klepnutím pokračovat', 240, 415, COL.DIM);
  }

  function render() {
    if (state.app === 'sleep') {
      cv.fillScreen(COL.BG);
      canvas.classList.add('is-off');
    } else {
      canvas.classList.remove('is-off');
      if (state.app === 'splash') drawSplash();
      else {
        cv.fillScreen(COL.BG);
        drawTopBar();
        if (state.demo) { if (DEMO_STEPS[state.demoStep].detail) drawDemoDetail(); else drawDemoCards(); }
        else if (state.settingsOpen) drawSettings();
        else if (state.detailOpen) drawDetail();
        else drawCards();
        drawStatusLine();
        drawNav();
      }
    }
    cv.flush();
  }

  // ─── Chování ───────────────────────────────────────────────────────────
  function startWifi() {
    clearTimers();
    state.app = 'wifi'; state.wifiFailed = false;
    log('připojuji k Wi-Fi…');
    later(state.wifiFailSim ? 4000 : 900, () => {
      if (state.wifiFailSim) {
        state.wifiFailed = true; state.app = 'idle'; state.pendingToggle = -1; state.edit = null; touch();
        melody(SND_ERR); log('Wi-Fi nedostupná');
        return;
      }
      log('připojeno k síti domov');
      state.app = 'query'; touch();
      queryNext(0);
    });
  }
  function queryNext(i) {
    if (i >= state.lights.length) {
      state.app = 'idle'; touch();
      if (state.pendingToggle >= 0) { const t = state.pendingToggle; state.pendingToggle = -1; toggle(t); }
      return;
    }
    const lt = state.lights[i];
    lt.busy = true;
    later(failing(i) ? 1500 : 380, () => {
      lt.busy = false;
      if (failing(i)) { lt.err = 'není v síti'; log(`${lt.label}: get_device_info selhalo (není v síti)`); }
      else {
        if (lt.on === null) lt.on = false;
        lt.stale = false; lt.unknown = false; lt.err = '';
        log(`${lt.label}: ${lt.on ? 'zapnuto, jas ' + lt.bri + ' %' : 'vypnuto'}`);
      }
      queryNext(i + 1);
    });
  }
  function toggle(i) {
    const lt = state.lights[i];
    if (!lt) return;
    touch();
    if (state.app === 'wifi' || state.app === 'query') { state.pendingToggle = i; log(`${lt.label}: přepnu po načtení stavu`); return; }
    if (state.wifiFailed) { state.pendingToggle = i; startWifi(); return; }
    if (lt.busy) return;
    const target = !(!lt.unknown && lt.on === true);
    if (!target && state.edit && state.edit.dev === i) state.edit = null;   // zhasnutí přebije čekající barvu či jas
    lt.busy = true; lt.err = '';
    log(`${lt.label}: posílám set_device_info device_on=${target}`);
    later(failing(i) ? 1500 : 550, () => {
      lt.busy = false;
      if (failing(i)) {
        lt.err = 'není v síti'; lt.stale = true;
        melody(SND_ERR); log(`${lt.label}: set_device_info selhalo (není v síti)`);
      } else {
        lt.on = target; lt.stale = false; lt.unknown = false;
        if (target && !lt.bri && !lt.isPlug) lt.bri = 100;
        if (lt.isPlug && target) { lt.onBase = 0; lt.onAt = performance.now(); }
        melody(target ? SND_ON : SND_OFF);
        log(`${lt.label}: ${target ? 'ZAPNUTO' : 'VYPNUTO'}`);
      }
      touch();
    });
  }
  function refreshAll() {
    touch();
    if (state.app !== 'idle') return;
    state.lights.forEach(l => { l.err = ''; });
    if (state.wifiFailed) startWifi(); else { state.app = 'query'; queryNext(0); }
  }

  // Detail světla: úprava se pošle hned, klepnutí během odesílání jen přepíšou
  // čekající hodnotu, takže pásek dostane vždy tu poslední (jako ve firmwaru)
  function openDetail(i) { state.detailOpen = true; state.detailIdx = i; state.pressedCtl = -1; }
  function closeDetail() { state.detailOpen = false; state.pressedCtl = -1; }
  function sendEdit() {
    const e = state.edit;
    if (!e || state.sending || state.app !== 'idle') return;
    if (state.wifiFailed) { startWifi(); return; }
    const lt = state.lights[e.dev];
    state.edit = null;
    if (!lt) return;
    state.sending = e;
    if (e.schedOn || e.schedOff) {
      // Časovač zásuvky: add/edit_schedule_rule, device_on se nemění
      const what = [];
      if (e.schedOn) what.push(`zapnutí ${e.schedOnEn ? hhmm(e.schedOnMin) : 'vypnuto'}`);
      if (e.schedOff) what.push(`vypnutí ${e.schedOffEn ? hhmm(e.schedOffMin) : 'vypnuto'}`);
      log(`${lt.label}: posílám edit_schedule_rule, ${what.join(', ')}`);
      later(failing(e.dev) ? 1500 : 250, () => {
        state.sending = null;
        if (failing(e.dev)) {
          lt.err = 'není v síti'; lt.stale = true;
          melody(SND_ERR); log(`${lt.label}: edit_schedule_rule selhalo (není v síti)`);
        } else {
          if (e.schedOn) { lt.schedOnEn = e.schedOnEn; lt.schedOnMin = e.schedOnMin; }
          if (e.schedOff) { lt.schedOffEn = e.schedOffEn; lt.schedOffMin = e.schedOffMin; }
          lt.stale = false; lt.err = '';
        }
        touch();
      });
      return;
    }
    const parts = ['device_on=true'];
    if (e.bri) parts.push(`brightness=${e.bri}`);
    if (e.hue >= 0) parts.push(`hue=${e.hue} saturation=${e.sat}`);
    log(`${lt.label}: posílám set_device_info ${parts.join(' ')}`);
    later(failing(e.dev) ? 1500 : 250, () => {
      state.sending = null;
      if (failing(e.dev)) {
        lt.err = 'není v síti'; lt.stale = true;
        melody(SND_ERR); log(`${lt.label}: set_device_info selhalo (není v síti)`);
      } else {
        const wasOn = !lt.unknown && lt.on === true;
        lt.on = true; lt.unknown = false; lt.stale = false; lt.err = '';
        if (e.bri) lt.bri = e.bri;
        if (!lt.bri) lt.bri = 100;
        if (e.hue >= 0) { lt.hue = e.hue; lt.sat = e.sat; lt.hasColor = true; }
        if (!wasOn) melody(SND_ON);
      }
      touch();
    });
  }
  function detailTap(x, y) {
    const c = detailCtlAt(x, y);
    if (c < 0) return;
    const i = state.detailIdx, lt = state.lights[i];
    const known = lt.on !== null && !lt.unknown;
    if (c < CTL_STEP0 && known && lt.hasColor === false) return;   // bílá žárovka barvu nemá
    click();
    if (c === CTL_SWITCH) { toggle(i); return; }
    if (state.edit && state.edit.dev !== i) sendEdit();
    const pending = state.edit && state.edit.dev === i ? state.edit : null;
    const sending = state.sending && state.sending.dev === i ? state.sending : null;
    if (lt.isPlug) {
      // Časovač zásuvky: posun času nebo přepínač povolení; posun pravidlo zároveň povolí
      const pv = plugView(i, lt);
      if (!state.edit) state.edit = { dev: i, bri: 0, hue: -1, sat: 0 };
      const e = state.edit;
      const stepMin = (cur, d, other) => {
        let m = ((cur % 1440) + 1440) % 1440;
        if ((d === 5 || d === -5) && m % 5 !== 0) m = d > 0 ? (Math.floor(m / 5) + 1) * 5 : Math.floor(m / 5) * 5;
        else m += d;
        m = ((m % 1440) + 1440) % 1440;
        if (m === ((other % 1440) + 1440) % 1440) m = ((m + d) % 1440 + 1440) % 1440;
        return m;
      };
      if (c === CTL_ON_SW) { e.schedOn = true; e.schedOnEn = !pv.schedOnEn; e.schedOnMin = pv.schedOnMin; }
      else if (c >= CTL_ON_STEP0 && c < CTL_ON_STEP0 + 4) { e.schedOn = true; e.schedOnEn = true; e.schedOnMin = stepMin(pv.schedOnMin, SCHED_STEPS[c - CTL_ON_STEP0], pv.schedOffMin); }
      else if (c === CTL_OFF_SW) { e.schedOff = true; e.schedOffEn = !pv.schedOffEn; e.schedOffMin = pv.schedOffMin; }
      else if (c >= CTL_OFF_STEP0 && c < CTL_OFF_STEP0 + 4) { e.schedOff = true; e.schedOffEn = true; e.schedOffMin = stepMin(pv.schedOffMin, SCHED_STEPS[c - CTL_OFF_STEP0], pv.schedOnMin); }
      return;
    }
    const lit = (known && lt.on === true) || !!sending;
    const startEdit = () => { if (!state.edit) state.edit = { dev: i, bri: 0, hue: -1, sat: 0 }; return state.edit; };
    if (c >= CTL_STEP0) {
      const cur = (pending && pending.bri) || (sending && sending.bri) || ((known && lt.bri) ? lt.bri : 100);
      const nv = Math.min(100, Math.max(1, cur + BRI_STEPS[c - CTL_STEP0]));
      if (nv === cur && (pending || lit)) return;
      startEdit().bri = nv;
    } else {
      const src = (pending && pending.hue >= 0) ? pending : ((sending && sending.hue >= 0) ? sending : (known ? lt : null));
      if (src && presetIndex(src.hue || 0, src.sat || 0) === c && (pending || lit)) return;
      const e = startEdit();
      e.hue = COLOR_PRESETS[c][1]; e.sat = COLOR_PRESETS[c][2];
      log(`${lt.label}: barva ${COLOR_PRESETS[c][0]}`);
    }
  }

  function openSettings() { state.settingsOpen = true; state.settingsSel = 0; state.detailOpen = false; }
  function closeSettings() { state.settingsOpen = false; }
  function startDemo() {
    closeSettings();
    state.demo = true; state.demoStep = 0; state.demoLast = now();
    state.pendingToggle = -1; state.edit = null; state.detailOpen = false;
    log('ukázka: klepnutí nebo BOOT = další, KEY nebo Ukončit = konec');
  }
  function stopDemo() { state.demo = false; touch(); }
  function nextDemoStep() { state.demoStep = (state.demoStep + 1) % DEMO_STEPS.length; state.demoLast = now(); }
  function changeSetting(i) {
    const s = state.settings;
    switch (i) {
      case 0: s.brightness = (s.brightness + 1) % 5; applyBrightness(); break;
      case 1: s.sleepSec = SLEEP_OPTIONS[(SLEEP_OPTIONS.indexOf(s.sleepSec) + 1) % SLEEP_OPTIONS.length]; break;
      case 2: s.wakeTouch = !s.wakeTouch; break;
      case 3: s.sound = !s.sound; if (s.sound) click(); break;
      case 4: startDemo(); return;
      case 5: s.fullOff = !s.fullOff; break;
      default: closeSettings(); return;
    }
    log(`nastavení: ${SET_ITEMS[i]} → ${settingsRow(i)[0]}`);
  }
  function goSleep() {
    clearTimers();
    state.demo = false; state.settingsOpen = false; state.detailOpen = false; state.sel = 0; state.selVisible = false;
    state.pressedCard = -1; state.pressedRight = false; state.pressedNav = -1; state.pressedCtl = -1;
    state.pendingToggle = -1; state.edit = null;
    state.lights.forEach(l => { l.busy = false; });
    state.app = 'sleep';
    log(state.onUsb ? 'displej zhasnut, dřímám (USB)' : (state.settings.fullOff ? 'úplné vypnutí, zapne jen PWR' : 'hluboký spánek, probudí dotyk, BOOT nebo KEY'));
  }
  // Na USB budí cokoli. Na baterii z hlubokého spánku dotyk (lze vypnout), BOOT
  // a KEY, PWR ne; po úplném vypnutí jen PWR a krabička startuje od začátku.
  function wake(by) {
    if (state.app !== 'sleep') return;
    if (!state.onUsb) {
      if (state.settings.fullOff) {
        if (by === 'PWR') powerOn(); else log('krabička je vypnutá, zapne ji jen podržení PWR');
        return;
      }
      if (by === 'PWR') { log('z hlubokého spánku PWR nebudí, použij dotyk, BOOT nebo KEY'); return; }
      if (by === 'dotyk' && !state.settings.wakeTouch) { log('probuzení dotykem je vypnuté, použij BOOT nebo KEY'); return; }
    }
    state.lights.forEach(l => { l.stale = true; l.err = ''; });
    log(`probuzení (${by})`);
    touch();
    startWifi();
  }
  // Zapnutí: úvodní obrazovka 5 s (klepnutí nebo tlačítko ji zkrátí), poslední
  // stav světel ani relace KLAP vypnutí nepřežijí
  function powerOn() {
    clearTimers();
    state.lights.forEach(l => { l.stale = true; l.unknown = true; l.err = ''; l.busy = false; });
    state.wifiFailed = false;
    state.app = 'splash';
    log('zapnutí krabičky');
    melody(SND_BOOT);
    touch();
    later(5000, () => { if (state.app === 'splash') startWifi(); });
  }

  // ─── Vstupy: dotyk na plátně ───────────────────────────────────────────
  function canvasPoint(ev) {
    const r = canvas.getBoundingClientRect();
    const p = ev.changedTouches ? ev.changedTouches[0] : ev;
    return { x: Math.floor((p.clientX - r.left) * W / r.width), y: Math.floor((p.clientY - r.top) * H / r.height) };
  }
  const hit = (px, py, x, y, w, h) => px >= x && px < x + w && py >= y && py < y + h;
  let downPos = null, longFired = false, longTimer = null;

  function pressedAt(p) {
    state.pressedCard = -1; state.pressedRight = false; state.pressedNav = -1; state.pressedCtl = -1;
    if (state.app === 'sleep' || state.app === 'splash') return;
    if (!state.settingsOpen && !state.detailOpen && !state.demo) {
      const n = state.lights.length;
      state.lights.forEach((_, i) => {
        const c = cardRectN(i, n);
        if (!hit(p.x, p.y, c.x, c.y, c.w, c.h)) return;
        const t = cardToggleRect(i, n);
        state.pressedCard = i; state.pressedRight = hit(p.x, p.y, t.x, t.y, t.w, t.h);
      });
    }
    if (state.detailOpen && !state.demo) state.pressedCtl = detailCtlAt(p.x, p.y);
    if ((state.demo || (!state.settingsOpen && !state.detailOpen)) && hit(p.x, p.y, NAV1_X, NAV_Y, NAV_W, NAV_H)) state.pressedNav = 0;
    if (hit(p.x, p.y, NAV2_X, NAV_Y, NAV_W, NAV_H)) state.pressedNav = 1;
  }
  function clearPressed() { state.pressedCard = -1; state.pressedRight = false; state.pressedNav = -1; state.pressedCtl = -1; }

  // Klepnutí (a v detailu i podržení) na bod displeje
  function tapAt(x, y) {
    touch();
    if (state.demo) {
      click();
      if (hit(x, y, NAV2_X, NAV_Y, NAV_W, NAV_H)) stopDemo(); else nextDemoStep();
      return;
    }
    if (!state.settingsOpen && !state.detailOpen && hit(x, y, NAV1_X, NAV_Y, NAV_W, NAV_H)) { click(); refreshAll(); return; }
    if (hit(x, y, NAV2_X, NAV_Y, NAV_W, NAV_H)) {
      click();
      if (state.settingsOpen) closeSettings(); else if (state.detailOpen) closeDetail(); else openSettings();
      return;
    }
    if (state.settingsOpen) {
      if (x < MODAL_X || x >= MODAL_X + MODAL_W) return;
      SET_ITEMS.forEach((_, i) => { const ry = MODAL_Y + ROW_Y0 + i * ROW_H; if (y >= ry && y < ry + ROW_H) { state.settingsSel = i; click(); changeSetting(i); } });
      return;
    }
    if (state.detailOpen) { detailTap(x, y); return; }
    // Pravá část karty (stav a přepínač) přepne, zbytek otevře detail s barvou a jasem
    const n = state.lights.length;
    state.lights.forEach((_, i) => {
      const c = cardRectN(i, n);
      if (!hit(x, y, c.x, c.y, c.w, c.h)) return;
      click(); state.sel = i;
      const t = cardToggleRect(i, n);
      if (hit(x, y, t.x, t.y, t.w, t.h)) toggle(i); else openDetail(i);
    });
  }
  // Dotyk: touchstart neblokuje posouvání stránky (na telefonu je plátno přes
  // celou šířku); posun prstu o víc než 10 px klepnutí zruší
  let touchStart = null;
  function cancelPress() { clearTimeout(longTimer); clearPressed(); downPos = null; touchStart = null; }
  function onDown(ev) {
    if (ev.touches) { const t = ev.touches[0]; touchStart = { x: t.clientX, y: t.clientY }; }
    else ev.preventDefault();
    downPos = canvasPoint(ev); longFired = false;
    pressedAt(downPos);
    if (state.app === 'sleep' || state.app === 'splash') return;
    longTimer = setTimeout(() => {
      longFired = true;
      // V detailu platí podržení jako pomalé klepnutí na prvek pod prstem
      if (state.detailOpen && !state.demo) { tapAt(downPos.x, downPos.y); clearPressed(); return; }
      // Podržení prstu: v ukázce ji ukončí, jinak otevře / zavře nastavení
      touch(); click();
      if (state.demo) stopDemo();
      else if (state.settingsOpen) closeSettings();
      else openSettings();
      clearPressed();
    }, 650);
  }
  function onUp(ev) {
    ev.preventDefault();
    clearTimeout(longTimer);
    clearPressed();
    if (!downPos) return;
    const { x, y } = downPos; downPos = null;
    if (longFired) return;
    if (state.app === 'sleep') { wake('dotyk'); return; }
    if (state.app === 'splash') { startWifi(); return; }
    tapAt(x, y);
  }
  canvas.addEventListener('mousedown', onDown);
  canvas.addEventListener('mouseup', onUp);
  canvas.addEventListener('mouseleave', cancelPress);
  canvas.addEventListener('touchstart', onDown, { passive: true });
  canvas.addEventListener('touchmove', ev => {
    if (!touchStart) return;
    const t = ev.touches[0];
    if (Math.abs(t.clientX - touchStart.x) > 10 || Math.abs(t.clientY - touchStart.y) > 10) cancelPress();
  }, { passive: true });
  canvas.addEventListener('touchcancel', cancelPress);
  canvas.addEventListener('touchend', onUp, { passive: false });
  canvas.addEventListener('contextmenu', ev => ev.preventDefault());

  // ─── Vstupy: fyzická tlačítka ──────────────────────────────────────────
  function bindHold(id, onShort, onLong) {
    const el = document.getElementById(id);
    if (!el) return;
    let t = null, fired = false, down = false;
    const press = (ev) => { ev.preventDefault(); down = true; fired = false; t = setTimeout(() => { fired = true; if (onLong) onLong(); }, 700); };
    const release = (ev) => { ev.preventDefault(); if (!down) return; down = false; clearTimeout(t); if (!fired) onShort(); };
    el.addEventListener('mousedown', press); el.addEventListener('mouseup', release);
    el.addEventListener('mouseleave', () => { down = false; clearTimeout(t); });
    el.addEventListener('touchstart', press, { passive: false }); el.addEventListener('touchend', release, { passive: false });
  }
  bindHold('btn-boot', () => {
    if (state.app === 'sleep') { wake('BOOT'); return; }
    if (state.app === 'splash') { startWifi(); return; }
    click(); touch();
    if (state.demo) nextDemoStep();
    else if (state.settingsOpen) state.settingsSel = (state.settingsSel + 1) % SET_ITEMS.length;
    else if (state.detailOpen) closeDetail();
    else if (state.lights.length <= 1) refreshAll();
    else if (!state.selVisible) { state.sel = 0; state.selVisible = true; }   // první stisk označí první světlo
    else state.sel = (state.sel + 1) % state.lights.length;
  }, () => {
    if (state.app === 'sleep' || state.app === 'splash') return;
    click(); touch();
    if (state.demo) stopDemo();
    else if (state.settingsOpen) closeSettings();
    else openSettings();
  });
  bindHold('btn-key', () => {
    if (state.app === 'sleep') { wake('KEY'); return; }
    if (state.app === 'splash') { startWifi(); return; }
    click(); touch();
    if (state.demo) stopDemo();
    else if (state.settingsOpen) changeSetting(state.settingsSel);
    else if (state.detailOpen) toggle(state.detailIdx);
    else { state.selVisible = true; toggle(state.sel); }
  });
  bindHold('btn-pwr', () => {
    if (state.app === 'sleep') { wake('PWR'); return; }
    if (state.app === 'splash') return;
    goSleep();
  }, () => { log('podržení PWR 6 s = úplné vypnutí (v simulátoru jen spánek)'); goSleep(); });

  // ─── Volby simulátoru ──────────────────────────────────────────────────
  const optUsb = document.getElementById('opt-usb');
  const optWifi = document.getElementById('opt-wifi-fail');
  const optFail = document.getElementById('opt-light-fail');
  const optLights = document.getElementById('opt-lights');
  if (optUsb) optUsb.addEventListener('change', () => { state.onUsb = optUsb.checked; state.battery = optUsb.checked ? 100 : 78; log(optUsb.checked ? 'napájení z USB: jen zhasne, nespí' : 'napájení z baterie: po prodlevě usne'); });
  if (optWifi) optWifi.addEventListener('change', () => { state.wifiFailSim = optWifi.checked; log(optWifi.checked ? 'simuluji výpadek Wi-Fi (projeví se při dalším připojení)' : 'Wi-Fi zase dostupná'); });
  if (optFail) optFail.addEventListener('change', () => { state.lightFailSim = optFail.checked; log(optFail.checked ? 'poslední světlo nebude v síti (klepni na Obnovit)' : 'všechna světla zase v síti'); });
  if (optLights) optLights.addEventListener('change', () => {
    const n = parseInt(optLights.value, 10) || 2;
    state.lights = makeLights(n).map(l => ({ ...l, stale: false }));
    state.sel = 0; state.selVisible = false; state.detailOpen = false; state.edit = null; touch();
    log(`${n} ${n === 1 ? 'světlo' : (n <= 4 ? 'světla' : 'světel')} na panelu`);
  });

  // ─── Hlavní smyčka ─────────────────────────────────────────────────────
  let lastRender = 0;
  function loop(t) {
    if (state.demo && now() - state.demoLast >= DEMO_STEP_MS) nextDemoStep();
    if (state.app === 'idle' && !state.lights.some(l => l.busy) && !state.sending && now() - state.lastInteract >= idleLimitMs()) goSleep();
    const lim = idleLimitMs();
    const wantDim = state.app === 'idle' && !state.demo && lim >= 10000 && now() - state.lastInteract >= lim - 5000;
    if (wantDim !== dimmed) { dimmed = wantDim; applyBrightness(); }
    if (t - lastRender >= 50) { lastRender = t; render(); }
    sendEdit();
    requestAnimationFrame(loop);
  }

  // Start jako zapnutí krabičky
  powerOn();
  requestAnimationFrame(loop);
})();
