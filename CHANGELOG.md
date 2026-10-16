# Changelog

Versions follow [semantic versioning](https://semver.org). The firmware prints its version at boot, in the serial
`S` statistics and in its settings panel; the server prints it at start and with `--version`.

## Unreleased

- **Videos:** `--file clip.mp4` plays any video ffmpeg can decode, in a loop and with sound (no PulseAudio needed).
  Tap to pause, drag sideways to seek (across the screen is 2 minutes; on the bar, the position follows the
  finger). A bar at the bottom shows the time, length and file name while paused or seeking and for 3 s after a
  touch. ffmpeg decodes the picture and the sound; the server keeps the clock and feeds the frames through the
  usual tile pipeline. The quality of moving frames adapts to the measured WiFi rate: detailed 720p footage plays
  at 26–29 fps (it was 18–19 at a fixed quality 70), simple footage at 30. Playback pauses while the Tab5 is
  disconnected (e.g. settings open).
- **Chrome and Edge found on Windows and macOS:** the server looks in their usual install folders when no browser
  is on the PATH, so `--browser` is rarely needed. Edge works like Chrome.
- **Lanes (firmware 1.3 with server 1.3): the stream spread over two TCP connections.** One connection carries at
  most about 3 MB/s to the Tab5, because its 64 KB receive window is fixed; two carried 4.4 MB/s in tests (WiFi 6).
  Each lane is drained into PSRAM by its own task, so the network stack's internal RAM isn't filled while frames
  decode. Four minutes of detailed video: 2.83 MB/s instead of 2.50, quality 49 instead of 41 at the same 27 fps,
  no stall over 250 ms, and the same internal-RAM margin as one connection (three lanes halved it for 3 % more).
  If a "shown" is 150 ms overdue the server sends one more update, so a packet lost at the end of a lane's burst
  is resent at once. `--lanes 1` keeps a single connection; older firmware and servers use one as before.
- **Nudges (firmware):** with the stream on the lanes, a lost event from the Tab5 (a "shown" or a touch) waited for
  lwIP's retransmission timer, over a second. While the page is busy the Tab5 now sends a tiny event after each
  one, so a loss is resent at once.
- **`wifiinfo` serial command:** shows whether the Tab5 uses WiFi 4 or WiFi 6. It does WiFi 6 only on a 2.4 GHz
  network that offers it on 20 MHz channels; a router's "force WiFi 4" for IoT networks costs throughput.
- **Statistics:** the serial `S` output includes lane and internal-memory figures.
- **Fix (firmware): touches waited while updates kept coming.** The Tab5 handled every update already received
  before reading the touch screen again, so with the link busy all the time (a video) a tap could reach the PC
  seconds late. It now handles one update per pass of its main loop.
- **Battery in the settings panel** (firmware): charge level, charging or not, voltage, current and a rough time
  to full or time left, at the top right. The level comes from the resting voltage (the reading corrected for the
  charge or load current), and a pack counts as present only after 8 s of steady readings, since the charger's
  output swings between about 4 and 8 V with no pack fitted. The serial `S` statistics include the same line.

## 1.2.2 - 2026-10-15

Server only; use it with firmware 1.2.0.

- **Dynamic quality.** Still content now goes out at JPEG quality 95 (was 88): on the Tab5's 16-bit panel that
  leaves about 0.05 % of pixels on text-heavy pages visibly off, against about 0.6 %. During animations,
  mostly-moving parts go out at quality 70 (85 for small changes) with half-resolution colour, other parts at 88.
  Every tile sent below the top quality is refined to 95 once it has stood still for 0.5 s, the oldest first and
  at most a tenth of the screen per update while something else moves. Frame rates are unchanged; data during
  animations is about 7 % higher, for the refinement. `--quality` now sets the top quality (default 95).

## 1.2.1 - 2026-10-14

Server only; use it with firmware 1.2.0.

- **Fix: YouTube stopped after a minute or so** with "Something went wrong". It treated the headless browser as a
  bot, from its "HeadlessChrome" user agent and `navigator.webdriver`. The browser now presents itself like a
  normal Chrome: the same user agent without "Headless", and no automation flag.
- **Version check:** the server prints the Tab5's firmware version when it connects, and a note when firmware and
  server are from different releases.

## 1.2.0 - 2026-10-14

About twice the frame rate: small animations up to 58 fps (was 22–28), full-screen ones about 30 (was 15–19).

- **Strips:** large changes go out as full-height strips, rotated by the server into the panel's orientation, and
  the Tab5 decodes them straight into its frame buffer. Measurements showed the PPA copy into the frame buffer cost
  more than decoding (45 ms per full screen; PSRAM bandwidth is the limit). The server picks rectangles or strips by
  their measured drawing cost.
- **Half-resolution colour for moving parts** (4:2:0, about 40 % faster to draw). Tiles sent that way are sent again
  in full colour once they have stood still for 0.5 s, so text ends up sharp even next to an endless animation.
- **Two updates in flight during animations**, except right after a touch. `--max-fps` now defaults to 60.
- **Firmware:** the main loop sleeps 1 ms instead of 5 ms while updates stream in. The hello also reports whether
  strips are supported and the screen orientation. 1.1 firmware keeps working with this server (rectangles only).
- **Fix: stripes along drawn lines.** The Tab5's JPEG decoder garbles some images only 16 pixels wide (every other
  line, depending on the content). Rectangles are now at least 32 pixels wide. This bug was there since 1.0.
- **Clear message when the browser profile is in use** (another server running) instead of a traceback.

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
