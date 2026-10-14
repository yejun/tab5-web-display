// Tab5 Web Display: shows a web page that headless Chromium renders on a PC (server/server.py) on an
// M5Stack Tab5, and sends touches back to it.  MIT licence, see LICENSE.
//
// The Tab5 is the TCP *server* (port 8765) so the PC only makes an outgoing connection (no firewall change).
// PC -> Tab5: updates, each a run of rectangles followed by an end marker. A rectangle is a 16-byte header
//             { "TAB5", u16 x, y, w, h, u32 len } (little endian) and `len` bytes of baseline JPEG; x, y, w and h
//             are multiples of 16. The end marker is a header with w = h = len = 0.
// Tab5 -> PC: 8-byte records { u8 type, u8 0, u16 x, u16 y, u16 0 }: 1 = touch down, 2 = move, 3 = up,
//             5 = update shown (sent after each end marker, so the PC never has more than one update in flight).
//
// Each rectangle is decoded by the P4's hardware JPEG engine to RGB565 and rotated into the panel's portrait
// frame buffer by the PPA (both DMA, the CPU only waits).
//
// Settings (WiFi, brightness, flip): swipe in from any edge, or tap the screen while no page is shown. The panel
// (settings_ui.h, LVGL) also opens by itself when no WiFi network is configured or the saved one can't be joined.
//
// Serial commands: ip | wifi <ssid> <password> | rot | S (stats) | F (dump the frame buffer, see tools/shot.py)
//                  | T <x> <y> (scripted tap) | drag <x0> <y0> <x1> <y1> [ms] | panel (open the settings)
//                  | forget (erase saved settings and restart)
#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <lvgl.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <driver/ppa.h>
#include <driver/jpeg_decode.h>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

static constexpr const char* FIRMWARE_VERSION = "1.0.0";
static constexpr int SCR_W = 1280, SCR_H = 720;   // landscape, as the page is rendered
static constexpr int FB_W = 720, FB_H = 1280;     // the panel's native portrait frame buffer
static constexpr uint16_t PORT = 8765;
static constexpr size_t JPEG_MAX = 1 << 20;        // largest accepted rectangle (a full-screen q90 frame is ~300 KB)
static constexpr int64_t AWAKE_AFTER_TOUCH_MS = 10000;   // WiFi power save stays off this long after a touch
static constexpr int64_t AWAKE_AFTER_STREAM_MS = 3000;   // ... and this long after updates came faster than 1 per 400 ms
static constexpr int64_t CONNECT_TIMEOUT_MS = 20000, RETRY_MS = 30000;
static constexpr int EDGE = 40, SWIPE_IN = 120;           // edge swipe: start within EDGE px, move SWIPE_IN px inwards
static constexpr int64_t SWIPE_MS = 700;

enum : uint8_t { EV_DOWN = 1, EV_MOVE = 2, EV_UP = 3, EV_SHOWN = 5 };

#pragma pack(push, 1)
struct RectHeader { char magic[4]; uint16_t x, y, w, h; uint32_t len; };
struct Event { uint8_t type, pad; uint16_t x, y, pad2; };
#pragma pack(pop)
static_assert(sizeof(RectHeader) == 16 && sizeof(Event) == 8, "wire format");

static Preferences prefs;
static String g_ssid, g_pass;               // the saved network
static int g_rot = 1;                       // 1 = landscape, 3 = upside down
static uint8_t g_bri = 160;
static uint16_t* g_fb = nullptr;
static ppa_client_handle_t g_ppa = nullptr;
static jpeg_decoder_handle_t g_jpeg = nullptr;
static uint8_t* g_in = nullptr;             // compressed rectangle
static uint8_t* g_out = nullptr;            // decoded RGB565 rectangle
static size_t g_in_cap = 0, g_out_cap = 0;
static WiFiServer g_server(PORT);
static WiFiClient g_client;
static bool g_have_client = false;          // (WiFiClient's operator bool means "connected", so track it ourselves)
static bool g_shown_any = false;            // has the current connection drawn anything yet?
static bool g_panel_open = false;
static int64_t g_last_touch = -AWAKE_AFTER_TOUCH_MS;
static int64_t g_last_update = 0, g_last_stream = -AWAKE_AFTER_STREAM_MS;
static bool g_wifi_sleep = true;

enum WifiState { WIFI_NONE, WIFI_CONNECTING, WIFI_UP, WIFI_FAILED };
static WifiState g_wifi = WIFI_NONE;
static int64_t g_wifi_t0 = 0;
static String g_try_ssid, g_try_pass;       // the network being joined; saved once that works
static bool g_attempt_failed = false;       // the last wifi_start() didn't work (for the settings dialog)

struct TouchPoint { bool pressed; int x, y; };             // (types up here: Arduino puts prototypes above code)
struct TouchScript { int64_t t0; int ms, x0, y0, x1, y1; };
static TouchScript g_script = {0, 0, 0, 0, 0, 0};

struct Stats { uint32_t updates, rects; uint64_t bytes, decode_us, ppa_us, recv_us; };
static Stats g_stats = {};

static int64_t now_ms() { return esp_timer_get_time() / 1000; }
static void panel_open();

// ---- display ------------------------------------------------------------------------------------------------

// Text drawn by M5GFX goes through the CPU cache; write it back (and drop the lines) so a later DMA write by the
// PPA can't be overwritten by stale cache lines.
static void fb_sync() {
  if (g_fb) esp_cache_msync(g_fb, (size_t)FB_W * FB_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

// Full-screen message (or, with banner, a bar along the bottom over the last page image).
static void show_status(const char* line1, const char* line2, bool banner = false) {
  auto& d = M5.Display;
  const int y0 = banner ? SCR_H - 64 : 0;
  d.fillRect(0, y0, SCR_W, banner ? 64 : SCR_H, banner ? 0x3186 : 0x10A3);
  d.setTextColor(0xEF3B);
  d.setTextDatum(banner ? middle_left : middle_center);
  d.setFont(&fonts::FreeSans18pt7b);
  if (banner) {
    d.drawString(line1, 24, y0 + 32);
  } else {
    d.drawString(line1, SCR_W / 2, SCR_H / 2 - 40);
    d.setTextColor(0x8C94);
    d.setFont(&fonts::FreeSans12pt7b);
    d.drawString(line2, SCR_W / 2, SCR_H / 2 + 20);
    d.drawString("Tap the screen for settings", SCR_W / 2, SCR_H - 60);
  }
  d.display();
  fb_sync();
  Serial.printf("status: %s %s\n", line1, line2);
}

// What to show while no page is on screen (and the settings are closed).
static void status_screen() {
  if (g_panel_open) return;
  char a[96], b[128];
  switch (g_wifi) {
    case WIFI_NONE:       show_status("WiFi is not set up", "Open the settings to choose a network"); break;
    case WIFI_CONNECTING: snprintf(a, sizeof(a), "Connecting to %s ...", g_try_ssid.c_str()); show_status(a, ""); break;
    case WIFI_FAILED:     snprintf(a, sizeof(a), "Could not connect to %s", g_try_ssid.c_str());
                          show_status(a, "Retrying every 30 s"); break;
    case WIFI_UP: {
      const String ip = WiFi.localIP().toString();
      snprintf(a, sizeof(a), "Waiting for the PC at %s", ip.c_str());
      snprintf(b, sizeof(b), "On the PC:  python server/server.py --device %s", ip.c_str());
      show_status(a, b);
      break;
    }
  }
}

static bool hw_setup() {
  auto* panel = static_cast<lgfx::Panel_DSI*>(M5.Display.getPanel());
  g_fb = panel ? (uint16_t*)panel->config_detail().buffer : nullptr;
  ppa_client_config_t pc = {};
  pc.oper_type = PPA_OPERATION_SRM;
  jpeg_decode_engine_cfg_t jc = {};
  jc.timeout_ms = 200;
  jpeg_decode_memory_alloc_cfg_t in_cfg = {JPEG_DEC_ALLOC_INPUT_BUFFER}, out_cfg = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  g_in = (uint8_t*)jpeg_alloc_decoder_mem(JPEG_MAX, &in_cfg, &g_in_cap);
  g_out = (uint8_t*)jpeg_alloc_decoder_mem((size_t)SCR_W * SCR_H * 2, &out_cfg, &g_out_cap);
  return g_fb && g_in && g_out && ppa_register_client(&pc, &g_ppa) == ESP_OK && jpeg_new_decoder_engine(&jc, &g_jpeg) == ESP_OK;
}

// Rotate a w x h landscape RGB565 rectangle into the portrait frame buffer at logical (x, y).
static bool ppa_blit(const void* src, int x, int y, int w, int h) {
  ppa_srm_oper_config_t op = {};
  op.in.buffer = src;
  op.in.pic_w = op.in.block_w = w;
  op.in.pic_h = op.in.block_h = h;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = g_fb;
  op.out.buffer_size = FB_W * FB_H * 2;
  op.out.pic_w = FB_W;
  op.out.pic_h = FB_H;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  if (g_rot == 1) {           // logical (x, y) -> panel (719 - y, x)
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    op.out.block_offset_x = FB_W - (y + h);
    op.out.block_offset_y = x;
  } else {                    // logical (x, y) -> panel (y, 1279 - x)
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    op.out.block_offset_x = y;
    op.out.block_offset_y = FB_H - (x + w);
  }
  op.scale_x = op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  return ppa_do_scale_rotate_mirror(g_ppa, &op) == ESP_OK;
}

// ---- PC connection ------------------------------------------------------------------------------------------

static void send_event(uint8_t type, int x, int y) {
  if (!g_have_client || !g_client.connected()) return;
  const Event e = {type, 0, (uint16_t)x, (uint16_t)y, 0};
  g_client.write((const uint8_t*)&e, sizeof(e));
}

static void drop_client() {
  if (g_have_client) g_client.stop();
  g_have_client = g_shown_any = false;
}

static bool read_exact(uint8_t* p, size_t n) {
  size_t got = 0;
  const int64_t deadline = now_ms() + 3000;
  while (got < n && g_client.connected() && now_ms() < deadline) {
    const int r = g_client.read(p + got, n - got);
    if (r > 0) got += r;
    else delay(1);
  }
  return got == n;
}

// Receive and draw one rectangle (or the end marker). Returns false if the connection must be dropped.
static bool handle_rect() {
  RectHeader h;
  const int64_t t0 = esp_timer_get_time();
  if (!read_exact((uint8_t*)&h, sizeof(h)) || memcmp(h.magic, "TAB5", 4) != 0) { Serial.println("ERR bad header"); return false; }
  if (h.w == 0 && h.h == 0) {   // end of update
    // continuous changes (an animation) arrive much faster under modem sleep's ~100 ms wake-ups if the radio stays on;
    // a once-a-second clock tick doesn't count
    const int64_t now = now_ms();
    if (now - g_last_update < 400) g_last_stream = now;
    g_last_update = now;
    g_stats.updates++;
    g_shown_any = true;
    send_event(EV_SHOWN, 0, 0);
    return true;
  }
  if (h.len == 0 || h.len > g_in_cap || h.x % 16 || h.y % 16 || h.w % 16 || h.h % 16 || h.w == 0 || h.h == 0 ||
      h.x + h.w > SCR_W || h.y + h.h > SCR_H) {
    Serial.printf("ERR bad rect %u,%u %ux%u len %u\n", h.x, h.y, h.w, h.h, (unsigned)h.len);
    return false;
  }
  if (!read_exact(g_in, h.len)) { Serial.println("ERR short read"); return false; }
  const int64_t t1 = esp_timer_get_time();
  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;   // little-endian RGB565, as the panel expects
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t out_size = 0;
  if (jpeg_decoder_process(g_jpeg, &cfg, g_in, h.len, g_out, g_out_cap, &out_size) != ESP_OK ||
      out_size < (uint32_t)h.w * h.h * 2) {
    Serial.printf("ERR decode %ux%u (%u bytes out)\n", h.w, h.h, (unsigned)out_size);
    return true;   // skip this rectangle but keep the connection
  }
  const int64_t t2 = esp_timer_get_time();
  if (!ppa_blit(g_out, h.x, h.y, h.w, h.h)) Serial.println("ERR ppa");
  const int64_t t3 = esp_timer_get_time();
  g_stats.rects++;
  g_stats.bytes += h.len;
  g_stats.recv_us += t1 - t0;
  g_stats.decode_us += t2 - t1;
  g_stats.ppa_us += t3 - t2;
  return true;
}

static void pc_poll() {
  WiFiClient incoming = g_server.accept();
  if (incoming) {
    if (g_panel_open) { incoming.stop(); return; }   // the PC retries every 2 s; it gets a full frame once we're back
    drop_client();                                    // the newest PC connection wins
    g_client = incoming;
    g_client.setNoDelay(true);
    g_have_client = true;
    Serial.printf("PC connected from %s\n", g_client.remoteIP().toString().c_str());
  }
  if (!g_have_client) return;
  if (!g_client.connected()) {
    const bool had_page = g_shown_any;
    drop_client();
    Serial.println("PC disconnected");
    if (!g_panel_open) {
      if (had_page) {
        char msg[96];
        snprintf(msg, sizeof(msg), "PC disconnected - waiting at %s:%u", WiFi.localIP().toString().c_str(), PORT);
        show_status(msg, "", true);
      } else {
        status_screen();
      }
    }
    return;
  }
  while (g_client.available() >= (int)sizeof(RectHeader))
    if (!handle_rect()) { g_client.stop(); break; }
}

// ---- WiFi ---------------------------------------------------------------------------------------------------

static void set_wifi_sleep(bool on) {
  if (on == g_wifi_sleep) return;
  g_wifi_sleep = on;
  WiFi.setSleep(on);
}

// Start joining a network (non-blocking; wifi_poll() follows it up). The credentials are saved once it works.
static void wifi_start(const String& ssid, const String& pass) {
  drop_client();
  g_try_ssid = ssid;
  g_try_pass = pass;
  g_attempt_failed = false;
  WiFi.disconnect(false, false);   // keep the WiFi stack up: re-initialising ESP-Hosted is unreliable on the Tab5
  WiFi.begin(ssid.c_str(), pass.c_str());
  g_wifi = WIFI_CONNECTING;
  g_wifi_t0 = now_ms();
  Serial.printf("wifi: connecting to %s\n", ssid.c_str());
  status_screen();
}

static void wifi_poll() {
  static bool listening = false;
  const wl_status_t st = WiFi.status();
  const int64_t now = now_ms();
  switch (g_wifi) {
    case WIFI_CONNECTING:
      if (st == WL_CONNECTED) {
        g_wifi = WIFI_UP;
        if (g_try_ssid != g_ssid || g_try_pass != g_pass) {
          g_ssid = g_try_ssid;
          g_pass = g_try_pass;
          prefs.putString("ssid", g_ssid);
          prefs.putString("pass", g_pass);
        }
        if (!listening) { g_server.begin(); g_server.setNoDelay(true); listening = true; }
        g_wifi_sleep = !g_wifi_sleep;   // force the next call to apply
        set_wifi_sleep(true);
        Serial.printf("IP %s\n", WiFi.localIP().toString().c_str());
        status_screen();
      } else if (now - g_wifi_t0 > CONNECT_TIMEOUT_MS ||
                 ((st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) && now - g_wifi_t0 > 4000)) {
        Serial.printf("wifi: could not connect to %s (status %d)\n", g_try_ssid.c_str(), (int)st);
        if (!g_ssid.isEmpty() && g_try_ssid != g_ssid) {
          wifi_start(g_ssid, g_pass);   // a new network failed: go back to the saved one
        } else {
          g_wifi = WIFI_FAILED;
          g_wifi_t0 = now;
          WiFi.disconnect(false, false);
          panel_open();                 // the saved network doesn't work: let the user fix it
          status_screen();
        }
        g_attempt_failed = true;
      }
      break;
    case WIFI_UP:
      if (st != WL_CONNECTED) {      // lost; the driver reconnects by itself
        drop_client();
        g_wifi = WIFI_CONNECTING;
        g_wifi_t0 = now;
        g_try_ssid = g_ssid;
        g_try_pass = g_pass;
        status_screen();
      }
      break;
    case WIFI_FAILED:
      if (!g_panel_open && !g_ssid.isEmpty() && now - g_wifi_t0 > RETRY_MS) wifi_start(g_ssid, g_pass);
      break;
    case WIFI_NONE:
      break;
  }
}

// Touch input. A scripted gesture (serial "T" / "drag") can stand in for a finger, for testing without one.

static TouchPoint read_touch() {
  const int64_t dt = now_ms() - g_script.t0;
  if (dt >= 0 && dt < g_script.ms) {
    const float f = (float)dt / g_script.ms;
    return {true, g_script.x0 + (int)((g_script.x1 - g_script.x0) * f), g_script.y0 + (int)((g_script.y1 - g_script.y0) * f)};
  }
  const auto t = M5.Touch.getDetail();
  return {t.isPressed(), t.x, t.y};
}

static void script_touch(int x0, int y0, int x1, int y1, int ms) { g_script = {now_ms(), ms, x0, y0, x1, y1}; }

#include "settings_ui.h"

// ---- touch on the mirrored page -------------------------------------------------------------------------------
// Touches are forwarded to the PC, except a swipe that starts at an edge and moves inwards: that opens the settings.
// A touch that starts in the edge strip is held back until it is clear which of the two it is.

static void handle_touch() {
  enum { IDLE, EDGE_PENDING, FORWARDING, LOCAL, IGNORE };
  static int st = IDLE;
  static int sx, sy, lx, ly;
  static int64_t t0, last_move;
  const TouchPoint t = read_touch();
  const int64_t now = now_ms();
  const bool page = g_have_client && g_shown_any;

  if (!t.pressed) {
    if (st == FORWARDING) send_event(EV_UP, lx, ly);
    else if (st == EDGE_PENDING) { send_event(EV_DOWN, sx, sy); send_event(EV_UP, sx, sy); }   // it was a tap
    else if (st == LOCAL) panel_open();
    st = IDLE;
    return;
  }
  g_last_touch = now;
  if (st == IDLE) {
    sx = lx = t.x; sy = ly = t.y; t0 = last_move = now;
    if (!page) st = LOCAL;   // nothing mirrored: any tap opens the settings
    else if (t.x < EDGE || t.x >= SCR_W - EDGE || t.y < EDGE || t.y >= SCR_H - EDGE) st = EDGE_PENDING;
    else { st = FORWARDING; send_event(EV_DOWN, t.x, t.y); }
    return;
  }
  if (st == EDGE_PENDING) {
    // inward distance from the edge the touch started at
    const int in = sx < EDGE ? t.x - sx : sx >= SCR_W - EDGE ? sx - t.x : sy < EDGE ? t.y - sy : sy - t.y;
    const int along = (sx < EDGE || sx >= SCR_W - EDGE) ? abs(t.y - sy) : abs(t.x - sx);
    if (in >= SWIPE_IN && now - t0 < SWIPE_MS) { st = IGNORE; panel_open(); return; }
    if (now - t0 >= SWIPE_MS || along > SWIPE_IN / 2) {   // not a swipe: hand the whole touch to the page
      st = FORWARDING;
      send_event(EV_DOWN, sx, sy);
    } else {
      return;
    }
  }
  if (st == FORWARDING && (abs(t.x - lx) >= 2 || abs(t.y - ly) >= 2) && now - last_move >= 16) {
    lx = t.x; ly = t.y; last_move = now;
    send_event(EV_MOVE, t.x, t.y);
  }
}

// ---- serial -------------------------------------------------------------------------------------------------

static void dump_framebuffer() {
  const size_t n = (size_t)FB_W * FB_H * 2;
  esp_cache_msync(g_fb, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  Serial.setTxTimeoutMs(200);   // wait for the host instead of dropping bytes
  Serial.printf("FB %d %d %u\n", FB_W, FB_H, (unsigned)n);
  const uint8_t* p = (const uint8_t*)g_fb;
  for (size_t off = 0; off < n;) {
    const size_t w = Serial.write(p + off, min((size_t)4096, n - off));
    if (w == 0) delay(1);
    off += w;
  }
  Serial.println();
  Serial.setTxTimeoutMs(0);
}

static void print_stats() {
  const Stats s = g_stats;
  const uint32_t r = s.rects ? s.rects : 1;
  Serial.printf("v%s | updates %u rects %u bytes %llu | per rect: recv %.1f ms decode %.1f ms ppa %.1f ms | wifi rssi %d sleep %s | "
                "psram free %u\n",
                FIRMWARE_VERSION, (unsigned)s.updates, (unsigned)s.rects, (unsigned long long)s.bytes, s.recv_us / 1000.0 / r,
                s.decode_us / 1000.0 / r, s.ppa_us / 1000.0 / r, (int)WiFi.RSSI(), g_wifi_sleep ? "on" : "off",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  g_stats = {};
}

static void handle_serial() {
  static String line;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (line.length() < 200) line += c; continue; }
    line.trim();
    if (line == "ip") {
      Serial.printf("IP %s\n", g_wifi == WIFI_UP ? WiFi.localIP().toString().c_str() : "none");
    } else if (line == "S") {
      print_stats();
    } else if (line == "F") {
      dump_framebuffer();
    } else if (line == "panel") {
      panel_open();
    } else if (line == "forget") {         // erase all saved settings (WiFi, brightness, flip) and restart
      prefs.clear();
      Serial.println("settings erased, restarting");
      delay(100);
      ESP.restart();
    } else if (line == "unconfigured") {   // test: behave as on a first start (nothing saved; undone by a reboot)
      drop_client();
      WiFi.disconnect(false, false);
      g_ssid = g_pass = "";
      g_wifi = WIFI_NONE;
      panel_open();
    } else if (line.startsWith("T ")) {   // synthetic tap, for measuring latency without a finger
      int x = 0, y = 0;
      sscanf(line.c_str() + 2, "%d %d", &x, &y);
      g_last_touch = now_ms();
      set_wifi_sleep(false);
      script_touch(x, y, x, y, 80);
      Serial.printf("tap %d %d\n", x, y);
    } else if (line.startsWith("drag ")) {   // drag x0 y0 x1 y1 [ms]: scripted finger movement
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0, ms = 300;
      sscanf(line.c_str() + 5, "%d %d %d %d %d", &x0, &y0, &x1, &y1, &ms);
      script_touch(x0, y0, x1, y1, ms);
      Serial.printf("drag %d,%d -> %d,%d in %d ms\n", x0, y0, x1, y1, ms);
    } else if (line == "rot") {
      set_rotation(g_rot == 1 ? 3 : 1);
      Serial.printf("rotation %d\n", g_rot);
    } else if (line.startsWith("wifi ")) {
      const int sp = line.indexOf(' ', 5);
      wifi_start(sp > 0 ? line.substring(5, sp) : line.substring(5), sp > 0 ? line.substring(sp + 1) : "");
    } else if (line.length()) {
      Serial.println("commands: ip | wifi <ssid> <password> | rot | S | F | T <x> <y> | drag x0 y0 x1 y1 [ms] | panel | forget");
    }
    line = "";
  }
}

// ---- main ---------------------------------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.output_power = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = false;
  M5.begin(cfg);
  Serial.printf("\nTab5 Web Display %s\n", FIRMWARE_VERSION);
  prefs.begin("webdisp", false);
  g_rot = prefs.getInt("rot", 1) == 3 ? 3 : 1;
  g_bri = (uint8_t)constrain((int)prefs.getUChar("bri", 160), 10, 255);
  M5.Display.setRotation(g_rot);
  M5.Display.setBrightness(g_bri);
  if (!hw_setup()) {
    show_status("Hardware setup failed", "JPEG decoder / PPA / memory");
    for (;;) delay(1000);
  }
  ui_setup();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  g_ssid = prefs.getString("ssid", "");
  g_pass = prefs.getString("pass", "");
  if (g_ssid.isEmpty()) panel_open();   // first start: straight to the WiFi setup
  else wifi_start(g_ssid, g_pass);
}

void loop() {
  M5.update();
  handle_serial();
  wifi_poll();
  if (g_wifi == WIFI_UP) pc_poll();
  if (g_panel_open) {
    ui_poll();
    lv_timer_handler();
  } else {
    handle_touch();
  }
  set_wifi_sleep(!g_panel_open && now_ms() - g_last_touch > AWAKE_AFTER_TOUCH_MS &&
                 now_ms() - g_last_stream > AWAKE_AFTER_STREAM_MS);
  delay(5);
}
