# Changelog

Versions follow [semantic versioning](https://semver.org). The firmware prints its version at boot, in the serial
`S` statistics and in its settings panel; the server prints it at start and with `--version`.

## Unreleased

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
