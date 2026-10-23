// USB keyboards and mice on the Tab5's USB-A port (the P4's high-speed USB, ESP-IDF's USB Host Library; through a hub
// too, and combined receivers: keyboard, mouse, media keys). Two tasks on core 0 run the host library and this client;
// what they receive goes through queues to the main loop (usb_poll), which owns the connection, LVGL and the screen.
//
// Keyboards are put in the HID boot protocol: 8-byte reports of the modifier keys and up to six others held. Their
// differences from the last report become key presses and releases, sent to the PC as the Tab5 Keyboard's are
// (EV_KEY: HID usage code and modifier bits, which the server repeats while held), or typed into the settings panel
// while it is open. Caps Lock is kept here (and shown on the keyboard's light); Scroll Lock opens and closes the
// settings.
//
// Mice are read in the report protocol, as described by their report descriptor (parsed below), because the boot
// protocol has no wheel: a touchpad scrolls with it, and many mice have back / forward buttons. A mouse whose
// descriptor isn't understood falls back to the boot protocol (buttons, x, y). The pointer is drawn by the Tab5
// (pointer.h); a PC that takes mouse events (TCFG bit 4) gets them as EV_MOUSE: hover, clicks, the wheel. With an
// older server, or a video, the left button acts as a finger.
#pragma once
#include <usb/usb_host.h>

static constexpr int USB_DEV_MAX = 4;       // keyboards and mice at once
static constexpr uint8_t HID_CAPS_LOCK = 0x39, HID_SCROLL_LOCK = 0x47;
static constexpr int USB_CTRL_MAX = 1024;   // longest report descriptor read

struct HidField { uint16_t bit; uint8_t size; bool sgn; };   // size 0: not there
struct MouseLayout {        // where a mouse's report has what (report protocol); boot protocol if !report
  bool report = false;
  bool ids = false;         // reports start with a report ID byte ...
  uint8_t id = 0;           // ... and this is the mouse's
  HidField x = {}, y = {}, wheel = {}, pan = {}, button[5] = {};
};

struct UsbKbdReport { int8_t slot; uint8_t r[8]; };   // r all 0: the keyboard went away (its keys are let go)
struct UsbMouseReport { uint8_t buttons; int16_t dx, dy; int8_t wheel, pan; };
static QueueHandle_t g_usb_kq, g_usb_mq;
static usb_host_client_handle_t g_usb_client;

struct UsbDev {
  usb_device_handle_t dev = nullptr;
  uint8_t iface = 0, ep = 0, mps0 = 8;
  usb_transfer_t* in = nullptr;    // the interrupt IN transfer, always submitted while the device is there
  usb_transfer_t* ctrl = nullptr;  // requests to the interface
  volatile bool ctrl_done = false;
  bool gone = false, in_busy = false;
  bool mouse = false;
  uint8_t leds = 0xFF;             // keyboard: lights last set
  MouseLayout layout;
};
static UsbDev g_usbd[USB_DEV_MAX];
static uint8_t g_usb_new[8];       // addresses of new devices, for the client task (from its event callback)
static int g_usb_new_n = 0;
static volatile uint8_t g_usb_leds = 1;   // the keyboards' lights: bit 0 Num Lock (the keypad types digits), 1 Caps Lock
struct UsbStats { uint32_t keyboards, mice, reports, mouse_reports; };
static UsbStats g_ustats = {};

// ---- report descriptors ----

// Find a mouse's buttons, X, Y, wheel and horizontal wheel (AC Pan) in its report descriptor: the input fields
// inside a Mouse application collection, with their bit offsets in the report that carries them.
static bool hid_parse_mouse(const uint8_t* d, int n, MouseLayout& L) {
  struct Global { uint16_t page; int32_t lmin; uint8_t size, count, id; };
  Global g = {}, stack[4];
  int sp = 0;
  uint32_t usages[16], umin = 0, umax = 0;
  int nu = 0;
  bool range = false;
  static uint16_t off[256];   // input bits so far, per report ID (only the client task parses)
  memset(off, 0, sizeof(off));
  int depth = 0, mouse_depth = -1;
  L = MouseLayout{};
  uint8_t id_of_x = 0;
  for (int i = 0; i < n;) {
    const uint8_t b = d[i++];
    if (b == 0xFE) {   // a long item: skip it
      if (i >= n) break;
      i += 2 + d[i];
      continue;
    }
    const int sz = (b & 3) == 3 ? 4 : b & 3;
    if (i + sz > n) break;
    uint32_t u = 0;
    for (int k = 0; k < sz; ++k) u |= (uint32_t)d[i + k] << (8 * k);
    const int32_t s = sz == 1 ? (int8_t)u : sz == 2 ? (int16_t)u : (int32_t)u;
    i += sz;
    const int type = (b >> 2) & 3, tag = b >> 4;
    if (type == 1) {   // global items
      switch (tag) {
        case 0: g.page = u; break;
        case 1: g.lmin = s; break;
        case 7: g.size = u; break;
        case 8: g.id = u; L.ids = true; break;
        case 9: g.count = u; break;
        case 10: if (sp < 4) stack[sp++] = g; break;
        case 11: if (sp) g = stack[--sp]; break;
      }
    } else if (type == 2) {   // local items (a 4-byte usage includes its page)
      const uint32_t full = sz == 4 ? u : (uint32_t)g.page << 16 | u;
      if (tag == 0 && nu < 16) usages[nu++] = full;
      else if (tag == 1) { umin = full; range = true; }
      else if (tag == 2) umax = full;
    } else if (type == 0) {   // main items
      if (tag == 10) {        // collection: is it the mouse (Generic Desktop, Mouse; application)?
        ++depth;
        if (mouse_depth < 0 && u == 1 && nu && usages[0] == 0x00010002) mouse_depth = depth;
      } else if (tag == 12) {   // end of collection
        if (depth == mouse_depth) mouse_depth = -1;
        --depth;
      } else if (tag == 8) {    // input: count fields of size bits
        const bool data = !(u & 1), variable = u & 2, relative = u & 4;
        uint16_t& o = off[g.id];
        for (int k = 0; k < g.count; ++k, o += g.size) {
          if (!data || !variable || mouse_depth < 0 || g.size == 0 || g.size > 32) continue;
          const uint32_t usage = range ? (umin + k <= umax ? umin + k : 0) : nu ? usages[min(k, nu - 1)] : 0;
          const HidField f = {o, g.size, g.lmin < 0};
          if (usage == 0x00010030 && relative && !L.x.size) { L.x = f; id_of_x = g.id; }
          else if (g.id != id_of_x && L.x.size) continue;   // (the rest must be in the same report)
          else if (usage == 0x00010031 && relative && !L.y.size) L.y = f;
          else if (usage == 0x00010038 && !L.wheel.size) L.wheel = f;
          else if (usage == 0x000C0238 && !L.pan.size) L.pan = f;
          else if ((usage >> 16) == 9 && (usage & 0xFFFF) >= 1 && (usage & 0xFFFF) <= 5) L.button[(usage & 0xFFFF) - 1] = f;
        }
      }
      nu = 0;   // local items last until the next main item
      range = false;
      umin = umax = 0;
    }
  }
  L.id = id_of_x;
  L.report = L.x.size && L.y.size;
  return L.report;
}

static int32_t hid_get(const uint8_t* r, int len, const HidField& f) {
  if (!f.size || (f.bit + f.size + 7) / 8 > len) return 0;
  uint32_t v = 0;
  for (int k = 0; k < f.size; ++k) v |= (uint32_t)((r[(f.bit + k) >> 3] >> ((f.bit + k) & 7)) & 1) << k;
  if (f.sgn && f.size < 32 && (v >> (f.size - 1)) & 1) v |= ~0u << f.size;
  return (int32_t)v;
}

static void usb_mouse_report(const UsbDev& k, const uint8_t* r, int len) {
  UsbMouseReport m = {};
  const MouseLayout& L = k.layout;
  if (!L.report) {   // boot protocol: buttons, x, y (some mice add a wheel)
    if (len < 3) return;
    m = {(uint8_t)(r[0] & 7), (int8_t)r[1], (int8_t)r[2], len >= 4 ? (int8_t)r[3] : (int8_t)0, 0};
  } else {
    if (L.ids) {
      if (len < 1 || r[0] != L.id) return;   // another of the device's reports (media keys, ...)
      ++r;
      --len;
    }
    for (int b = 0; b < 5; ++b)
      if (hid_get(r, len, L.button[b])) m.buttons |= 1 << b;
    m.dx = constrain(hid_get(r, len, L.x), -32768, 32767);
    m.dy = constrain(hid_get(r, len, L.y), -32768, 32767);
    m.wheel = constrain(hid_get(r, len, L.wheel), -127, 127);
    m.pan = constrain(hid_get(r, len, L.pan), -127, 127);
  }
  ++g_ustats.mouse_reports;
  xQueueSend(g_usb_mq, &m, 0);
}

// ---- the client task ----

static void usb_in_done(usb_transfer_t* t) {
  UsbDev& k = g_usbd[(intptr_t)t->context];
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (k.mouse) {
      usb_mouse_report(k, t->data_buffer, t->actual_num_bytes);
    } else if (t->actual_num_bytes >= 8) {
      UsbKbdReport rep{(int8_t)(intptr_t)t->context, {}};
      memcpy(rep.r, t->data_buffer, 8);
      ++g_ustats.reports;
      xQueueSend(g_usb_kq, &rep, 0);
    }
  } else if (t->status != USB_TRANSFER_STATUS_TIMED_OUT) {   // gone, cancelled, stalled: stop
    k.in_busy = false;
    return;
  }
  if (k.gone || usb_host_transfer_submit(t) != ESP_OK) k.in_busy = false;
}

static void usb_ctrl_done(usb_transfer_t* t) { g_usbd[(intptr_t)t->context].ctrl_done = true; }

// A request to the device's interface, waited for (up to 500 ms). Returns the bytes of the data stage, or -1.
static int usb_control(UsbDev& k, uint8_t type, uint8_t request, uint16_t value, uint16_t len, const uint8_t* out = nullptr) {
  usb_setup_packet_t* s = (usb_setup_packet_t*)k.ctrl->data_buffer;
  s->bmRequestType = type;
  s->bRequest = request;
  s->wValue = value;
  s->wIndex = k.iface;
  s->wLength = len;
  if (out && len) memcpy(k.ctrl->data_buffer + sizeof(usb_setup_packet_t), out, len);
  k.ctrl->num_bytes = sizeof(usb_setup_packet_t) + (type & 0x80 ? usb_round_up_to_mps(len, k.mps0) : len);
  k.ctrl->device_handle = k.dev;
  k.ctrl->bEndpointAddress = 0;
  k.ctrl->callback = usb_ctrl_done;
  k.ctrl_done = false;
  if (usb_host_transfer_submit_control(g_usb_client, k.ctrl) != ESP_OK) return -1;
  for (int i = 0; i < 50 && !k.ctrl_done; ++i) usb_host_client_handle_events(g_usb_client, pdMS_TO_TICKS(10));
  if (!k.ctrl_done || k.ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return -1;
  return k.ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
}

// What a device is, on the serial console: its IDs and name.
static void usb_describe(uint8_t addr, usb_device_handle_t dev) {
  const usb_device_desc_t* dd;
  usb_device_info_t info;
  char name[64] = "";
  if (usb_host_device_info(dev, &info) == ESP_OK && info.str_desc_product) {
    const usb_str_desc_t* sd = info.str_desc_product;
    int n = 0;
    for (int i = 0; i < (sd->bLength - 2) / 2 && n < (int)sizeof(name) - 1; ++i)
      name[n++] = sd->wData[i] < 128 ? (char)sd->wData[i] : '?';
    name[n] = 0;
  }
  if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK)
    Serial.printf("usb: device %u: %04x:%04x \"%s\"\n", addr, dd->idVendor, dd->idProduct, name);
}

// A new device: claim its boot keyboard and mouse interfaces, if it has them.
static void usb_open(uint8_t addr) {
  usb_device_handle_t dev;
  if (usb_host_device_open(g_usb_client, addr, &dev) != ESP_OK) return;
  const usb_config_desc_t* cfg;
  const usb_device_desc_t* dd;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || usb_host_get_device_descriptor(dev, &dd) != ESP_OK) {
    usb_host_device_close(g_usb_client, dev);
    return;
  }
  usb_describe(addr, dev);
  int off = 0, rd_len = 0;
  const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
  const usb_intf_desc_t* hid_if = nullptr;
  bool claimed = false;
  while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
      hid_if = i->bInterfaceClass == 3 && i->bInterfaceSubClass == 1 &&
               (i->bInterfaceProtocol == 1 || i->bInterfaceProtocol == 2) && i->bAlternateSetting == 0 ? i : nullptr;
      rd_len = 0;
    } else if (d->bDescriptorType == 0x21 && hid_if) {   // HID descriptor: its report descriptor's length
      const uint8_t* h = (const uint8_t*)d;
      rd_len = h[7] | (h[8] << 8);
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && hid_if) {
      const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
      if (!(e->bEndpointAddress & 0x80) || (e->bmAttributes & 3) != USB_BM_ATTRIBUTES_XFER_INT) continue;
      int slot = 0;
      while (slot < USB_DEV_MAX && g_usbd[slot].dev) ++slot;
      if (slot == USB_DEV_MAX) break;
      if (usb_host_interface_claim(g_usb_client, dev, hid_if->bInterfaceNumber, 0) != ESP_OK) break;
      UsbDev& k = g_usbd[slot];
      k = UsbDev{};
      k.dev = dev;
      k.iface = hid_if->bInterfaceNumber;
      k.ep = e->bEndpointAddress;
      k.mps0 = dd->bMaxPacketSize0;
      k.mouse = hid_if->bInterfaceProtocol == 2;
      const int mps = USB_EP_DESC_GET_MPS(e);
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + USB_CTRL_MAX, 0, &k.ctrl);
      usb_host_transfer_alloc(mps, 0, &k.in);
      k.ctrl->context = (void*)(intptr_t)slot;
      if (k.mouse && rd_len > 0 && rd_len <= USB_CTRL_MAX) {   // its report descriptor (GET_DESCRIPTOR, Report)
        const int got = usb_control(k, 0x81, 0x06, 0x2200, rd_len);
        if (got > 0) hid_parse_mouse(k.ctrl->data_buffer + sizeof(usb_setup_packet_t), got, k.layout);
      }
      usb_control(k, 0x21, 0x0B, k.layout.report ? 1 : 0, 0);   // SET_PROTOCOL: report or boot
      k.in->device_handle = dev;
      k.in->bEndpointAddress = k.ep;
      k.in->callback = usb_in_done;
      k.in->context = (void*)(intptr_t)slot;
      k.in->num_bytes = mps;
      k.in_busy = usb_host_transfer_submit(k.in) == ESP_OK;
      claimed = true;
      ++(k.mouse ? g_ustats.mice : g_ustats.keyboards);
      if (k.mouse) {
        const MouseLayout& L = k.layout;
        if (L.report)
          Serial.printf("usb: mouse at address %u (interface %u): report %u, x %u:%u y %u:%u wheel %u:%u pan %u:%u buttons", addr,
                        k.iface, L.id, L.x.bit, L.x.size, L.y.bit, L.y.size, L.wheel.bit, L.wheel.size, L.pan.bit, L.pan.size);
        else
          Serial.printf("usb: mouse at address %u (interface %u): boot protocol (report descriptor %d bytes not understood)",
                        addr, k.iface, rd_len);
        for (const HidField& b : L.button)
          if (b.size) Serial.printf(" %u", b.bit);
        Serial.println();
      } else {
        Serial.printf("usb: keyboard at address %u (interface %u)\n", addr, k.iface);
      }
      hid_if = nullptr;   // the interface's first IN endpoint
    }
  }
  if (!claimed) usb_host_device_close(g_usb_client, dev);
}

static void usb_client_event(const usb_host_client_event_msg_t* m, void*) {
  if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (g_usb_new_n < (int)sizeof(g_usb_new)) g_usb_new[g_usb_new_n++] = m->new_dev.address;
  } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    for (UsbDev& k : g_usbd)
      if (k.dev == m->dev_gone.dev_hdl) k.gone = true;
  }
}

static void usb_client_task(void*) {
  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 8;
  cc.async.client_event_callback = usb_client_event;
  if (usb_host_client_register(&cc, &g_usb_client) != ESP_OK) { vTaskDelete(nullptr); return; }
  for (;;) {
    usb_host_client_handle_events(g_usb_client, pdMS_TO_TICKS(50));
    while (g_usb_new_n) usb_open(g_usb_new[--g_usb_new_n]);
    for (int i = 0; i < USB_DEV_MAX; ++i) {
      UsbDev& k = g_usbd[i];
      if (k.dev && !k.gone && !k.mouse && k.leds != g_usb_leds) {   // the keyboard's lights (SET_REPORT, output)
        const uint8_t leds = g_usb_leds;
        usb_control(k, 0x21, 0x09, 0x0200, 1, &leds);
        k.leds = leds;   // (also if it failed: some keyboards have no lights, don't keep asking)
      }
      if (!k.dev || !k.gone) continue;   // unplugged: stop its transfer, then let it go
      if (k.in_busy) {
        usb_host_endpoint_halt(k.dev, k.ep);
        usb_host_endpoint_flush(k.dev, k.ep);
        continue;   // its callback comes with the next events
      }
      usb_host_endpoint_clear(k.dev, k.ep);
      usb_host_interface_release(g_usb_client, k.dev, k.iface);
      bool shared = false;   // the device's other interface (a receiver's keyboard and mouse) still open
      for (int j = 0; j < USB_DEV_MAX; ++j) shared |= j != i && g_usbd[j].dev == k.dev;
      if (!shared) usb_host_device_close(g_usb_client, k.dev);
      usb_host_transfer_free(k.in);
      usb_host_transfer_free(k.ctrl);
      const bool mouse = k.mouse;
      k = UsbDev{};
      if (mouse) {
        const UsbMouseReport up = {};   // its buttons up
        xQueueSend(g_usb_mq, &up, portMAX_DELAY);
      } else {
        UsbKbdReport rep{(int8_t)i, {}};   // its keys up
        xQueueSend(g_usb_kq, &rep, portMAX_DELAY);
      }
      Serial.printf("usb: %s unplugged\n", mouse ? "mouse" : "keyboard");
    }
  }
}

static void usb_lib_task(void*) {
  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hc) != ESP_OK) {
    Serial.println("usb: host library failed to start");
    vTaskDelete(nullptr);
    return;
  }
  xTaskCreatePinnedToCore(usb_client_task, "usb_hid", 4096, nullptr, 3, nullptr, 0);
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

static void usb_setup() {
  g_usb_kq = xQueueCreate(32, sizeof(UsbKbdReport));
  g_usb_mq = xQueueCreate(64, sizeof(UsbMouseReport));
  M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);   // 5 V on the USB-A port (off after power-up)
  xTaskCreatePinnedToCore(usb_lib_task, "usb_host", 4096, nullptr, 4, nullptr, 0);
}

// ---- the main loop's side: keyboards ----

static uint8_t g_usb_held[USB_DEV_MAX][8];   // the last report of each keyboard
static uint8_t g_usb_mods = 0;               // modifier bits held on any of them (Ctrl 1, Shift 2, Alt 4, GUI 8)
static bool g_caps = false;

static bool usb_has(const uint8_t* r, uint8_t code) {
  for (int i = 2; i < 8; ++i)
    if (r[i] == code) return true;
  return false;
}

static uint8_t usb_key_mods(uint8_t code) {   // what goes with a key: the modifiers, and Caps Lock on letters
  return g_usb_mods ^ (g_caps && code >= 0x04 && code <= 0x1D ? KMOD_SHIFT : 0);
}

static void usb_kbd_release_all() {   // (the settings panel opens: the page lets go of everything)
  for (auto& held : g_usb_held) {
    for (int i = 2; i < 8; ++i)
      if (held[i] > 3 && !g_panel_open) send_event(EV_KEY, held[i], usb_key_mods(held[i]), 0);
    memset(held, 0, 8);
  }
}

static void usb_kbd_poll() {
  UsbKbdReport rep;
  while (xQueueReceive(g_usb_kq, &rep, 0) == pdTRUE) {
    uint8_t* held = g_usb_held[rep.slot];
    const uint8_t* r = rep.r;
    if (r[2] == 1) continue;   // ErrorRollOver: too many keys at once, nothing to go by
    ++g_kbd.keys;
    g_last_touch = now_ms();   // activity, as a key on the Tab5 Keyboard
    g_usb_mods = 0;
    memcpy(held, r, 2);
    for (const auto& h : g_usb_held) g_usb_mods |= (h[0] | h[0] >> 4) & 0x0F;   // left and right alike
    if (usb_has(r, HID_SCROLL_LOCK) && !usb_has(held, HID_SCROLL_LOCK)) {   // the settings panel
      usb_kbd_release_all();
      memcpy(held, r, 8);
      if (g_panel_open) panel_close();
      else panel_open();
      continue;
    }
    for (int i = 2; i < 8; ++i)   // keys let go
      if (held[i] > 3 && held[i] != HID_SCROLL_LOCK && held[i] != HID_CAPS_LOCK && !usb_has(r, held[i]) && !g_panel_open)
        send_event(EV_KEY, held[i], usb_key_mods(held[i]), 0);
    for (int i = 2; i < 8; ++i) {   // keys pressed
      const uint8_t c = r[i];
      if (c <= 3 || c == HID_SCROLL_LOCK || usb_has(held, c)) continue;
      if (c == HID_CAPS_LOCK) {
        g_caps = !g_caps;
        g_usb_leds = 1 | (g_caps ? 2 : 0);
      } else if (g_panel_open) {
        kbd_type_in_panel({c, usb_key_mods(c)});
      } else {
        send_event(EV_KEY, c, usb_key_mods(c), 1);
      }
    }
    memcpy(held, r, 8);
  }
}

// ---- the main loop's side: mice ----

struct Mouse {
  uint8_t buttons = 0, sent_buttons = 0;
  float fx = SCR_W / 2, fy = SCR_H / 2;   // the pointer, with the fractions of pixels
  int wheel = 0, pan = 0;                  // not yet sent
  int sent_x = -1, sent_y = -1;
  int64_t sent_at = 0;
  bool finger = false;                     // the left button stands in for a finger (EV_DOWN sent)
};
static Mouse g_mouse;

// A mouse count becomes pixels: 1:1 when slow, up to 3x when fast (as a desktop's pointer).
static float mouse_accel(int d) { return d * min(1.0f + abs(d) * 0.06f, 3.0f); }

static void mouse_send() {
  Mouse& m = g_mouse;
  const int x = g_ptr.x, y = g_ptr.y;
  if (g_mouse_pc) {   // buttons (bits 0-4), the keyboards' Ctrl / Shift / Alt (bits 5-7); z: wheel, pan (int8 each)
    uint8_t mods = g_usb_mods | (g_kbd.ctrl ? KMOD_CTRL : 0) | (g_kbd.aa ? KMOD_SHIFT : 0) | (g_kbd.alt ? KMOD_ALT : 0);
    const int w = constrain(m.wheel, -127, 127), p = constrain(m.pan, -127, 127);
    send_event(EV_MOUSE, x, y, (uint8_t)((m.buttons & 0x1F) | (mods & 7) << 5), (uint16_t)((uint8_t)(int8_t)w | (uint8_t)(int8_t)p << 8));
    m.wheel -= w;
    m.pan -= p;
  } else {            // a finger: down, moves and up with the left button
    const bool left = m.buttons & 1;
    if (left && !m.finger) send_event(EV_DOWN, x, y);
    else if (left) send_event(EV_MOVE, x, y);
    else if (m.finger) send_event(EV_UP, x, y);
    m.finger = left;
    m.wheel = m.pan = 0;   // (no wheel for a finger)
  }
  m.sent_x = x;
  m.sent_y = y;
  m.sent_buttons = m.buttons;
  m.sent_at = now_ms();
}

static void mouse_poll() {
  Mouse& m = g_mouse;
  UsbMouseReport rep;
  bool any = false, pressed = false;
  while (xQueueReceive(g_usb_mq, &rep, 0) == pdTRUE) {
    any = true;
    pressed |= rep.buttons & ~m.buttons & 1;
    m.fx = constrain(m.fx + mouse_accel(rep.dx), 0.0f, (float)(SCR_W - 1));
    m.fy = constrain(m.fy + mouse_accel(rep.dy), 0.0f, (float)(SCR_H - 1));
    m.buttons = rep.buttons;
    m.wheel += g_scroll_rev ? -rep.wheel : rep.wheel;   // (the settings' reverse scrolling)
    m.pan += g_scroll_rev ? -rep.pan : rep.pan;
    if (rep.buttons != m.sent_buttons) break;   // a click: send it before the moves after it
  }
  if (any) {
    g_last_touch = now_ms();
    g_ptr.shown = true;
    g_ptr.x = (int)m.fx;
    g_ptr.y = (int)m.fy;
    g_ptr.buttons = m.buttons;
  }
  const bool page = g_have_client && g_shown_any;
  if (g_panel_open || !page) {   // the settings read the pointer themselves (LVGL); nothing mirrored: a click opens
    if (!g_panel_open && pressed) panel_open();   // them, as a tap does
    m.wheel = m.pan = 0;
    m.sent_buttons = m.buttons;
    m.finger = false;
    if (!g_panel_open) ptr_update();
    return;
  }
  ptr_update();
  const bool moved = g_ptr.x != m.sent_x || g_ptr.y != m.sent_y;
  if (m.buttons != m.sent_buttons || m.wheel || m.pan || (moved && now_ms() - m.sent_at >= 16 && (g_mouse_pc || m.finger)))
    mouse_send();
}

static void usb_poll() {
  if (!g_usb_kq) return;
  usb_kbd_poll();
  mouse_poll();
}
