# Changelog

Versions follow [semantic versioning](https://semver.org). The firmware prints its version at boot, in the serial
`S` statistics and in its settings panel; the server prints it at start and with `--version`.

## 1.1.0 - 2026-10-14

- **Sound.** What the page plays comes out of the Tab5's speaker (or headphones). The server runs Chromium with a
  private PulseAudio/PipeWire sink, records it and streams 48 kHz stereo PCM over UDP while there is sound. The
  firmware plays it through an adaptive jitter buffer (40–250 ms, starting at 60 ms) that follows WiFi hiccups
  and the clock difference between PC and Tab5. Needs a Linux PC with `pactl` and `parec`; `--no-audio` turns it
  off. Firmware older than 1.1 keeps working without sound.
- **`--power-save auto|off`:** `off` keeps the Tab5's WiFi power save off the whole time, for the quickest
  response at the cost of battery. `auto` (the default) also switches it off while sound plays.
- **Volume** slider in the Tab5's settings, remembered. The speaker is powered only while sound plays.
- **Touch demo with sound:** the buttons and switches click, a Music switch plays an arpeggio, finger paint plays
  a tone that follows your finger, and the Volume slider sets the level.
- **Fix: dragging on a busy page no longer drops the frame rate** to 2–3 fps with touch delays growing past a second.
  The server waited for Chromium to handle each touch before reading the Tab5's next message, so its "shown" acks
  queued behind finger moves. Touches now go to the page from their own queue, and waiting moves collapse into the
  newest.
- **Faster server:** comparing frames took 24 ms and ran on the event loop, holding up touches, acks and sound.
  It now takes about 2 ms, in a worker thread with the JPEG encoding. Server CPU dropped from a full core to about
  a third on the follow-finger test, which now reaches the 30 fps cap with touch-to-screen at about 40 ms (was
  70–120 ms). Full-screen animation went from 15–17 to about 19 fps.
- **Fix:** `APPS_DIR` is resolved, so the server also starts when `__file__` is relative (e.g. under `-m cProfile`).
- **Protocol:** on connect the firmware sends a hello with its version; the server sends settings only after one.
- **New app: 老黄历 (Chinese almanac),** `--app almanac`. It shows the lunar date, 干支 and zodiac, 宜/忌, 冲煞,
  吉神方位, 彭祖百忌, solar terms and festivals, and the auspicious hours. Browse days with 前一天/后一天. Built on
  lunar-javascript (MIT), which works offline. It uses bundled LXGW WenKai and Ma Shan Zheng brush lettering,
  cut down to the characters it can show (344 KB), so no Chinese font is needed on the PC.
- **Merged demos:** the home launcher's Touch demo and Motion test tiles are now one "Touch & motion" tile; the two
  pages link to each other.

## 1.0.0 - 2026-10-13

First release.

- **Firmware:**
  - Shows pages rendered by the PC server: changed rectangles arrive as JPEG, are decoded by the ESP32-P4's
    hardware JPEG engine, and are rotated into the frame buffer by the PPA.
  - Touches are forwarded to the page.
  - A settings panel (LVGL) for WiFi, brightness and flip opens on first start and with a swipe in from any edge.
    WiFi networks are saved only once joined, and a failed new network falls back to the previous one.
  - WiFi power save is switched off while touching and while updates stream in.
- **Server:**
  - Headless Chromium mirrored through its screencast. Only changed 16×16 tiles are sent, as TV-range 4:4:4 JPEG.
  - The next frame is prepared while the previous one is in flight.
  - The Tab5 is found over USB, and the server reconnects automatically.
- **Built-in apps:** home launcher, dashboard (Open-Meteo weather and air quality, Hacker News; no API keys;
  New York unless `--city` says otherwise, `--city auto` locates by IP address),
  clock with calendar and world clocks, sticky notes with an on-screen keyboard, touch demo, motion test.
- **Pages:** `--url` for any web page, `--file` for a local page (reloaded on change), plus app settings with
  `--city`, `--units` and `--set`.
