// Tab5 Web Display: shows a web page that headless Chromium renders on a PC (server/server.py) on an
// M5Stack Tab5, and sends touches back to it.  MIT licence, see LICENSE.
//
// The Tab5 is the TCP *server* (port 8765) so the PC only makes an outgoing connection (no firewall change).
// PC -> Tab5: updates, each a run of rectangles followed by an end marker. A rectangle is a 16-byte header
//             { "TAB5", u16 x, y, w, h, u32 len } (little endian) and `len` bytes of baseline JPEG; x, y, w and h
//             are multiples of 16. The end marker is a header with w = h = len = 0.
//             A header with magic "TCFG" instead is settings: x = WiFi power save (0 auto, 1 always off),
//             y bits: 1 = send every finger (multi-touch), not only the first; 2 = send gestures (below).
//             Magic "TPTS" (firmware 1.3.2, only on lanes) times the update that follows: x | y << 16 = when it is due on
//             the PC's clock (ms, wrapping), w = how many ms before that the PC sent it (signed). See pacing below.
//             Magic "TABS" is a strip: landscape columns x..x+w at full height (y = 0, h = 720), sent as a
//             720-wide, w-tall JPEG already rotated into the panel's portrait orientation, and decoded straight
//             into the frame buffer (see handle_strip).
// Tab5 -> PC: 8-byte records { u8 type, u8 flags, u16 x, u16 y, u16 z }: 1 = touch down, 2 = move, 3 = up
//             (with multi-touch on, flags = the finger, 0-4; otherwise only the first finger, flags 0),
//             5 = update shown (sent after each end marker, so the PC never has more than one update in flight;
//             firmware 1.3.2: x = 1 + how many ms late a timed update began, 0 for an untimed one),
//             6 = hello, sent on connect: x.y.z = firmware version; flags: 1 = can play sound, 2 = takes strips,
//             4 = screen flipped 180°. The PC sends "TCFG" and "TABS" only after a hello, so older firmware never
//             sees them. 7 = lanes (offer and answer): the stream can continue over several connections, see lanes.h.
//             8 = nudge: nothing, sent while the stream is on lanes so that a lost event is resent quickly.
//             9 = key (keyboard.h): flags 1 = down, 0 = up; x = USB HID usage code, y = HID modifier bits.
//             10 = gesture (only if the PC asked for them): x = 1 swipe in from the left edge, 2 from the right,
//             3 from the bottom, 4 three fingers swiped right, 5 three fingers swiped left. The PC switches and
//             closes browser tabs, and goes back and forward, with them.
// Sound arrives separately as UDP, see audio.h.
//
// Each rectangle is decoded by the P4's hardware JPEG engine to RGB565 and rotated into the panel's portrait
// frame buffer by the PPA (both DMA, the CPU only waits).
//
// Settings (WiFi, brightness, volume, flip): swipe in from the top edge (from any edge with a PC that doesn't take
// gestures, and while no page is shown), or tap the screen while no page is shown. The panel
// (settings_ui.h, LVGL) also opens by itself when no WiFi network is configured or the saved one can't be joined.
//
// Serial commands: ip | wifi <ssid> <password> | wifiinfo | rot | rot auto | S (stats) | F (dump the frame buffer, see tools/shot.py)
//                  | T <x> <y> (scripted tap) | drag <x0> <y0> <x1> <y1> [ms] | pinch <cx> <cy> <d0> <d1> [ms]
//                  | three <x0> <x1> <y> [ms] (three-finger swipe)
//                  | panel (open the settings)
//                  | beep (speaker check) | forget (erase saved settings and restart)
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
#include <esp_wifi.h>

static constexpr const char* FIRMWARE_VERSION = "1.3.2";
static constexpr int SCR_W = 1280, SCR_H = 720;   // landscape, as the page is rendered
static constexpr int FB_W = 720, FB_H = 1280;     // the panel's native portrait frame buffer
static constexpr uint16_t PORT = 8765;
static constexpr size_t JPEG_MAX = 1 << 20;        // largest accepted rectangle (a full-screen q90 frame is ~300 KB)
static constexpr int64_t AWAKE_AFTER_TOUCH_MS = 10000;   // WiFi power save stays off this long after a touch
static constexpr int64_t AWAKE_AFTER_STREAM_MS = 3000;   // ... and this long after updates came faster than 1 per 400 ms
static constexpr int64_t AWAKE_AFTER_SOUND_MS = 1000;    // ... and this long after sound
static constexpr int64_t CONNECT_TIMEOUT_MS = 20000, RETRY_MS = 30000;
static constexpr int EDGE = 40, SWIPE_IN = 120;           // edge swipe: start within EDGE px, move SWIPE_IN px inwards
static constexpr int64_t SWIPE_MS = 700;
static constexpr int SWIPE3 = 150;                    // three fingers moved this far sideways: back / forward

enum : uint8_t { EV_DOWN = 1, EV_MOVE = 2, EV_UP = 3, EV_SHOWN = 5, EV_HELLO = 6, EV_LANES = 7, EV_NUDGE = 8, EV_KEY = 9, EV_GESTURE = 10 };
enum : uint8_t { GEST_LEFT = 1, GEST_RIGHT = 2, GEST_BOTTOM = 3, GEST_THREE_RIGHT = 4, GEST_THREE_LEFT = 5 };
enum : uint8_t { HELLO_SOUND = 1, HELLO_STRIPS = 2, HELLO_FLIPPED = 4 };   // hello flags

#pragma pack(push, 1)
struct RectHeader { char magic[4]; uint16_t x, y, w, h; uint32_t len; };
struct Event { uint8_t type, flags; uint16_t x, y, z; };
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
static uint8_t g_power_mode = 0;          // from the PC: 0 = auto, 1 = power save always off
static bool g_multitouch = false;         // from the PC: it takes every finger
static bool g_gestures = false;           // from the PC: it takes edge and three-finger swipes (tabs, back)
static uint8_t g_kled = 10;               // keyboard light brightness, 0 (off) to 100

enum WifiState { WIFI_NONE, WIFI_CONNECTING, WIFI_UP, WIFI_FAILED };
static WifiState g_wifi = WIFI_NONE;
static int64_t g_wifi_t0 = 0;
static String g_try_ssid, g_try_pass;       // the network being joined; saved once that works
static bool g_attempt_failed = false;       // the last wifi_start() didn't work (for the settings dialog)

struct TouchPoint { bool pressed; int x, y; };             // (types up here: Arduino puts prototypes above code)
struct Finger { int id, x, y; };
struct TouchScript { int64_t t0; int ms, x0, y0, x1, y1; };
static TouchScript g_script = {0, 0, 0, 0, 0, 0};
static TouchScript g_script2 = {0, 0, 0, 0, 0, 0};   // a second finger (serial "pinch", "three")
static TouchScript g_script3 = {0, 0, 0, 0, 0, 0};   // a third (serial "three")

struct Stats { uint32_t updates, rects, strips; uint64_t bytes, decode_us, ppa_us, recv_us;
               uint32_t gaps[32], gap_max; };   // time between updates shown while streaming, in 10 ms steps
static Stats g_stats = {};

static int64_t now_ms() { return esp_timer_get_time() / 1000; }
static void panel_open();
static void kbd_set_light(uint8_t level);

#include "audio.h"

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

static int64_t g_event_sent = 0;   // when the last event went to the PC

// ---- pacing: a playout buffer for video ----
// WiFi delivers frames in bunches: sent evenly, they arrived 5 ms apart and then 60 ms apart, and drawn on arrival a
// 30 fps video skipped a frame on the screen every fifth or so. With "TPTS", the PC sends video frames ahead of time,
// each with its due time; the Tab5 holds each until then, so they come out evenly. Its clock and the PC's are tied by
// the fastest deliveries seen (arrival - due + how early it was sent), like the sound's jitter buffer. For a second
// after a touch or a key, frames are drawn as they come, so nothing waits that the user is acting on.
static constexpr int64_t PACE_TOUCH_MS = 1000;
struct Pacing { bool due_on = false, off_ok = false; int64_t due = 0, rise_at = 0; uint32_t off = 0;
                uint16_t shown_x = 0; };   // for the update's "shown": 0 untimed, else 1 + how late it began (ms)
static Pacing g_pace;
struct PaceStats { uint32_t timed, late; int64_t early_ms; };
static PaceStats g_pstats = {};

static void send_event(uint8_t type, int x, int y, uint8_t flags = 0, uint16_t z = 0) {
  if (!g_have_client || !g_client.connected()) return;
  const Event e = {type, flags, (uint16_t)x, (uint16_t)y, z};
  g_client.write((const uint8_t*)&e, sizeof(e));
  g_event_sent = esp_timer_get_time() / 1000;
}

#include "lanes.h"

static void drop_client() {
  lanes_close();
  if (g_have_client) g_client.stop();
  g_have_client = g_shown_any = false;
  g_pc_ip = 0;
  g_power_mode = 0;
}

static bool read_exact(uint8_t* p, size_t n) {
  if (g_lanes_on) return lanes_read(p, n);
  size_t got = 0;
  const int64_t deadline = now_ms() + 3000;
  while (got < n && g_client.connected() && now_ms() < deadline) {
    const int r = g_client.read(p + got, n - got);
    if (r > 0) got += r;
    else delay(1);
  }
  return got == n;
}

// A strip: landscape columns x..x+w at full height, which are whole rows of the portrait frame buffer, so the JPEG
// (rotated by the PC to suit g_rot) is decoded straight into place. That skips the PPA copy, which costs more than
// decoding: PSRAM bandwidth is the limit, and the copy reads and writes every pixel once more.
static bool handle_strip(const RectHeader& h) {
  if (h.y != 0 || h.h != SCR_H || h.x % 16 || h.w % 16 || h.w == 0 || h.x + h.w > SCR_W || h.len == 0 || h.len > g_in_cap) {
    Serial.printf("ERR bad strip %u %ux%u len %u\n", h.x, h.w, h.h, (unsigned)h.len);
    return false;
  }
  const int64_t t0 = esp_timer_get_time();
  if (!read_exact(g_in, h.len)) { Serial.println("ERR short read"); return false; }
  const int64_t t1 = esp_timer_get_time();
  const int row = g_rot == 1 ? h.x : SCR_W - h.x - h.w;   // first frame buffer row
  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  uint32_t out_size = 0;
  if (jpeg_decoder_process(g_jpeg, &cfg, g_in, h.len, (uint8_t*)(g_fb + (size_t)row * FB_W), (size_t)h.w * FB_W * 2,
                           &out_size) != ESP_OK)
    Serial.printf("ERR decode strip %u (%u bytes out)\n", h.w, (unsigned)out_size);
  g_stats.strips++;
  g_stats.bytes += h.len;
  g_stats.recv_us += t1 - t0;
  g_stats.decode_us += esp_timer_get_time() - t1;
  return true;
}

static bool g_update_done = false;   // handle_rect() just finished an update

static void pace_stamp(const RectHeader& h) {
  Pacing& p = g_pace;
  const int64_t now = now_ms();
  const uint32_t due_pc = h.x | (uint32_t)h.y << 16, now32 = (uint32_t)now;
  const uint32_t sample = now32 - due_pc + (uint32_t)(int32_t)(int16_t)h.w;   // our clock minus the PC's, plus the delay
  const int32_t d = (int32_t)(sample - p.off);
  if (!p.off_ok || d < 0 || d > 2000) { p.off = sample; p.off_ok = true; }   // faster than ever (or the PC restarted)
  if (now >= p.rise_at) { p.off += 1; p.rise_at = now + 2000; }              // follow a clock drifting the other way
  p.due = now + (int32_t)(due_pc + p.off - now32);
  p.due_on = true;
  g_pstats.timed++;
  if (p.due <= now) g_pstats.late++;
  else g_pstats.early_ms += p.due - now;
}

// Receive and draw one rectangle (or the end marker). Returns false if the connection must be dropped.
static bool handle_rect() {
  RectHeader h;
  const int64_t t0 = esp_timer_get_time();
  if (!read_exact((uint8_t*)&h, sizeof(h))) { Serial.println("ERR short header"); return false; }
  if (memcmp(h.magic, "TCFG", 4) == 0) {   // a setting
    g_power_mode = h.x == 1 ? 1 : 0;
    g_multitouch = h.y & 1;
    g_gestures = h.y & 2;
    Serial.printf("PC: WiFi power save %s, %s%s\n", g_power_mode ? "off" : "auto", g_multitouch ? "multi-touch" : "one finger",
                  g_gestures ? ", gestures" : "");
    return true;
  }
  if (memcmp(h.magic, "TPTS", 4) == 0) { pace_stamp(h); return true; }
  if (memcmp(h.magic, "TABS", 4) == 0) return handle_strip(h);
  if (memcmp(h.magic, "TSTR", 4) == 0) { lanes_start(h); return true; }
  if (memcmp(h.magic, "TAB5", 4) != 0) { Serial.println("ERR bad header"); return false; }
  if (h.w == 0 && h.h == 0) {   // end of update
    // continuous changes (an animation) arrive much faster under modem sleep's ~100 ms wake-ups if the radio stays on;
    // a once-a-second clock tick doesn't count
    const int64_t now = now_ms();
    if (now - g_last_update < 400) g_last_stream = now;
    if (const int64_t gap = now - g_last_update; gap < 500) {   // frame pacing: how evenly updates reach the screen
      g_stats.gaps[min(gap / 10, (int64_t)31)]++;
      g_stats.gap_max = max(g_stats.gap_max, (uint32_t)gap);
    }
    g_last_update = now;
    g_stats.updates++;
    g_shown_any = true;
    send_event(EV_SHOWN, g_pace.shown_x, 0);   // (x: firmware 1.3.2, timed updates)
    g_pace.shown_x = 0;
    g_update_done = true;
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
    g_multitouch = g_gestures = false;                // until the PC asks for them
    g_pace = {};
    g_pc_ip = (uint32_t)g_client.remoteIP();
    Serial.printf("PC connected from %s\n", g_client.remoteIP().toString().c_str());
    send_event(EV_HELLO, 1, 3, HELLO_SOUND | HELLO_STRIPS | (g_rot == 3 ? HELLO_FLIPPED : 0), 2);   // version 1.3.2
    lanes_offer();
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
  // one update at a time: while the PC keeps the link busy (a video), the next one is always waiting, and touches
  // and sound must still get their turn in between
  lanes_accept();
  if (lanes_failed()) { Serial.println("ERR lane closed"); g_client.stop(); return; }
  // With the stream on the lanes, the main connection only carries our events. When one is lost, nothing follows it,
  // and lwIP resends only on its retransmission timer, which is at least a second (its 500 ms ticks): measured, the
  // PC waited 1.1-1.4 s for an update's "shown" several times a minute. So while updates or touches come and go,
  // a nudge follows every event within 20 ms: a lost one is then followed by others, the PC answers with
  // duplicate acks, and lwIP resends it at once.
  const int64_t now = now_ms();
  if (g_lanes_on && now - g_event_sent >= 20 && (now - g_last_update < 500 || now - g_last_touch < 500))
    send_event(EV_NUDGE, 0, 0);
  g_update_done = false;
  while (!g_update_done && (g_lanes_on ? lanes_ready() : g_client.available() >= (int)sizeof(RectHeader))) {
    if (g_pace.due_on) {   // a timed update: wait for its turn (it stays in the lane buffers meanwhile)
      const int64_t now = now_ms();
      if (now < g_pace.due && now - g_last_touch > PACE_TOUCH_MS) break;
      g_pace.due_on = false;
      g_pace.shown_x = 1 + (uint16_t)constrain(now - g_pace.due, (int64_t)0, (int64_t)60000);
    }
    if (!handle_rect()) { g_client.stop(); break; }
  }
}

// ---- WiFi ---------------------------------------------------------------------------------------------------

// Serial "wifiinfo": the WiFi standard in use. The Tab5 does WiFi 6 only if the router offers it on that network,
// on a 20 MHz channel; otherwise WiFi 4, which carries noticeably less.
static void wifi_info() {
  wifi_phy_mode_t phy;
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_negotiated_phymode(&phy) != ESP_OK || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    Serial.println("wifi: not connected");
    return;
  }
  const char* name = phy == WIFI_PHY_MODE_HE20 ? "WiFi 6 (802.11ax, 20 MHz)" : phy == WIFI_PHY_MODE_HT40 ? "WiFi 4 (802.11n, 40 MHz)"
                   : phy == WIFI_PHY_MODE_HT20 ? "WiFi 4 (802.11n, 20 MHz)" : phy == WIFI_PHY_MODE_11G ? "802.11g" : "802.11b";
  Serial.printf("wifi: %s on channel %d, %d dBm; the network %s WiFi 6\n", name, ap.primary, ap.rssi,
                ap.phy_11ax ? "offers" : "does not offer");
}

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
        audio_start();
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

// The fingers on the screen, up to 5, with the touch controller's numbers (stable while a finger stays down).
static int read_fingers(Finger* f) {
  const int64_t dt = now_ms() - g_script.t0;
  if (dt >= 0 && dt < g_script.ms) {
    const TouchPoint t = read_touch();
    f[0] = {0, t.x, t.y};
    int n = 1;
    const float k = (float)dt / g_script.ms;
    for (const TouchScript* b : {&g_script2, &g_script3})
      if (b->t0 == g_script.t0) { f[n] = {n, b->x0 + (int)((b->x1 - b->x0) * k), b->y0 + (int)((b->y1 - b->y0) * k)}; ++n; }
    return n;
  }
  int n = 0;
  for (int i = 0; i < M5.Touch.getCount() && n < 5; ++i) {
    const auto& d = M5.Touch.getDetail(i);
    if (d.isPressed()) f[n++] = {(int)M5.Touch.getTouchPointRaw(i).id, d.x, d.y};
  }
  return n;
}

static void script_touch(int x0, int y0, int x1, int y1, int ms) { g_script = {now_ms(), ms, x0, y0, x1, y1}; }

#include "battery.h"
#include "settings_ui.h"
#include "keyboard.h"

// ---- touch on the mirrored page -------------------------------------------------------------------------------
// Touches are forwarded to the PC, except a swipe that starts at an edge and moves inwards: that opens the settings.
// A touch that starts in the edge strip is held back until it is clear which of the two it is (a second finger
// settles it: a pinch or two-finger scroll is for the page).
// With multi-touch, every finger goes to the PC and the browser makes pinch zoom and scrolling of them. Otherwise
// (an older server, or a video) only the first finger does, and the touch ends when it lifts.
// With gestures, only the top edge opens the settings: a swipe in from the left, right or bottom edge goes to the PC
// as a gesture (tabs), and so does three fingers swiped sideways (back / forward), which the page never sees.

static void handle_touch() {
  enum { IDLE, EDGE_PENDING, FORWARDING, LOCAL, IGNORE, THREE };
  static int st = IDLE;
  static int first, sx, sy;
  static int tx, ty;                  // three fingers: where their middle started           // the finger that started the touch, and where
  static Finger sent[5];              // fingers the PC has been told are down, where it last heard of them
  static int nsent = 0;
  static int64_t t0, last_move;
  Finger f[5];
  const int n = read_fingers(f);
  const int64_t now = now_ms();
  const bool page = g_have_client && g_shown_any;
  auto wire = [](int id) { return (uint8_t)(g_multitouch ? id : 0); };
  auto find = [&](int id) -> const Finger* {
    for (int i = 0; i < n; ++i)
      if (f[i].id == id) return &f[i];
    return nullptr;
  };

  if (st == FORWARDING) {   // fingers that lifted
    for (int j = 0; j < nsent;) {
      if (find(sent[j].id)) { ++j; continue; }
      send_event(EV_UP, sent[j].x, sent[j].y, wire(sent[j].id));
      if (!g_multitouch) st = IGNORE;   // one finger: the touch is over, the rest is ignored
      sent[j] = sent[--nsent];
    }
  }
  if (n == 0) {
    if (st == EDGE_PENDING) { send_event(EV_DOWN, sx, sy); send_event(EV_UP, sx, sy); }   // it was a tap
    else if (st == LOCAL) panel_open();
    st = IDLE;
    return;
  }
  g_last_touch = now;
  if (st == IDLE) {
    first = f[0].id; sx = f[0].x; sy = f[0].y; t0 = last_move = now; nsent = 0;
    if (!page) st = LOCAL;   // nothing mirrored: any tap opens the settings
    else if (sx < EDGE || sx >= SCR_W - EDGE || sy < EDGE || sy >= SCR_H - EDGE) st = EDGE_PENDING;
    else st = FORWARDING;
  }
  if (st == EDGE_PENDING) {
    const Finger* p = find(first);
    bool forward = n > 1 || !p || now - t0 >= SWIPE_MS;
    if (!forward) {
      // inward distance from the edge the touch started at
      const int in = sx < EDGE ? p->x - sx : sx >= SCR_W - EDGE ? sx - p->x : sy < EDGE ? p->y - sy : sy - p->y;
      const int along = (sx < EDGE || sx >= SCR_W - EDGE) ? abs(p->y - sy) : abs(p->x - sx);
      if (in >= SWIPE_IN) {
        st = IGNORE;
        const int edge = sx < EDGE ? GEST_LEFT : sx >= SCR_W - EDGE ? GEST_RIGHT : sy < EDGE ? 0 : GEST_BOTTOM;
        if (g_gestures && edge) send_event(EV_GESTURE, edge, 0);
        else panel_open();
        return;
      }
      forward = along > SWIPE_IN / 2;
    }
    if (!forward) return;
    st = FORWARDING;   // not a swipe: hand the whole touch to the page, from where it started
    send_event(EV_DOWN, sx, sy, wire(first));
    sent[nsent++] = {first, sx, sy};
  }
  int mx = 0, my = 0;   // the fingers' middle
  for (int i = 0; i < n; ++i) { mx += f[i].x / n; my += f[i].y / n; }
  if (st == FORWARDING && g_gestures && n >= 3) {   // three fingers: a gesture, not for the page; take back the others
    for (int j = 0; j < nsent; ++j) send_event(EV_UP, sent[j].x, sent[j].y, wire(sent[j].id));
    nsent = 0;
    st = THREE;
    tx = mx; ty = my;
  }
  if (st == THREE) {
    if (n >= 3 && abs(mx - tx) >= SWIPE3 && abs(mx - tx) > 2 * abs(my - ty)) {   // (while all three are down)
      send_event(EV_GESTURE, mx > tx ? GEST_THREE_RIGHT : GEST_THREE_LEFT, 0);
      st = IGNORE;
    }
    return;
  }
  if (st != FORWARDING) return;
  const bool move_due = now - last_move >= 16;
  bool moved = false;
  for (int i = 0; i < n; ++i) {
    if (!g_multitouch && f[i].id != first) continue;
    Finger* s = nullptr;
    for (int j = 0; j < nsent; ++j)
      if (sent[j].id == f[i].id) s = &sent[j];
    if (!s) {
      send_event(EV_DOWN, f[i].x, f[i].y, wire(f[i].id));
      sent[nsent++] = f[i];
    } else if (move_due && (abs(f[i].x - s->x) >= 2 || abs(f[i].y - s->y) >= 2)) {
      send_event(EV_MOVE, f[i].x, f[i].y, wire(f[i].id));
      *s = f[i];
      moved = true;
    }
  }
  if (moved) last_move = now;
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
  const uint32_t r = s.rects + s.strips ? s.rects + s.strips : 1;
  const AudioStats a = g_astats;
  Serial.printf("v%s | updates %u rects %u strips %u bytes %llu | per rect or strip: recv %.1f ms decode %.1f ms ppa %.1f ms | "
                "wifi rssi %d sleep %s | psram free %u\n",
                FIRMWARE_VERSION, (unsigned)s.updates, (unsigned)s.rects, (unsigned)s.strips, (unsigned long long)s.bytes,
                s.recv_us / 1000.0 / r, s.decode_us / 1000.0 / r, s.ppa_us / (1000.0 * (s.rects ? s.rects : 1)), (int)WiFi.RSSI(), g_wifi_sleep ? "on" : "off",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("sound | packets %u late %u lost %u frames hold-ups %u | steering: dropped %u repeated %u | buffer %d ms "
                "(target %d) | speaker %s vol %u\n",
                (unsigned)a.packets, (unsigned)a.late, (unsigned)a.lost_frames, (unsigned)a.holdups, (unsigned)a.dropped,
                (unsigned)a.repeated, (int)(g_level * 1000 / AUDIO_RATE), g_target * 1000 / AUDIO_RATE,
                g_spk_ready ? "on" : "off", g_vol);
  uint32_t ngaps = 0, acc = 0, p50 = 0, p95 = 0, over50 = 0;
  for (int i = 0; i < 32; ++i) { ngaps += g_stats.gaps[i]; if (i >= 5) over50 += g_stats.gaps[i]; }
  for (int i = 0; i < 32 && ngaps; ++i) {
    acc += g_stats.gaps[i];
    if (!p50 && acc * 2 >= ngaps) p50 = i * 10 + 5;
    if (!p95 && acc * 100 >= ngaps * 95) p95 = i * 10 + 5;
  }
  if (ngaps) Serial.printf("pacing | %u gaps between updates shown: p50 ~%u ms, p95 ~%u ms, max %u ms, over 50 ms %u"
                           " | timed %u, late %u, early by %lld ms on average\n", ngaps, p50, p95, g_stats.gap_max, over50,
                           g_pstats.timed, g_pstats.late, g_pstats.timed ? g_pstats.early_ms / g_pstats.timed : 0LL);
  g_pstats = {};
  char batt[128];
  battery_text(batt, sizeof(batt));
  Serial.printf("battery | %s\n", batt);
  Serial.printf("keyboard | %s 0x%02X | %u key events | %u probes, longest %u us\n", g_kbd.present ? "firmware" : "none, last firmware",
                g_kbd.version, g_kbd.keys, g_kbd.probes, g_kbd.probe_us_max);
  const LaneStats ls = g_lstats;
  g_lstats = {};
  Serial.printf("lanes | %d in use | chunks %u | waited for data %u times, %u ms (lanes %u/%u), longest %u ms | longest idle %u ms on lane %u, others holding %u bytes\n", g_lanes_on,
                (unsigned)ls.chunks, (unsigned)ls.waits, (unsigned)ls.wait_ms, (unsigned)ls.lane_wait_ms[0],
                (unsigned)ls.lane_wait_ms[1], (unsigned)ls.max_wait_ms, (unsigned)ls.idle_max_ms,
                (unsigned)ls.idle_lane, (unsigned)ls.idle_others);
  Serial.printf("memory | internal free %u (lowest %u, largest block %u) | psram free %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  g_stats = {};
  g_astats = {};
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
    } else if (line == "wifiinfo") {
      wifi_info();
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
    } else if (line.startsWith("three ")) {   // three x0 x1 y [ms]: three fingers (80 px apart) moved sideways
      int x0 = 400, x1 = 800, y = 360, ms = 400;
      sscanf(line.c_str() + 6, "%d %d %d %d", &x0, &x1, &y, &ms);
      g_last_touch = now_ms();
      set_wifi_sleep(false);
      script_touch(x0, y - 80, x1, y - 80, ms);
      g_script2 = {g_script.t0, ms, x0, y, x1, y};
      g_script3 = {g_script.t0, ms, x0, y + 80, x1, y + 80};
      Serial.printf("three fingers %d -> %d at y %d in %d ms\n", x0, x1, y, ms);
    } else if (line.startsWith("pinch ")) {   // pinch cx cy d0 d1 [ms]: two fingers, d0 apart -> d1 apart (sideways)
      int cx = 640, cy = 360, d0 = 100, d1 = 400, ms = 500;
      sscanf(line.c_str() + 6, "%d %d %d %d %d", &cx, &cy, &d0, &d1, &ms);
      g_last_touch = now_ms();
      set_wifi_sleep(false);
      script_touch(cx - d0 / 2, cy, cx - d1 / 2, cy, ms);
      g_script2 = {g_script.t0, ms, cx + d0 / 2, cy, cx + d1 / 2, cy};
      g_script3 = {};
      Serial.printf("pinch at %d,%d: %d -> %d px in %d ms\n", cx, cy, d0, d1, ms);
    } else if (line == "beep") {   // speaker check: a 1 kHz tone, and the codec and amplifier state
      const bool on = speaker_on();
      g_spk_keep = now_ms() + 2000;
      const bool queued = on && M5.Speaker.tone(1000, 500, 1);
      Serial.printf("beep: speaker %s, running %d, tone %s, vol %u | ES8388 power %02x dac %02x | amp reg %02x\n",
                    on ? "on" : "FAILED", (int)M5.Speaker.isRunning(), queued ? "queued" : "FAILED", g_vol,
                    M5.In_I2C.readRegister8(0x10, 2, 400000), M5.In_I2C.readRegister8(0x10, 4, 400000),
                    M5.In_I2C.readRegister8(0x43, 0x05, 400000));
    } else if (line == "rot") {
      set_rotation(g_rot == 1 ? 3 : 1);
      Serial.printf("rotation %d\n", g_rot);
    } else if (line.startsWith("kbw ") || line.startsWith("kbr ")) {   // keyboard register: kbw <reg> <value> | kbr <reg> (hex)
      unsigned reg = 0, val = 0;
      const int n = sscanf(line.c_str() + 4, "%x %x", &reg, &val);
      bool ok = true;
      if (line[2] == 'w' && n == 2) ok = M5.Ex_I2C.writeRegister8(KBD_ADDR, reg, val, KBD_FREQ);
      uint8_t v = 0;
      ok = ok && M5.Ex_I2C.readRegister(KBD_ADDR, reg, &v, 1, KBD_FREQ);
      Serial.printf("kb reg 0x%02X = 0x%02X%s\n", reg, v, ok ? "" : " (I2C error)");
    } else if (line == "rot auto") {   // forget the saved rotation: from the next start, it follows the keyboard
      prefs.remove("rot");
      Serial.println("rotation: none saved, chosen at the next start");
    } else if (line.startsWith("wifi ")) {
      const int sp = line.indexOf(' ', 5);
      wifi_start(sp > 0 ? line.substring(5, sp) : line.substring(5), sp > 0 ? line.substring(sp + 1) : "");
    } else if (line.length()) {
      Serial.println("commands: ip | wifi <ssid> <password> | wifiinfo | rot | rot auto | S | F | T <x> <y> | drag x0 y0 x1 y1 [ms] | pinch cx cy d0 d1 [ms] | three x0 x1 y [ms] | panel | beep | forget");
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
  cfg.internal_spk = true;   // configured here, switched on only while sound plays (audio.h)
  M5.begin(cfg);
  Serial.printf("\nTab5 Web Display %s\n", FIRMWARE_VERSION);
  prefs.begin("webdisp", false);
  // The rotation the flip switch saved, or with none saved, upright for how the Tab5 is likely held: docked in
  // M5Stack's keyboard, that is flipped (as in M5Stack's own keyboard examples).
  g_kled = (uint8_t)min((int)prefs.getUChar("kled", 10), 100);
  kbd_begin();
  const bool auto_rot = !prefs.isKey("rot");
  g_rot = auto_rot ? (kbd_present_at_boot() ? 3 : 1) : prefs.getInt("rot", 1) == 3 ? 3 : 1;
  if (auto_rot) Serial.printf("rotation %d (none saved; %s)\n", g_rot, g_rot == 3 ? "keyboard fitted" : "no keyboard");
  g_bri = (uint8_t)constrain((int)prefs.getUChar("bri", 160), 10, 255);
  M5.Display.setRotation(g_rot);
  M5.Display.setBrightness(g_bri);
  audio_setup();
  if (!hw_setup()) {
    show_status("Hardware setup failed", "JPEG decoder / PPA / memory");
    for (;;) delay(1000);
  }
  ui_setup();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  lanes_begin();
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
  speaker_poll();
  battery_poll();
  kbd_poll();
  set_wifi_sleep(g_power_mode == 0 && !g_panel_open && now_ms() - g_last_touch > AWAKE_AFTER_TOUCH_MS &&
                 now_ms() - g_last_stream > AWAKE_AFTER_STREAM_MS && !audio_recent(AWAKE_AFTER_SOUND_MS));
  // while updates stream in, come back quickly for the next one; otherwise save power
  delay(g_have_client && now_ms() - g_last_update < 500 ? 1 : 5);
}
