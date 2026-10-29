// The Tab5 Keyboard (M5Stack A164): an STM32 on Ext.Port1 that scans a 5 x 14 key matrix and queues press/release
// events, read over I2C (address 0x6D, SDA G0, SCL G1; INT G50 goes low while events are queued).
// It is used in its "normal" mode, which reports each key going down and up. The Sym and Aa layers are applied here
// (Aa: capitals, and Shift on keys that don't type, e.g. Shift+arrows to select; Sym: the second symbol printed on
// a key, and Home/End/Page Up/Page Down/Delete on the arrows and Backspace),
// and keys go to the PC as USB HID usage codes with a modifier byte, which the server turns into browser key events.
// In the settings panel, keys type into the WiFi dialog instead.
// Its two lights are ours (see kbd_leds): the left one shows Sym / Aa held, the right one the connection.
// Plugging the keyboard in or out while running is noticed within KBD_PROBE_MS.
#pragma once

static constexpr uint8_t KBD_ADDR = 0x6D;
static constexpr int KBD_SDA = 0, KBD_SCL = 1, KBD_INT = 50;
static constexpr uint32_t KBD_FREQ = 400000;
static constexpr int64_t KBD_PROBE_MS = 2000;   // look for a keyboard this often while none answers
static constexpr int64_t KBD_POLL_MS = 100;     // read the queue at least this often, should an INT edge be missed
enum : uint8_t { KBD_REG_EVENT_NUM = 0x02, KBD_REG_BRIGHTNESS = 0x03, KBD_REG_MODE = 0x10, KBD_REG_RGB_MODE = 0x11,
                 KBD_REG_RGB1 = 0x60, KBD_REG_RGB2 = 0x64, KBD_REG_KEY_EVENT = 0x20, KBD_REG_VERSION = 0xFE };
enum : uint8_t { KMOD_CTRL = 0x01, KMOD_SHIFT = 0x02, KMOD_ALT = 0x04 };   // HID modifier bits

// HID usage code and forced modifier of each key, row by row (from M5Stack's M5Unit-KEYBOARD library,
// https://github.com/m5stack/M5Unit-KEYBOARD: MIT License, Copyright (c) 2025 M5Stack Technology CO LTD, see
// licenses/MIT-M5Unit-KEYBOARD.txt), and of each key with Sym held. 0 = a modifier key: Sym (3,0), Aa (3,1), Ctrl (4,0), Alt (4,1).
struct KbdKey { uint8_t code, mod; };
static const KbdKey KBD_BASE[70] = {
  // Esc 1 2 3 4 5 6 7 8 9 0 - + Del
  {0x29, 0}, {0x1E, 0}, {0x1F, 0}, {0x20, 0}, {0x21, 0}, {0x22, 0}, {0x23, 0}, {0x24, 0}, {0x25, 0}, {0x26, 0},
  {0x27, 0}, {0x2D, 0}, {0x2E, 2}, {0x4C, 0},
  // ` ! @ # $ % ^ & * ( ) [ ] backslash
  {0x35, 0}, {0x1E, 2}, {0x1F, 2}, {0x20, 2}, {0x21, 2}, {0x22, 2}, {0x23, 2}, {0x24, 2}, {0x25, 2}, {0x26, 2},
  {0x27, 2}, {0x2F, 0}, {0x30, 0}, {0x31, 0},
  // Tab q w e r t y u i o p ; ' Backspace
  {0x2B, 0}, {0x14, 0}, {0x1A, 0}, {0x08, 0}, {0x15, 0}, {0x17, 0}, {0x1C, 0}, {0x18, 0}, {0x0C, 0}, {0x12, 0},
  {0x13, 0}, {0x33, 0}, {0x34, 0}, {0x2A, 0},
  // Sym Aa a s d f g h j k l Up _ Enter
  {0, 0}, {0, 0}, {0x04, 0}, {0x16, 0}, {0x07, 0}, {0x09, 0}, {0x0A, 0}, {0x0B, 0}, {0x0D, 0}, {0x0E, 0},
  {0x0F, 0}, {0x52, 0}, {0x2D, 2}, {0x28, 0},
  // Ctrl Alt z x c v b n m . Left Down Right Space
  {0, 0}, {0, 0}, {0x1D, 0}, {0x1B, 0}, {0x06, 0}, {0x19, 0}, {0x05, 0}, {0x11, 0}, {0x10, 0}, {0x37, 0},
  {0x50, 0}, {0x51, 0}, {0x4F, 0}, {0x2C, 0},
};
// The Sym layer differs only here: ` ~  ! ?  * /  ( <  ) >  [ {  ] }  \ |  ; :  ' "  _ =  . ,
// and Left Home, Right End, Up Page Up, Down Page Down, Backspace Delete (keys the keyboard lacks)
struct KbdSym { uint8_t idx; KbdKey key; };
static const KbdSym KBD_SYM[] = {
  {14, {0x35, 2}}, {15, {0x38, 2}}, {22, {0x38, 0}}, {23, {0x36, 2}}, {24, {0x37, 2}}, {25, {0x2F, 2}},
  {26, {0x30, 2}}, {27, {0x31, 2}}, {39, {0x33, 2}}, {40, {0x34, 2}}, {54, {0x2E, 0}}, {65, {0x36, 0}},
  {66, {0x4A, 0}}, {68, {0x4D, 0}}, {53, {0x4B, 0}}, {67, {0x4E, 0}}, {41, {0x4C, 0}},
};
static constexpr uint8_t KIDX_SYM = 42, KIDX_AA = 43, KIDX_CTRL = 56, KIDX_ALT = 57;

struct Kbd {
  bool present = false;
  uint8_t version = 0;
  int fails = 0;                  // I2C errors in a row
  int64_t next_probe = 0, next_poll = 0;
  bool sym = false, aa = false, ctrl = false, alt = false;
  KbdKey down[70] = {};           // what each held key was sent as, so its release matches even if Sym changed
  uint32_t keys = 0;              // key events read, for the stats
  uint32_t probes = 0, probe_us_max = 0;   // looking for a keyboard: how often, and the longest it took
  uint32_t led[2] = {~0u, ~0u};   // colour last set on each light (0xRRGGBB); ~0: unknown, set it again
};
static Kbd g_kbd;

// The character a key types in the WiFi dialog (US layout), or 0.
static char kbd_char(KbdKey k) {
  static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
  static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
  if (k.code < 0x04 || k.code > 0x38) return 0;
  return (k.mod & KMOD_SHIFT ? upper : lower)[k.code - 0x04];
}

static void kbd_type_in_panel(KbdKey k) {
  if (lv_obj_has_flag(ui_modal, LV_OBJ_FLAG_HIDDEN)) {   // the panel itself: Esc closes it
    if (k.code == 0x29) panel_close();
    return;
  }
  lv_obj_t* ta = lv_keyboard_get_textarea(ui_kb);
  const char c = kbd_char(k);
  if (c == '\n') ui_connect();
  else if (c == '\x1b') ui_hide_modal();
  else if (c == '\b') { if (ta) lv_textarea_delete_char(ta); }
  else if (k.code == 0x4C) { if (ta) lv_textarea_delete_char_forward(ta); }
  else if (k.code == 0x50) { if (ta) lv_textarea_cursor_left(ta); }
  else if (k.code == 0x4F) { if (ta) lv_textarea_cursor_right(ta); }
  else if (k.code == 0x4A) { if (ta) lv_textarea_set_cursor_pos(ta, 0); }
  else if (k.code == 0x4D) { if (ta) lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); }
  else if (c == '\t') {   // to the other field, if it is shown
    lv_obj_t* other = ta == ui_ta_ssid ? ui_ta_pass : ui_ta_ssid;
    if (!lv_obj_has_flag(other, LV_OBJ_FLAG_HIDDEN)) {
      lv_keyboard_set_textarea(ui_kb, other);
      lv_obj_add_state(other, LV_STATE_FOCUSED);
      if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    }
  } else if (c && ta && !(k.mod & (KMOD_CTRL | KMOD_ALT))) {
    lv_textarea_add_char(ta, c);
  }
}

static void kbd_event(uint8_t raw) {
  const bool pressed = raw & 0x80;
  const int row = (raw >> 4) & 7, col = raw & 15;
  if (row > 4 || col > 13) return;
  const int i = row * 14 + col;
  ++g_kbd.keys;
  g_last_touch = now_ms();   // a key counts as activity: WiFi stays awake, and lost events are nudged along
  switch (i) {
    case KIDX_SYM: g_kbd.sym = pressed; return;
    case KIDX_AA: g_kbd.aa = pressed; return;
    case KIDX_CTRL: g_kbd.ctrl = pressed; return;
    case KIDX_ALT: g_kbd.alt = pressed; return;
  }
  KbdKey k;
  if (pressed) {
    k = KBD_BASE[i];
    if (g_kbd.sym)
      for (const KbdSym& s : KBD_SYM)
        if (s.idx == i) k = s.key;
    // Aa is Shift only where that is unsurprising: a letter, or a key that doesn't type. On a symbol or digit key, US
    // Shift would type a character that isn't printed on it (. would give >); Sym gives the printed one.
    const bool symbol = (k.code >= 0x1E && k.code <= 0x27) || (k.code >= 0x2D && k.code <= 0x38);
    if (g_kbd.aa && !symbol) k.mod |= KMOD_SHIFT;
    if (g_kbd.ctrl) k.mod |= KMOD_CTRL;
    if (g_kbd.alt) k.mod |= KMOD_ALT;
    g_kbd.down[i] = k;
  } else {
    k = g_kbd.down[i];
    g_kbd.down[i] = {0, 0};
    if (!k.code) return;   // pressed before we started reading
  }
  if (g_panel_open) {
    if (pressed) kbd_type_in_panel(k);
  } else {
    send_event(EV_KEY, k.code, k.mod, pressed ? 1 : 0);
  }
}

static bool kbd_probe() {
  if (!M5.Ex_I2C.scanID(KBD_ADDR, KBD_FREQ)) return false;
  g_kbd.version = M5.Ex_I2C.readRegister8(KBD_ADDR, KBD_REG_VERSION, KBD_FREQ);
  // normal mode, and an empty queue (writing 0 to the count clears it)
  // and the lights ours, at the brightness from the settings (it scales every colour; 0 = off)
  g_kbd.led[0] = g_kbd.led[1] = ~0u;
  return M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_MODE, 0, KBD_FREQ) &&
         M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_EVENT_NUM, 0, KBD_FREQ) &&
         M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_RGB_MODE, 1, KBD_FREQ) &&
         M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_BRIGHTNESS, g_kled, KBD_FREQ);
}

// The lights. Left: off, white while Aa is held, amber while Sym is (alternating with both). Right: green while a
// PC is connected, dim blue with WiFi up and no PC, amber without WiFi, blinking red below 10 % battery.
// Colours are written only when they change.
static constexpr uint32_t LED_OFF = 0, LED_WHITE = 0xFFFFFF, LED_AMBER = 0xFF8000, LED_GREEN = 0x00FF00,
                          LED_IDLE = 0x000040, LED_RED = 0xFF0000;

static void kbd_set_light(uint8_t level) {   // from the settings slider
  g_kled = level;
  if (g_kbd.present) M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_BRIGHTNESS, level, KBD_FREQ);
}

static bool kbd_led(int i, uint32_t rgb) {
  if (g_kbd.led[i] == rgb) return true;
  const uint8_t bgr[3] = {(uint8_t)rgb, (uint8_t)(rgb >> 8), (uint8_t)(rgb >> 16)};   // registers: B, G, R
  const uint8_t reg = i ? KBD_REG_RGB2 : KBD_REG_RGB1;
  for (int c = 0; c < 3; ++c)
    if (!M5.Ex_I2C.writeRegister8(KBD_ADDR, reg + c, bgr[c], KBD_FREQ)) return false;
  g_kbd.led[i] = rgb;
  return true;
}

static bool kbd_leds(int64_t now) {
  const Kbd& k = g_kbd;
  const bool blink = (now / 400) % 2;
  const uint32_t left = k.aa && k.sym ? (blink ? LED_WHITE : LED_AMBER) : k.aa ? LED_WHITE : k.sym ? LED_AMBER : LED_OFF;
  uint32_t right = g_have_client ? LED_GREEN : g_wifi == WIFI_UP ? LED_IDLE : LED_AMBER;
  const Battery& b = g_batt;
  if (b.presence.state == BatteryPresence::Present && b.level < 10 && !b.charging) right = (now / 500) % 2 ? LED_RED : LED_OFF;
  return kbd_led(0, left) && kbd_led(1, right);
}

static void kbd_release_all() {   // keys held when the keyboard went away, or the connection changed
  for (int i = 0; i < 70; ++i)
    if (g_kbd.down[i].code) kbd_event(0x00 | (i / 14) << 4 | (i % 14));
  g_kbd.sym = g_kbd.aa = g_kbd.ctrl = g_kbd.alt = false;
}

static void kbd_begin() {
  pinMode(KBD_INT, INPUT_PULLUP);
  // M5Unified's external bus (port 0) is otherwise for Port A, which this firmware doesn't use
  M5.Ex_I2C.begin(I2C_NUM_0, KBD_SDA, KBD_SCL);
}

static void kbd_poll() {
  Kbd& k = g_kbd;
  const int64_t now = now_ms();
  if (!k.present) {
    if (now < k.next_probe) return;
    k.next_probe = now + KBD_PROBE_MS;
    const int64_t t0 = esp_timer_get_time();
    const bool found = kbd_probe();
    k.probe_us_max = max(k.probe_us_max, (uint32_t)(esp_timer_get_time() - t0));
    ++k.probes;
    if (!found) return;
    k.present = true;
    k.fails = 0;
    Serial.printf("keyboard found (firmware 0x%02X)\n", k.version);
  }
  bool ok = kbd_leds(now);
  if (ok && digitalRead(KBD_INT) == HIGH && now < k.next_poll) return;
  k.next_poll = now + KBD_POLL_MS;
  uint8_t n = 0;
  ok = ok && M5.Ex_I2C.readRegister(KBD_ADDR, KBD_REG_EVENT_NUM, &n, 1, KBD_FREQ);
  for (int j = 0; ok && j < n && j < 32; ++j) {
    uint8_t raw = 0xFF;
    ok = M5.Ex_I2C.readRegister(KBD_ADDR, KBD_REG_KEY_EVENT, &raw, 1, KBD_FREQ);
    if (ok && raw != 0xFF) kbd_event(raw);
  }
  if (ok) { k.fails = 0; return; }
  if (++k.fails < 3) return;
  k.present = false;
  kbd_release_all();
  Serial.println("keyboard gone");
}

// At startup: is a keyboard fitted? Its controller powers up with the Tab5, so it gets a moment to answer.
static bool kbd_present_at_boot() {
  for (int i = 0; i < 5 && !g_kbd.present; ++i) {
    if (i) delay(100);
    g_kbd.next_probe = 0;
    kbd_poll();
  }
  return g_kbd.present;
}
