// Settings panel (LVGL): WiFi network list and password entry, connection status, brightness, volume, flip.
// Included by web_display.ino after the WiFi and display helpers. LVGL only runs while the panel is open; its
// strips are rotated into the frame buffer by the PPA like the page rectangles. While the panel is open the PC
// connection is refused; the PC reconnects within 2 s of closing and sends a full frame.
#pragma once

static constexpr uint32_t C_BG = 0x11141c, C_CARD = 0x1a1f2b, C_LINE = 0x2a3142, C_INK = 0xe8e4da, C_DIM = 0x8b93a7,
                          C_ACCENT = 0x5ae2c7, C_WARN = 0xf0a868;
static constexpr int MAX_NETS = 24;

struct Net { char ssid[33]; int8_t rssi; bool secure; };
static Net g_nets[MAX_NETS];
static int g_net_count = 0;

static lv_display_t* g_disp;
static lv_obj_t *ui_wifi, *ui_addr, *ui_pc, *ui_hint, *ui_list, *ui_scan_btn, *ui_scan_lbl;
static lv_obj_t *ui_modal, *ui_modal_title, *ui_ta_ssid, *ui_ta_pass, *ui_show_btn, *ui_msg, *ui_spinner, *ui_kb;
static lv_obj_t *ui_connect_btn, *ui_bri, *ui_vol, *ui_flip;
static bool g_scanning = false, g_connect_pending = false, g_touch_block = false;
static String g_pending_ssid;   // the network the dialog is joining

static void ui_start_scan();

// ---- LVGL plumbing ------------------------------------------------------------------------------------------

static void ui_flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  ppa_blit(px, a->x1, a->y1, lv_area_get_width(a), lv_area_get_height(a));
  lv_display_flush_ready(d);
}

static void ui_touch(lv_indev_t*, lv_indev_data_t* d) {
  static int16_t lx = 0, ly = 0;
  const TouchPoint t = read_touch();
  bool pressed = t.pressed;
  if (pressed) { lx = t.x; ly = t.y; }
  if (g_touch_block) {          // the finger that opened the panel: ignore it until it lifts
    if (!pressed) g_touch_block = false;
    pressed = false;
  }
  d->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  d->point.x = lx;
  d->point.y = ly;
}

static void* ui_alloc(size_t bytes) { return heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// ---- small builders -----------------------------------------------------------------------------------------

static lv_obj_t* ui_card(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, 20, 0);
  lv_obj_set_style_pad_all(o, 22, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* ui_label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static lv_obj_t* ui_button(lv_obj_t* parent, const char* text, lv_event_cb_t cb, bool primary) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_LINE), 0);
  lv_obj_set_style_radius(b, 16, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = ui_label(b, text, &lv_font_montserrat_20, primary ? C_BG : C_INK);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

// ---- status -------------------------------------------------------------------------------------------------

static void ui_refresh_status() {
  const String ip = WiFi.localIP().toString();
  switch (g_wifi) {
    case WIFI_NONE:       lv_label_set_text(ui_wifi, "Not set up - choose a network"); break;
    case WIFI_CONNECTING: lv_label_set_text_fmt(ui_wifi, "Connecting to %s ...", g_try_ssid.c_str()); break;
    case WIFI_FAILED:     lv_label_set_text_fmt(ui_wifi, "Could not connect to %s", g_try_ssid.c_str()); break;
    case WIFI_UP:         lv_label_set_text_fmt(ui_wifi, LV_SYMBOL_WIFI "  %s  (%d dBm)", g_ssid.c_str(), (int)WiFi.RSSI()); break;
  }
  lv_obj_set_style_text_color(ui_wifi, lv_color_hex(g_wifi == WIFI_FAILED ? C_WARN : C_INK), 0);
  if (g_wifi == WIFI_UP) {
    lv_label_set_text_fmt(ui_addr, "%s : %u", ip.c_str(), PORT);
    lv_label_set_text_fmt(ui_hint, "On the PC run:\nserver.py --device %s", ip.c_str());
  } else {
    lv_label_set_text(ui_addr, "-");
    lv_label_set_text(ui_hint, "Connect to WiFi, then run server/server.py on the PC.");
  }
  lv_label_set_text(ui_pc, g_wifi != WIFI_UP ? "-" : "Paused while settings are open");
}

// ---- network list -------------------------------------------------------------------------------------------

static const char* signal_word(int rssi) {
  return rssi >= -55 ? "excellent" : rssi >= -67 ? "good" : rssi >= -78 ? "fair" : "weak";
}

static void ui_show_modal(const char* ssid, bool secure);

static void net_clicked(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0) ui_show_modal(nullptr, true);   // "Other network..."
  else ui_show_modal(g_nets[i].ssid, g_nets[i].secure);
}

static void ui_fill_list() {
  lv_obj_clean(ui_list);
  for (int i = 0; i < g_net_count; i++) {
    const Net& n = g_nets[i];
    const bool current = g_wifi == WIFI_UP && g_ssid == n.ssid;
    lv_obj_t* b = lv_list_add_button(ui_list, current ? LV_SYMBOL_OK : LV_SYMBOL_WIFI, n.ssid);
    lv_obj_set_height(b, 64);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(current ? C_ACCENT : C_INK), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_t* info = ui_label(b, "", &lv_font_montserrat_20, C_DIM);
    lv_label_set_text_fmt(info, "%s, %s", signal_word(n.rssi), n.secure ? "secured" : "open");
    lv_obj_add_event_cb(b, net_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  lv_obj_t* other = lv_list_add_button(ui_list, LV_SYMBOL_PLUS, "Other network ...");
  lv_obj_set_height(other, 64);
  lv_obj_set_style_bg_color(other, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_text_color(other, lv_color_hex(C_DIM), 0);
  lv_obj_add_event_cb(other, net_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
}

static void ui_start_scan() {
  if (g_scanning || g_wifi == WIFI_CONNECTING) return;   // a scan during a connection attempt fails
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) { lv_label_set_text(ui_scan_lbl, "Scan failed - tap Scan to retry"); return; }
  g_scanning = true;
  lv_label_set_text(ui_scan_lbl, "Scanning ...");
}

static void ui_scan_poll() {
  if (!g_scanning) return;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  g_scanning = false;
  if (n < 0) { lv_label_set_text(ui_scan_lbl, "Scan failed - tap Scan to retry"); return; }
  g_net_count = 0;
  for (int i = 0; i < n; i++) {             // de-duplicate by SSID (keep the strongest), skip hidden networks
    const String ss = WiFi.SSID(i);
    if (ss.isEmpty()) continue;
    int j = 0;
    while (j < g_net_count && strcmp(g_nets[j].ssid, ss.c_str()) != 0) j++;
    const int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (j == g_net_count) {
      if (g_net_count == MAX_NETS) continue;
      strlcpy(g_nets[j].ssid, ss.c_str(), sizeof(g_nets[j].ssid));
      g_net_count++;
    } else if (rssi <= g_nets[j].rssi) {
      continue;
    }
    g_nets[j].rssi = rssi;
    g_nets[j].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  for (int a = 1; a < g_net_count; a++)     // strongest first
    for (int b = a; b > 0 && g_nets[b].rssi > g_nets[b - 1].rssi; b--) { Net x = g_nets[b]; g_nets[b] = g_nets[b - 1]; g_nets[b - 1] = x; }
  lv_label_set_text_fmt(ui_scan_lbl, "%d network%s found", g_net_count, g_net_count == 1 ? "" : "s");
  ui_fill_list();
}

// ---- password dialog ----------------------------------------------------------------------------------------

static void ui_hide_modal() {
  g_connect_pending = false;
  lv_obj_add_flag(ui_modal, LV_OBJ_FLAG_HIDDEN);
}

static void ui_connect() {
  const char* ssid = lv_textarea_get_text(ui_ta_ssid);
  if (!ssid[0]) { lv_label_set_text(ui_msg, "Enter the network name"); return; }
  g_pending_ssid = ssid;
  wifi_start(ssid, lv_textarea_get_text(ui_ta_pass));
  g_connect_pending = true;
  lv_label_set_text_fmt(ui_msg, "Connecting to %s ...", ssid);
  lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_DIM), 0);
  lv_obj_remove_flag(ui_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_state(ui_connect_btn, LV_STATE_DISABLED);
}

static void ui_show_modal(const char* ssid, bool secure) {
  const bool other = ssid == nullptr;
  lv_textarea_set_text(ui_ta_ssid, other ? "" : ssid);
  lv_textarea_set_text(ui_ta_pass, "");
  lv_textarea_set_password_mode(ui_ta_pass, true);
  lv_label_set_text(ui_show_btn, "Show");
  lv_label_set_text(ui_modal_title, other ? "Join another network" : ssid);
  lv_label_set_text(ui_msg, secure ? (other ? "Enter the network name and password" : "Enter the password") : "Open network");
  lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_DIM), 0);
  lv_obj_add_flag(ui_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_state(ui_connect_btn, LV_STATE_DISABLED);
  if (other) lv_obj_remove_flag(ui_ta_ssid, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(ui_ta_ssid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(ui_modal, LV_OBJ_FLAG_HIDDEN);
  if (!secure && !other) { ui_connect(); return; }   // nothing to type
  lv_obj_t* target = other ? ui_ta_ssid : ui_ta_pass;
  lv_keyboard_set_textarea(ui_kb, target);
  lv_obj_add_state(target, LV_STATE_FOCUSED);
  lv_obj_remove_state(other ? ui_ta_pass : ui_ta_ssid, LV_STATE_FOCUSED);
}

static void ui_connect_poll() {
  if (!g_connect_pending) return;
  if (g_attempt_failed) {
    g_connect_pending = false;
    lv_obj_add_flag(ui_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(ui_connect_btn, LV_STATE_DISABLED);
    lv_label_set_text(ui_msg, "Could not connect. Check the password and try again.");
    lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_WARN), 0);
  } else if (g_wifi == WIFI_UP && g_ssid == g_pending_ssid) {
    ui_hide_modal();
    ui_fill_list();
  }
}

static lv_obj_t* ui_textarea(lv_obj_t* parent, const char* placeholder) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, lv_pct(100));
  lv_obj_set_style_text_font(ta, &lv_font_montserrat_28, 0);
  lv_obj_set_style_bg_color(ta, lv_color_hex(C_BG), 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(C_INK), 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_STATE_FOCUSED);
  lv_obj_add_event_cb(ta, [](lv_event_t* e) {   // the keyboard types into whichever field was tapped
    lv_obj_t* t = (lv_obj_t*)lv_event_get_target(e);
    lv_keyboard_set_textarea(ui_kb, t);
    lv_obj_remove_state(t == ui_ta_ssid ? ui_ta_pass : ui_ta_ssid, LV_STATE_FOCUSED);
  }, LV_EVENT_FOCUSED, nullptr);
  return ta;
}

// LVGL's stock layout has ". , :" after M; use the usual ", ." order (the rest is LVGL's default).
#define KB_KEY(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | (w)))     // letter key, width w
#define KB_MODE(w) ((lv_buttonmatrix_ctrl_t)(LV_KEYBOARD_CTRL_BUTTON_FLAGS | (w)))   // mode switch / close / OK
#define KB_FN(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CHECKED | (w)))      // backspace, enter, arrows
static const char* const kb_lower[] = {
  "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
  "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
  "_", "-", "z", "x", "c", "v", "b", "n", "m", ",", ".", ":", "\n",
  LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const char* const kb_upper[] = {
  "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
  "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
  "_", "-", "Z", "X", "C", "V", "B", "N", "M", ",", ".", ":", "\n",
  LV_SYMBOL_CLOSE, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t kb_ctrl[] = {
  KB_MODE(5), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
  KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_FN(7),
  KB_MODE(6), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3),
  KB_KEY(3), KB_KEY(3), KB_FN(7),
  KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1),
  KB_KEY(1),
  KB_MODE(2), KB_FN(2), (lv_buttonmatrix_ctrl_t)6, KB_FN(2),
  KB_MODE(2)};

static void ui_build_modal(lv_obj_t* scr) {
  ui_modal = lv_obj_create(scr);
  lv_obj_set_size(ui_modal, SCR_W, SCR_H);
  lv_obj_set_pos(ui_modal, 0, 0);
  lv_obj_set_style_bg_color(ui_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(ui_modal, LV_OPA_70, 0);
  lv_obj_set_style_border_width(ui_modal, 0, 0);
  lv_obj_set_style_radius(ui_modal, 0, 0);
  lv_obj_remove_flag(ui_modal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* box = ui_card(ui_modal, 240, 24, 800, 340);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box, 14, 0);
  ui_modal_title = ui_label(box, "", &lv_font_montserrat_28, C_INK);
  ui_ta_ssid = ui_textarea(box, "Network name");
  lv_obj_t* row = lv_obj_create(box);       // password field + Show
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 12, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  ui_ta_pass = ui_textarea(row, "Password");
  lv_obj_set_flex_grow(ui_ta_pass, 1);
  lv_textarea_set_password_mode(ui_ta_pass, true);
  lv_obj_t* show = ui_button(row, "Show", [](lv_event_t*) {
    const bool hidden = lv_textarea_get_password_mode(ui_ta_pass);
    lv_textarea_set_password_mode(ui_ta_pass, !hidden);
    lv_label_set_text(ui_show_btn, hidden ? "Hide" : "Show");
  }, false);
  lv_obj_set_size(show, 110, 58);
  ui_show_btn = lv_obj_get_child(show, 0);

  lv_obj_t* msg_row = lv_obj_create(box);
  lv_obj_set_size(msg_row, lv_pct(100), 40);
  lv_obj_set_style_bg_opa(msg_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(msg_row, 0, 0);
  lv_obj_set_style_pad_all(msg_row, 0, 0);
  lv_obj_remove_flag(msg_row, LV_OBJ_FLAG_SCROLLABLE);
  ui_spinner = lv_spinner_create(msg_row);
  lv_obj_set_size(ui_spinner, 32, 32);
  lv_obj_align(ui_spinner, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_arc_color(ui_spinner, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ui_spinner, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ui_spinner, 5, 0);
  ui_msg = ui_label(msg_row, "", &lv_font_montserrat_20, C_DIM);
  lv_obj_align(ui_msg, LV_ALIGN_LEFT_MID, 44, 0);

  lv_obj_t* buttons = lv_obj_create(box);
  lv_obj_set_size(buttons, lv_pct(100), 64);
  lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(buttons, 0, 0);
  lv_obj_set_style_pad_all(buttons, 0, 0);
  lv_obj_remove_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* cancel = ui_button(buttons, "Cancel", [](lv_event_t*) { ui_hide_modal(); }, false);
  lv_obj_set_size(cancel, 200, 64);
  lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);
  ui_connect_btn = ui_button(buttons, "Connect", [](lv_event_t*) { ui_connect(); }, true);
  lv_obj_set_size(ui_connect_btn, 240, 64);
  lv_obj_align(ui_connect_btn, LV_ALIGN_RIGHT_MID, 0, 0);

  ui_kb = lv_keyboard_create(ui_modal);
  lv_obj_set_size(ui_kb, SCR_W, 300);
  lv_obj_align(ui_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(ui_kb, &lv_font_montserrat_28, 0);
  lv_keyboard_set_map(ui_kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lower, kb_ctrl);
  lv_keyboard_set_map(ui_kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_upper, kb_ctrl);
  lv_obj_add_event_cb(ui_kb, [](lv_event_t*) { ui_connect(); }, LV_EVENT_READY, nullptr);       // the keyboard's OK key
  lv_obj_add_event_cb(ui_kb, [](lv_event_t*) { ui_hide_modal(); }, LV_EVENT_CANCEL, nullptr);   // its close key
  lv_obj_add_flag(ui_modal, LV_OBJ_FLAG_HIDDEN);
}

// ---- panel --------------------------------------------------------------------------------------------------

static void set_rotation(int r) {
  g_rot = r;
  M5.Display.setRotation(g_rot);   // touch coordinates follow it
  prefs.putInt("rot", g_rot);
  if (g_panel_open) lv_obj_invalidate(lv_screen_active());
  else { drop_client(); status_screen(); }   // the PC redraws the page after reconnecting
}

static void panel_close() {
  if (!g_panel_open) return;
  ui_hide_modal();
  g_panel_open = false;
  if (WiFi.scanComplete() >= 0) WiFi.scanDelete();
  g_scanning = false;
  status_screen();
}

static void panel_open() {
  if (g_panel_open) return;
  g_panel_open = true;
  drop_client();
  g_touch_block = read_touch().pressed;
  lv_slider_set_value(ui_bri, g_bri, LV_ANIM_OFF);
  lv_slider_set_value(ui_vol, g_vol, LV_ANIM_OFF);
  if (g_rot == 3) lv_obj_add_state(ui_flip, LV_STATE_CHECKED);
  else lv_obj_remove_state(ui_flip, LV_STATE_CHECKED);
  ui_refresh_status();
  ui_fill_list();
  lv_obj_invalidate(lv_screen_active());
  ui_start_scan();
  Serial.println("settings open");
}

static void ui_poll() {
  static int64_t next = 0;
  ui_scan_poll();
  ui_connect_poll();
  if (now_ms() >= next) { ui_refresh_status(); next = now_ms() + 500; }
}

static void ui_setup() {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return (uint32_t)(esp_timer_get_time() / 1000); });
  g_disp = lv_display_create(SCR_W, SCR_H);
  lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(g_disp, ui_flush);
  const size_t buf = SCR_W * 80 * 2;
  lv_display_set_buffers(g_disp, ui_alloc(buf), ui_alloc(buf), buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_theme(g_disp, lv_theme_default_init(g_disp, lv_color_hex(C_ACCENT), lv_color_hex(C_DIM), true,
                                                      &lv_font_montserrat_20));
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, ui_touch);

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* title = ui_label(scr, "Settings", &lv_font_montserrat_28, C_INK);
  lv_obj_set_pos(title, 34, 28);
  lv_obj_t* sub = ui_label(scr, "", &lv_font_montserrat_20, C_DIM);
  lv_label_set_text_fmt(sub, "Tab5 Web Display %s", FIRMWARE_VERSION);
  lv_obj_set_pos(sub, 180, 34);

  // status
  lv_obj_t* st = ui_card(scr, 32, 80, 410, 300);
  lv_obj_set_flex_flow(st, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(st, 6, 0);
  ui_label(st, "WI-FI", &lv_font_montserrat_20, C_DIM);
  ui_wifi = ui_label(st, "", &lv_font_montserrat_20, C_INK);
  lv_label_set_long_mode(ui_wifi, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ui_wifi, lv_pct(100));
  lv_obj_t* h2 = ui_label(st, "ADDRESS", &lv_font_montserrat_20, C_DIM);
  lv_obj_set_style_margin_top(h2, 10, 0);
  ui_addr = ui_label(st, "", &lv_font_montserrat_28, C_ACCENT);
  lv_obj_t* h3 = ui_label(st, "PC", &lv_font_montserrat_20, C_DIM);
  lv_obj_set_style_margin_top(h3, 10, 0);
  ui_pc = ui_label(st, "", &lv_font_montserrat_20, C_INK);
  ui_hint = ui_label(st, "", &lv_font_montserrat_20, C_DIM);
  lv_obj_set_style_margin_top(ui_hint, 10, 0);
  lv_label_set_long_mode(ui_hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ui_hint, lv_pct(100));

  // display
  lv_obj_t* dc = ui_card(scr, 32, 396, 410, 190);
  lv_obj_t* bl = ui_label(dc, "Brightness", &lv_font_montserrat_20, C_INK);
  lv_obj_set_pos(bl, 0, 6);
  ui_bri = lv_slider_create(dc);
  lv_slider_set_range(ui_bri, 10, 255);
  lv_obj_set_size(ui_bri, 210, 16);
  lv_obj_set_pos(ui_bri, 150, 10);
  lv_obj_set_ext_click_area(ui_bri, 20);
  lv_obj_add_event_cb(ui_bri, [](lv_event_t*) {
    g_bri = (uint8_t)lv_slider_get_value(ui_bri);
    M5.Display.setBrightness(g_bri);
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(ui_bri, [](lv_event_t*) { prefs.putUChar("bri", g_bri); }, LV_EVENT_RELEASED, nullptr);
  lv_obj_t* vl = ui_label(dc, "Volume", &lv_font_montserrat_20, C_INK);
  lv_obj_set_pos(vl, 0, 56);
  ui_vol = lv_slider_create(dc);
  lv_slider_set_range(ui_vol, 0, 255);
  lv_obj_set_size(ui_vol, 210, 16);
  lv_obj_set_pos(ui_vol, 150, 60);
  lv_obj_set_ext_click_area(ui_vol, 20);
  lv_obj_add_event_cb(ui_vol, [](lv_event_t*) { set_volume((uint8_t)lv_slider_get_value(ui_vol), false); },
                      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(ui_vol, [](lv_event_t*) {   // save, and let the new level be heard
    prefs.putUChar("vol", g_vol);
    set_volume(g_vol, true);
  }, LV_EVENT_RELEASED, nullptr);
  lv_obj_t* fl = ui_label(dc, "Flip screen 180°", &lv_font_montserrat_20, C_INK);
  lv_obj_set_pos(fl, 0, 104);
  ui_flip = lv_switch_create(dc);
  lv_obj_set_size(ui_flip, 84, 44);
  lv_obj_align(ui_flip, LV_ALIGN_TOP_RIGHT, 0, 94);
  lv_obj_add_event_cb(ui_flip, [](lv_event_t*) {
    set_rotation(lv_obj_has_state(ui_flip, LV_STATE_CHECKED) ? 3 : 1);
  }, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* done = ui_button(scr, "Done", [](lv_event_t*) { panel_close(); }, true);
  lv_obj_set_pos(done, 32, 604);
  lv_obj_set_size(done, 410, 84);
  lv_obj_set_style_text_font(lv_obj_get_child(done, 0), &lv_font_montserrat_28, 0);

  // networks
  lv_obj_t* nc = ui_card(scr, 466, 24, 782, 664);
  lv_obj_t* nt = ui_label(nc, "Wi-Fi networks", &lv_font_montserrat_28, C_INK);
  lv_obj_set_pos(nt, 0, 4);
  ui_scan_lbl = ui_label(nc, "", &lv_font_montserrat_20, C_DIM);
  lv_obj_set_pos(ui_scan_lbl, 0, 46);
  ui_scan_btn = ui_button(nc, LV_SYMBOL_REFRESH "  Scan", [](lv_event_t*) { ui_start_scan(); }, false);
  lv_obj_set_size(ui_scan_btn, 150, 56);
  lv_obj_align(ui_scan_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
  ui_list = lv_list_create(nc);
  lv_obj_set_pos(ui_list, 0, 86);
  lv_obj_set_size(ui_list, 738, 532);
  lv_obj_set_style_bg_color(ui_list, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_border_width(ui_list, 0, 0);
  lv_obj_set_style_pad_all(ui_list, 0, 0);
  lv_obj_set_style_text_font(ui_list, &lv_font_montserrat_28, 0);

  ui_build_modal(scr);
}
