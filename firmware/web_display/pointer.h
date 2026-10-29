// The mouse pointer (a USB mouse, usb_hid.h). The PC's headless browser draws none, so the Tab5 draws it over the
// page itself, which also means it follows the mouse without a round trip to the PC.
//
// It is an anti-aliased arrow with a soft shadow, alpha-blended over the page. The page beneath it is kept in
// g_ptr_under: moving the pointer puts that back and blends the arrow at the new place. A rectangle or strip of the
// page that arrives under the pointer is decoded into g_out first (a strip otherwise goes straight into the frame
// buffer), the pointer's part of it taken into g_ptr_under and the arrow blended into it, and then it is copied to
// the screen by the PPA as usual: so the arrow never flickers over a video.
//
// The blend runs on the P4's SIMD unit (PIE: 128-bit registers q0-q7, here 8 RGB565 pixels at a time). It is not
// needed for speed (the arrow is 1152 pixels: measured 86 us in plain C, 8.5 us with PIE, against ~10 ms to decode a
// frame), it is there because the chip has it. ptr_selftest() checks it against the C version at startup, which is
// used instead if they ever differ; serial "simd" runs the check again and times both, "simd stress N" runs N blends
// of new random pages.
#pragma once
#include <esp_cpu.h>

static constexpr int PTR_W = 32, PTR_H = 36;   // the pointer's box, logical px (PTR_W a multiple of 8: PIE vectors)
static constexpr int PTR_HOT = 3;              // the arrow's tip, this far into the box both ways
static constexpr int PTR_N = PTR_W * PTR_H;

struct Pointer {
  bool shown = false;      // wanted on screen: a mouse moved since the last touch
  bool drawn = false;      // in the frame buffer, at box (bx, by), drawn with rotation rot
  int x = SCR_W / 2, y = SCR_H / 2;   // the tip, logical
  int bx = 0, by = 0, rot = 1;
  uint8_t buttons = 0;     // the mouse's (for the settings panel)
  bool simd = false;       // the PIE blend passed its check
};
static Pointer g_ptr;
struct PtrStats { uint32_t draws, patches; };
static PtrStats g_ptr_stats = {};

// The arrow as 32 - alpha (0-32) and its colour premultiplied by alpha (RGB565), as the blend takes them; and as
// LVGL's RGB565A8 image (colour plane, then alpha plane) for the settings panel.
alignas(16) static uint16_t g_ptr_ia[PTR_N], g_ptr_fg[PTR_N];
alignas(16) static uint16_t g_ptr_under[PTR_N], g_ptr_out[PTR_N];
static uint8_t g_ptr_lv[PTR_N * 3];

// ---- the blend: out = under * (32 - a) / 32 + colour * a / 32, per channel ----

static void ptr_blend_c(const uint16_t* bg, const uint16_t* ia, const uint16_t* fg, uint16_t* out, int n) {
  for (int i = 0; i < n; ++i) {
    const uint32_t b = bg[i], k = ia[i];
    out[i] = (uint16_t)(((((b & 0xF800) * k) >> 5) & 0xF800) | ((((b & 0x07E0) * k) >> 5) & 0x07E0) |
                        ((((b & 0x001F) * k) >> 5) & 0x001F)) + fg[i];
  }
}

// The same, 8 pixels per step. Each channel is masked out in place, multiplied by 32 - a (esp.vmul.u16 shifts the
// 32-bit products right by SAR = 5) and masked again; the three are put together and the colour added (it can't
// carry into the next channel: each sum is at most the channel's maximum). n: a multiple of 8, 16-byte aligned arrays.
// (PIE instructions take only the registers x8-x15 for addresses and counts, hence the register variables.) A plain
// loop: with the hardware loop (esp.lp.setup) a run sometimes came out wrong on a v1.x chip, its last 8 pixels (once
// in 100000 runs of "simd stress"; 0 in 1000000 like this, for 10% more time).
static void ptr_blend_pie(const uint16_t* bg, const uint16_t* ia, const uint16_t* fg, uint16_t* out, int n) {
  alignas(16) static const uint16_t masks[24] = {
    0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800,
    0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0,
    0x001F, 0x001F, 0x001F, 0x001F, 0x001F, 0x001F, 0x001F, 0x001F,
  };
  if (n < 8) return;
  register const uint16_t* r_bg asm("a0") = bg;
  register const uint16_t* r_ia asm("a1") = ia;
  register const uint16_t* r_fg asm("a2") = fg;
  register uint16_t* r_out asm("a3") = out;
  register const uint16_t* r_m asm("a4") = masks;
  register int r_steps asm("a5") = n / 8;
  register int r_sar asm("s1") = 5;
  asm volatile(
      "esp.movx.w.sar %[sar]\n\t"
      "esp.vld.128.ip q5, %[m], 16\n\t"   // q5-q7: the red, green and blue masks
      "esp.vld.128.ip q6, %[m], 16\n\t"
      "esp.vld.128.ip q7, %[m], 16\n\t"
      "1:\n\t"
      "esp.vld.128.ip q0, %[bg], 16\n\t"
      "esp.vld.128.ip q1, %[ia], 16\n\t"
      "esp.vld.128.ip q2, %[fg], 16\n\t"
      "esp.andq q3, q0, q5\n\t"
      "esp.vmul.u16 q3, q3, q1\n\t"
      "esp.andq q3, q3, q5\n\t"
      "esp.andq q4, q0, q6\n\t"
      "esp.vmul.u16 q4, q4, q1\n\t"
      "esp.andq q4, q4, q6\n\t"
      "esp.orq q3, q3, q4\n\t"
      "esp.andq q4, q0, q7\n\t"
      "esp.vmul.u16 q4, q4, q1\n\t"
      "esp.andq q4, q4, q7\n\t"
      "esp.orq q3, q3, q4\n\t"
      "esp.vadd.u16 q3, q3, q2\n\t"
      "esp.vst.128.ip q3, %[out], 16\n\t"
      "addi %[steps], %[steps], -1\n\t"
      "bnez %[steps], 1b\n\t"
      : [bg] "+r"(r_bg), [ia] "+r"(r_ia), [fg] "+r"(r_fg), [out] "+r"(r_out), [m] "+r"(r_m), [steps] "+r"(r_steps)
      : [sar] "r"(r_sar)
      : "memory");
}

static void ptr_blend(const uint16_t* bg, uint16_t* out) {
  if (g_ptr.simd) ptr_blend_pie(bg, g_ptr_ia, g_ptr_fg, out, PTR_N);
  else ptr_blend_c(bg, g_ptr_ia, g_ptr_fg, out, PTR_N);
}

// ---- the arrow ----

// Its outline (px from the tip), the classic shape: down the left side, the notch, the tail, back up to the wing.
static const float PTR_SHAPE[][2] = {{0, 0}, {0, 24}, {6, 18.4f}, {10.5f, 27.3f}, {13.6f, 25.8f}, {9.2f, 16.9f}, {16.8f, 16.9f}};
static constexpr int PTR_SHAPE_N = sizeof(PTR_SHAPE) / sizeof(PTR_SHAPE[0]);

static bool ptr_inside(float x, float y) {   // even-odd rule
  bool in = false;
  for (int i = 0, j = PTR_SHAPE_N - 1; i < PTR_SHAPE_N; j = i++) {
    const float xi = PTR_SHAPE[i][0], yi = PTR_SHAPE[i][1], xj = PTR_SHAPE[j][0], yj = PTR_SHAPE[j][1];
    if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
  }
  return in;
}

static float ptr_edge_dist(float x, float y) {
  float best = 1e9f;
  for (int i = 0, j = PTR_SHAPE_N - 1; i < PTR_SHAPE_N; j = i++) {
    const float ax = PTR_SHAPE[j][0], ay = PTR_SHAPE[j][1], dx = PTR_SHAPE[i][0] - ax, dy = PTR_SHAPE[i][1] - ay;
    const float t = constrain(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    best = min(best, hypotf(x - ax - t * dx, y - ay - t * dy));
  }
  return best;
}

// Rendered once, 4 x 4 samples per pixel: a black arrow with a white inside (1.4 px in from the edge), over a
// shadow 1.2 px right and 2 px down that fades out over 2.5 px.
static void ptr_make_sprite() {
  for (int py = 0; py < PTR_H; ++py)
    for (int px = 0; px < PTR_W; ++px) {
      float a = 0, c = 0;   // coverage, and colour (0 black - 1 white) times coverage
      for (int s = 0; s < 16; ++s) {
        const float x = px - PTR_HOT + (s % 4 + 0.5f) / 4, y = py - PTR_HOT + (s / 4 + 0.5f) / 4;
        if (ptr_inside(x, y)) {
          a += 1;
          if (ptr_edge_dist(x, y) > 1.4f) c += 1;
        } else {
          const float sx = x - 1.2f, sy = y - 2.0f;
          a += 0.35f * (ptr_inside(sx, sy) ? 1.0f : max(0.0f, 1.0f - ptr_edge_dist(sx, sy) / 2.5f));
        }
      }
      a /= 16;
      const float col = a > 0 ? c / 16 / a : 0;   // the colour itself
      const int i = py * PTR_W + px, a32 = (int)(a * 32 + 0.5f);
      g_ptr_ia[i] = 32 - a32;
      g_ptr_fg[i] = (uint16_t)((int)(col * 31 * a32 / 32) << 11 | (int)(col * 63 * a32 / 32) << 5 | (int)(col * 31 * a32 / 32));
      const uint16_t c565 = (uint16_t)((int)(col * 31 + 0.5f) << 11 | (int)(col * 63 + 0.5f) << 5 | (int)(col * 31 + 0.5f));
      g_ptr_lv[2 * i] = c565 & 0xFF;
      g_ptr_lv[2 * i + 1] = c565 >> 8;
      g_ptr_lv[2 * PTR_N + i] = (uint8_t)(a * 255 + 0.5f);
    }
}

// ---- the check ----

struct PtrTest { bool ok; uint32_t c_cycles, pie_cycles; };

static PtrTest ptr_selftest() {
  alignas(16) static uint16_t bg[PTR_N], a[PTR_N], b[PTR_N];
  PtrTest r = {true, ~0u, ~0u};
  uint32_t seed = 12345;
  for (int round = 0; round < 8 && r.ok; ++round) {
    for (int i = 0; i < PTR_N; ++i) {   // random pages, and plain black / white / one colour
      seed = seed * 1664525 + 1013904223;
      bg[i] = round == 0 ? 0 : round == 1 ? 0xFFFF : round == 2 ? 0x8410 : (uint16_t)(seed >> 16);
    }
    a[PTR_N - 1] = 0x5A5A;   // (a guard: the PIE loop must write exactly n pixels)
    memset(a, 0, sizeof(a) - 2);
    ptr_blend_c(bg, g_ptr_ia, g_ptr_fg, b, PTR_N);
    ptr_blend_pie(bg, g_ptr_ia, g_ptr_fg, a, PTR_N - 8);
    r.ok = memcmp(a, b, (PTR_N - 8) * 2) == 0 && a[PTR_N - 1] == 0x5A5A;
    ptr_blend_pie(bg, g_ptr_ia, g_ptr_fg, a, PTR_N);
    r.ok = r.ok && memcmp(a, b, sizeof(a)) == 0;
    if (!r.ok)
      for (int i = 0; i < PTR_N; ++i)
        if (a[i] != b[i]) {
          Serial.printf("simd: differs at %d: bg %04x a %d -> C %04x, PIE %04x\n", i, bg[i], 32 - g_ptr_ia[i], b[i], a[i]);
          break;
        }
  }
  for (int k = 0; k < 4; ++k) {   // the fastest of a few runs each (an interrupt can land in one)
    uint32_t t = esp_cpu_get_cycle_count();
    ptr_blend_c(bg, g_ptr_ia, g_ptr_fg, b, PTR_N);
    r.c_cycles = min(r.c_cycles, esp_cpu_get_cycle_count() - t);
    t = esp_cpu_get_cycle_count();
    ptr_blend_pie(bg, g_ptr_ia, g_ptr_fg, a, PTR_N);
    r.pie_cycles = min(r.pie_cycles, esp_cpu_get_cycle_count() - t);
  }
  return r;
}

// serial "simd stress N": N blends of new random pages into an output filled with a marker each time, against C.
// (A check on the same data again would miss a fault that writes some pixels in the wrong place: they'd still hold
// the right values from the run before.)
static void ptr_stress(int runs) {
  alignas(16) static uint16_t bg[PTR_N], a[PTR_N], b[PTR_N];
  uint32_t seed = esp_cpu_get_cycle_count(), bad = 0, first = 0;
  for (int run = 0; run < runs; ++run) {
    for (int i = 0; i < PTR_N; ++i) {
      seed = seed * 1664525 + 1013904223;
      bg[i] = (uint16_t)(seed >> 16);
    }
    for (int i = 0; i < PTR_N; ++i) a[i] = 0xA5C3;
    ptr_blend_c(bg, g_ptr_ia, g_ptr_fg, b, PTR_N);
    ptr_blend_pie(bg, g_ptr_ia, g_ptr_fg, a, PTR_N);
    if (memcmp(a, b, sizeof(a))) {
      if (!bad++)
        for (int i = 0; i < PTR_N; ++i)
          if (a[i] != b[i]) { first = i; break; }
    }
    if (run % 200 == 199) vTaskDelay(1);   // (the task watchdog)
  }
  Serial.printf("simd stress: %u of %d runs differ%s", bad, runs, bad ? "" : "\n");
  if (bad) Serial.printf(", the first at pixel %u\n", first);
}

static void ptr_report(const PtrTest& t) {
  const uint32_t mhz = ESP.getCpuFreqMHz();
  Serial.printf("simd: PIE blend %s; %d pixels: C %.1f us, PIE %.1f us (%.1fx)\n", t.ok ? "matches C" : "DIFFERS, using C",
                PTR_N, (float)t.c_cycles / mhz, (float)t.pie_cycles / mhz, (float)t.c_cycles / (t.pie_cycles ? t.pie_cycles : 1));
}

static void ptr_setup() {
  ptr_make_sprite();
  const PtrTest t = ptr_selftest();
  g_ptr.simd = t.ok;
  ptr_report(t);
}

// ---- in the frame buffer ----

// Write back / drop the cache lines of a range (its ends rounded out to whole 128-byte lines).
static void ptr_cache(const void* p, size_t n, int flags) {
  const uintptr_t a = (uintptr_t)p & ~(uintptr_t)127, e = ((uintptr_t)p + n + 127) & ~(uintptr_t)127;
  esp_cache_msync((void*)a, e - a, flags);
}
static constexpr int PTR_TO_RAM = ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE;
static constexpr int PTR_FROM_RAM = ESP_CACHE_MSYNC_FLAG_DIR_M2C;

// Where a picture's pixels are: the frame buffer, or a decoded rectangle or strip in g_out.
struct PtrSurface {
  uint16_t* base;
  bool panel;      // in the panel's portrait orientation (with rotation rot), rows row0.. of it; else landscape
  int rot, row0, rows;
  int x, y, w, h;  // landscape: the logical rectangle it holds
};

static PtrSurface ptr_fb(int rot) { return {g_fb, true, rot, 0, FB_H, 0, 0, 0, 0}; }

static uint16_t* ptr_px(const PtrSurface& s, int lx, int ly) {
  if (lx < 0 || ly < 0 || lx >= SCR_W || ly >= SCR_H) return nullptr;
  if (!s.panel) {
    if (lx < s.x || ly < s.y || lx >= s.x + s.w || ly >= s.y + s.h) return nullptr;
    return s.base + (size_t)(ly - s.y) * s.w + (lx - s.x);
  }
  const int px = s.rot == 1 ? FB_W - 1 - ly : ly, py = (s.rot == 1 ? lx : FB_H - 1 - lx) - s.row0;
  return py >= 0 && py < s.rows ? s.base + (size_t)py * FB_W + px : nullptr;
}

// The bytes of a surface that the box at (bx, by) touches, for the cache.
static void ptr_span(const PtrSurface& s, int bx, int by, const uint8_t** p, size_t* n) {
  const uint16_t* lo = nullptr;
  const uint16_t* hi = nullptr;
  for (const int cy : {by, by + PTR_H - 1})   // the box's corners are its extremes in either orientation
    for (const int cx : {bx, bx + PTR_W - 1}) {
      const int x = constrain(cx, 0, SCR_W - 1), y = constrain(cy, 0, SCR_H - 1);   // (clipped to the surface)
      if (!s.panel) {
        if (s.w <= 0 || s.h <= 0) continue;
        const uint16_t* q = s.base + (size_t)(constrain(y, s.y, s.y + s.h - 1) - s.y) * s.w + (constrain(x, s.x, s.x + s.w - 1) - s.x);
        lo = !lo || q < lo ? q : lo; hi = !hi || q > hi ? q : hi;
        continue;
      }
      const int px = s.rot == 1 ? FB_W - 1 - y : y, py = constrain((s.rot == 1 ? x : FB_H - 1 - x) - s.row0, 0, s.rows - 1);
      const uint16_t* q = s.base + (size_t)py * FB_W + px;
      lo = !lo || q < lo ? q : lo; hi = !hi || q > hi ? q : hi;
    }
  *p = (const uint8_t*)lo;
  *n = lo ? (size_t)(hi - lo + 1) * 2 : 0;
}

static void ptr_cache_box(const PtrSurface& s, int bx, int by, int flags) {
  const uint8_t* p;
  size_t n;
  ptr_span(s, bx, by, &p, &n);
  if (n) ptr_cache(p, n, flags);
}

// Copy the box between a surface and an array (only the pixels the surface holds).
static void ptr_get(const PtrSurface& s, int bx, int by, uint16_t* dst) {
  for (int y = 0; y < PTR_H; ++y)
    for (int x = 0; x < PTR_W; ++x)
      if (const uint16_t* p = ptr_px(s, bx + x, by + y)) dst[y * PTR_W + x] = *p;
}

static void ptr_put(const PtrSurface& s, int bx, int by, const uint16_t* src) {
  for (int y = 0; y < PTR_H; ++y)
    for (int x = 0; x < PTR_W; ++x)
      if (uint16_t* p = ptr_px(s, bx + x, by + y)) *p = src[y * PTR_W + x];
}

static void ptr_erase() {
  if (!g_ptr.drawn) return;
  const PtrSurface fb = ptr_fb(g_ptr.rot);
  ptr_put(fb, g_ptr.bx, g_ptr.by, g_ptr_under);
  ptr_cache_box(fb, g_ptr.bx, g_ptr.by, PTR_TO_RAM);
  g_ptr.drawn = false;
}

static void ptr_draw() {
  const int bx = g_ptr.x - PTR_HOT, by = g_ptr.y - PTR_HOT;
  const PtrSurface fb = ptr_fb(g_rot);
  ptr_cache_box(fb, bx, by, PTR_FROM_RAM);
  ptr_get(fb, bx, by, g_ptr_under);
  ptr_blend(g_ptr_under, g_ptr_out);
  ptr_put(fb, bx, by, g_ptr_out);
  ptr_cache_box(fb, bx, by, PTR_TO_RAM);
  g_ptr.drawn = true;
  g_ptr.bx = bx;
  g_ptr.by = by;
  g_ptr.rot = g_rot;
  ++g_ptr_stats.draws;
}

// Show the pointer where it should be (or not at all): on a shown page, with the settings closed.
static void ptr_update() {
  const bool want = g_ptr.shown && g_have_client && g_shown_any && !g_panel_open;
  if (g_ptr.drawn && (!want || g_ptr.bx != g_ptr.x - PTR_HOT || g_ptr.by != g_ptr.y - PTR_HOT || g_ptr.rot != g_rot))
    ptr_erase();
  if (want && !g_ptr.drawn) ptr_draw();
}

static void ptr_hide() {   // a touch: the finger is the pointer now
  g_ptr.shown = false;
  ptr_erase();
}

// Is the drawn pointer in the logical rectangle?
static bool ptr_over(int x, int y, int w, int h) {
  return g_ptr.drawn && g_ptr.rot == g_rot && g_ptr.bx < x + w && x < g_ptr.bx + PTR_W && g_ptr.by < y + h && y < g_ptr.by + PTR_H;
}

// A decoded rectangle or strip, about to go on screen, holds part of the box: take the new page pixels beneath the
// pointer from it, and blend the arrow into it.
static void ptr_patch(const PtrSurface& s) {
  ptr_cache_box(s, g_ptr.bx, g_ptr.by, PTR_FROM_RAM);
  ptr_get(s, g_ptr.bx, g_ptr.by, g_ptr_under);
  ptr_blend(g_ptr_under, g_ptr_out);
  ptr_put(s, g_ptr.bx, g_ptr.by, g_ptr_out);
  ptr_cache_box(s, g_ptr.bx, g_ptr.by, PTR_TO_RAM);
  ++g_ptr_stats.patches;
}
