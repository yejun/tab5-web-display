#!/usr/bin/env python3
"""Tab5 Web Display server: renders a web page in headless Chromium and mirrors it to an M5Stack Tab5
running the web_display firmware; touches on the Tab5 come back as touch events in the page.

The Tab5 listens on TCP 8765 and this script connects to it, so the PC needs no open port. Only changed
16x16 tiles are sent, grouped into rectangles and encoded as baseline JPEG with full-resolution colour
(4:4:4) so text stays sharp. See firmware/web_display/web_display.ino for the wire format.

    server.py                          # the home launcher; the Tab5 is found over USB
    server.py --device 192.168.1.50     # Tab5 address given
    server.py --app dashboard          # a built-in app: home, dashboard, clock, notes, demo, motion
    server.py --url https://example.com
    server.py --file my_page.html      # a local page, reloaded whenever it changes
    server.py --app dashboard --city Berlin --units metric

The built-in apps read their settings from window.TAB5: --city/--lat/--lon/--units, plus any --set key=value
(the clock takes --set zones=Europe/London,Asia/Tokyo,America/New_York). They need no API keys.
"""
import argparse
import asyncio
import base64
import io
import json
import os
import pathlib
import shutil
import signal
import struct
import sys
import time

import numpy as np
from PIL import Image
from playwright.async_api import async_playwright

__version__ = "1.0.0"

W, H, TILE, PORT = 1280, 720, 16, 8765
# Chromium's rasterisation jitters a few pixels by a few levels between frames. A tile counts as changed if some
# pixel moved by more than NOISE, or if the whole tile shifted on average by more than DRIFT (a slow fade or gradient).
NOISE, DRIFT = 6, 1.5
SETTLE_S = 0.5   # after this long without changes, send whatever still differs at all (exact cleanup pass)
RECT = struct.Struct("<4sHHHHI")   # "TAB5", x, y, w, h, jpeg length
EVENT = struct.Struct("<BBHHH")    # type, 0, x, y, 0
EV_DOWN, EV_MOVE, EV_UP, EV_SHOWN = 1, 2, 3, 5
APPS_DIR = pathlib.Path(__file__).with_name("apps")
APPS = ["home", "dashboard", "clock", "notes", "demo", "motion"]
PROFILE_DIR = pathlib.Path(os.environ.get("XDG_DATA_HOME", pathlib.Path.home() / ".local/share")) / "tab5-web-display"
ESPRESSIF_USB_VID = 0x303A
TIMEOUTS = (TimeoutError, asyncio.TimeoutError)   # the same class from Python 3.11 on, different before


def find_serial_port():
    """The Tab5's USB serial port (the ESP32-P4's built-in USB has Espressif's vendor ID)."""
    from serial.tools import list_ports
    ports = [p.device for p in list_ports.comports() if p.vid == ESPRESSIF_USB_VID]
    if not ports:
        raise SystemExit("No Tab5 found on USB. Connect it, or pass its address with --device <ip> "
                         "(shown in the Tab5's settings: swipe in from any screen edge).")
    return ports[0]


def device_ip_from_serial(port):
    """Ask the firmware for its IP address over USB serial."""
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = s.rts = True
    s.open()
    try:
        s.reset_input_buffer()
        s.write(b"ip\n")
        end = time.monotonic() + 3
        buf = b""
        while time.monotonic() < end:
            buf += s.read(256)
            for line in buf.decode(errors="replace").splitlines():
                if line.startswith("IP ") and line[3:].strip() != "none":
                    return line[3:].strip()
    finally:
        s.close()
    raise SystemExit(f"The Tab5 on {port} has no IP address yet; is it on WiFi? Or pass --device <ip>.")


def dirty_rects(prev, cur, exact=False):
    """Rectangles (x, y, w, h) covering every changed tile: one per run of changed tile rows, split where
    a row run has a wide horizontal gap. All values are multiples of TILE."""
    if prev is None:
        return [(0, 0, W, H)]
    diff = np.abs(prev.astype(np.int16) - cur).max(axis=2).reshape(H // TILE, TILE, W // TILE, TILE)
    if exact:
        tiles = diff.any(axis=(1, 3))
    else:
        tiles = (diff.max(axis=(1, 3)) > NOISE) | (diff.mean(axis=(1, 3)) > DRIFT)
    rows = tiles.any(axis=1)
    rects, r = [], 0
    while r < len(rows):
        if not rows[r]:
            r += 1
            continue
        r0 = r
        while r < len(rows) and rows[r]:
            r += 1
        cols = np.flatnonzero(tiles[r0:r].any(axis=0))
        start = last = cols[0]
        for c in list(cols[1:]) + [None]:
            if c is None or c - last > 4:
                rects.append((start * TILE, r0 * TILE, (last - start + 1) * TILE, (r - r0) * TILE))
                start = c
            if c is not None:
                last = c
    return rects


# The P4's JPEG decoder expands "TV range" BT.601 YCbCr (Y 16-235, Cb/Cr 16-240) to RGB, while JPEG writers use
# the full 0-255 range; encoding full range crushes dark greys to black. So convert to TV range ourselves: Pillow's
# RGB -> YCbCr is full-range BT.601, and squeezing each channel into TV range is a per-channel lookup table.
TV_RANGE = [round(16 + v * 219 / 255) for v in range(256)] + [round(128 + (v - 128) * 224 / 255) for v in range(256)] * 2


def encode(img, rect, quality):
    x, y, w, h = rect
    ycc = Image.fromarray(img[y:y + h, x:x + w]).convert("YCbCr").point(TV_RANGE)
    buf = io.BytesIO()
    ycc.save(buf, "JPEG", quality=quality, subsampling=0)
    return buf.getvalue()


class Mirror:
    """One Chromium page mirrored to the Tab5: frames out, touches in."""

    def __init__(self, page, cdp, args):
        self.page, self.cdp, self.args = page, cdp, args
        self.dirty = asyncio.Event()
        self.touch_at = None          # when the last touch event arrived, for the touch -> shown latency
        self.touch_in_flight = None   # that time, once an update showing its effect has been sent
        self.latest = None            # newest screencast frame (base64 PNG) not yet decoded
        self.frame = None             # newest decoded frame: what the page looks like now
        self.sent_at, self.show_times = 0.0, []   # send -> shown round trips, for the stats
        self.stats_task = None
        self.live, self.host = False, None   # live: the Tab5 is showing our updates

    @staticmethod
    def decode_png(data):
        img = Image.open(io.BytesIO(base64.b64decode(data))).convert("RGB")
        if img.size != (W, H):
            img = img.resize((W, H))
        return np.asarray(img)

    async def capture(self):
        """What the page looks like now. Chromium's screencast sends a lossless frame every time it paints, so the
        newest frame is always current; nothing else is asked of Chromium here. (Page.captureScreenshot while the
        screencast runs occasionally never returned, which froze the whole mirror.)"""
        if self.latest is None and self.frame is None:   # nothing painted yet: wait for the first frame
            for _ in range(100):
                await asyncio.sleep(0.05)
                if self.latest is not None:
                    break
            else:
                raise TimeoutError("no frame from Chromium")
        if self.latest is not None:
            data, self.latest = self.latest, None
            self.frame = await asyncio.to_thread(self.decode_png, data)
        return self.frame

    async def touch(self, kind, x, y):
        types = {EV_DOWN: "touchStart", EV_MOVE: "touchMove", EV_UP: "touchEnd"}
        points = [] if kind == EV_UP else [{"x": x, "y": y}]
        try:
            await asyncio.wait_for(self.cdp.send("Input.dispatchTouchEvent", {"type": types[kind], "touchPoints": points}), 2)
        except TIMEOUTS:
            print("touch event timed out (page busy or navigating)")
        self.touch_at = time.monotonic()
        self.dirty.set()

    async def read_events(self, reader, shown):
        while True:
            kind, _, x, y, _ = EVENT.unpack(await reader.readexactly(EVENT.size))
            if kind == EV_SHOWN:
                if not self.live:
                    self.live = True
                    print(f"showing on Tab5 {self.host}:{PORT}")
                shown.set()
                self.show_times.append(time.monotonic() - self.sent_at)
                if self.touch_in_flight is not None:
                    print(f"touch->shown {1000 * (time.monotonic() - self.touch_in_flight):4.0f} ms")
                    self.touch_in_flight = None
            elif kind in (EV_DOWN, EV_MOVE, EV_UP):
                await self.touch(kind, x, y)

    async def session(self, host):
        self.host = host
        reader, writer = await asyncio.open_connection(host, PORT)
        shown = asyncio.Event()
        shown.set()
        self.touch_in_flight = None
        events = asyncio.create_task(self.read_events(reader, shown))
        prev, last_start, needs_settle = None, 0.0, False
        stats, stats_t = [], time.monotonic()
        try:
            while not events.done():
                settle = False
                try:   # repaints wake us; the timeouts are the cleanup pass and a safety net
                    await asyncio.wait_for(self.dirty.wait(), SETTLE_S if needs_settle else 1.0)
                except TIMEOUTS:
                    settle, needs_settle = needs_settle, False
                self.dirty.clear()
                await asyncio.sleep(max(0.0, last_start + 1 / self.args.max_fps - time.monotonic()))
                # capture and encode while the previous update may still be on its way to the panel (pipelining);
                # prev already includes that update, so the diff is against what the Tab5 is about to show
                t0 = last_start = time.monotonic()
                touch_t = self.touch_at if self.touch_at is not None and self.touch_at < t0 else None
                cur = await self.capture()
                rects = dirty_rects(prev, cur, exact=settle)
                t1 = time.monotonic()
                out = bytearray()
                q = min(95, self.args.quality + 5) if settle else self.args.quality
                for r, jpg in zip(rects, await asyncio.to_thread(lambda: [encode(cur, r, q) for r in rects])):
                    out += RECT.pack(b"TAB5", *r, len(jpg)) + jpg
                t2 = time.monotonic()
                if rects:
                    out += RECT.pack(b"TAB5", 0, 0, 0, 0, 0)
                    await asyncio.wait_for(shown.wait(), 5.0)   # at most one update in flight
                    shown.clear()
                    if touch_t is not None:
                        self.touch_in_flight, self.touch_at = touch_t, None
                    self.sent_at = time.monotonic()
                    writer.write(out)
                    await asyncio.wait_for(writer.drain(), 5.0)
                    # remember only what the Tab5 now shows, so sub-threshold drift still adds up and gets sent
                    if prev is None:
                        prev = cur.copy()
                    for x, y, w, h in rects:
                        prev[y:y + h, x:x + w] = cur[y:y + h, x:x + w]
                    needs_settle = not settle
                    stats.append((len(out), sum(w * h for _, _, w, h in rects), t1 - t0, t2 - t1))
                    if self.args.verbose:
                        print(f"{len(rects)} rect(s) {len(out) / 1024:6.1f} KB | capture {1000 * (t1 - t0):3.0f} ms "
                              f"encode {1000 * (t2 - t1):3.0f} ms " + " ".join(f"{x},{y},{w}x{h}" for x, y, w, h in rects))
                elif touch_t is not None and t0 - touch_t > 1.0:
                    self.touch_at = None   # the touch changed nothing visible
                if (now := time.monotonic()) - stats_t >= 1.0:
                    await self.report(stats, now - stats_t)
                    stats, stats_t = [], now
        finally:
            events.cancel()
            writer.close()

    async def report(self, stats, secs):
        """Once a second: print a summary of continuous activity and hand it to the page (window.tab5Stats)."""
        n = len(stats)
        s = {"fps": n / secs, "kbps": sum(b for b, *_ in stats) / 1024 / secs,
             "area": 100 * sum(a for _, a, *_ in stats) / max(n, 1) / (W * H),
             "capture": 1000 * sum(c for *_, c, _ in stats) / max(n, 1), "encode": 1000 * sum(e for *_, e in stats) / max(n, 1)}
        show = 1000 * sum(self.show_times) / max(len(self.show_times), 1)
        self.show_times = []
        if n > 2:
            print(f"{s['fps']:4.1f} fps {s['kbps']:6.0f} KB/s | avg area {s['area']:4.1f}% capture {s['capture']:3.0f} ms "
                  f"encode {s['encode']:3.0f} ms send->shown {show:3.0f} ms")
        # in the background and with a timeout: the frame loop must never wait for the page
        if self.stats_task is None or self.stats_task.done():
            self.stats_task = asyncio.create_task(self.push_stats(s))

    async def push_stats(self, s):
        try:
            await asyncio.wait_for(self.page.evaluate("s => window.tab5Stats && window.tab5Stats(s)", s), 1)
        except Exception:
            pass   # page busy or navigating


async def watch_page(page, path):
    """Reload the page when a file next to it changes (handy while editing a page)."""
    def stamp():
        return max((f.stat().st_mtime for f in path.parent.iterdir() if f.is_file()), default=0)
    last = stamp()
    while True:
        await asyncio.sleep(0.5)
        if (s := stamp()) != last:
            last = s
            print("page changed, reloading")
            await page.reload()


def find_browser():
    for name in ("chromium", "chromium-browser", "google-chrome-stable", "google-chrome"):
        if path := shutil.which(name):
            return path
    return None   # Playwright's own Chromium (playwright install chromium)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    ap.add_argument("--device", help="Tab5 IP address (default: ask the Tab5 over USB)")
    ap.add_argument("--port", help="Tab5 USB serial port, used to find its IP (default: found automatically)")
    what = ap.add_mutually_exclusive_group()
    what.add_argument("--app", default="home", help=f"built-in app: {', '.join(APPS)} (default home)")
    what.add_argument("--url", help="any web page")
    what.add_argument("--file", help="a local HTML file (reloaded when it, or a file next to it, changes)")
    ap.add_argument("--city", help="weather location for the dashboard (default New York; 'auto': from the PC's IP address)")
    ap.add_argument("--lat", type=float, help="weather location as coordinates (with --lon)")
    ap.add_argument("--lon", type=float)
    ap.add_argument("--units", choices=["metric", "imperial"], help="default: by country")
    ap.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="extra setting for the apps")
    ap.add_argument("--profile", default=str(PROFILE_DIR), help=f"browser profile (keeps the notes; default {PROFILE_DIR})")
    ap.add_argument("--quality", type=int, default=88, help="JPEG quality (default 88)")
    ap.add_argument("--max-fps", type=float, default=30, help="cap on updates per second (default 30)")
    ap.add_argument("--browser", default=find_browser(), help="Chromium/Chrome executable (default: found on PATH)")
    ap.add_argument("--show", action="store_true", help="show the browser window too (for debugging)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.url:
        url, watch = args.url, None
    elif args.file:
        watch = pathlib.Path(args.file).resolve()
        if not watch.is_file():
            raise SystemExit(f"no such file: {watch}")
        url = watch.as_uri()
    else:
        name, _, query = args.app.partition("?")
        name = name.removesuffix(".html")
        if name not in APPS:
            raise SystemExit(f"unknown app {name!r}; choose from {', '.join(APPS)}")
        watch = APPS_DIR / f"{name}.html"
        url = watch.as_uri() + (f"?{query}" if query else "")
    settings = {k: v for k, v in (("city", args.city), ("lat", args.lat), ("lon", args.lon), ("units", args.units))
                if v is not None}
    for kv in args.set:
        k, sep, v = kv.partition("=")
        if not sep:
            raise SystemExit(f"--set needs KEY=VALUE, got {kv!r}")
        settings[k] = v
    host = args.device or device_ip_from_serial(args.port or find_serial_port())

    async with async_playwright() as pw:
        # a persistent profile, so the apps' localStorage (notes, clock options) survives restarts
        ctx = await pw.chromium.launch_persistent_context(
            args.profile, executable_path=args.browser, headless=not args.show,
            viewport={"width": W, "height": H}, device_scale_factor=1, has_touch=True)
        await ctx.add_init_script(f"window.TAB5 = {json.dumps(settings)};")
        page = ctx.pages[0] if ctx.pages else await ctx.new_page()
        try:
            await page.goto(url)
        except Exception as e:   # e.g. no network: Chromium shows its error page, and the user can still navigate
            print(f"could not load {url}: {str(e).splitlines()[0]}")
        cdp = await ctx.new_cdp_session(page)
        mirror = Mirror(page, cdp, args)

        # Chromium sends a (lossless) frame whenever it paints; keep only the newest.
        async def on_frame(ev):
            mirror.latest = ev["data"]
            mirror.dirty.set()
            await cdp.send("Page.screencastFrameAck", {"sessionId": ev["sessionId"]})
        cdp.on("Page.screencastFrame", on_frame)
        await cdp.send("Page.startScreencast", {"format": "png", "maxWidth": W, "maxHeight": H})
        if watch:
            asyncio.create_task(watch_page(page, watch))

        # kill -USR1 <pid> prints where every task is waiting (for diagnosing a stall; not on Windows)
        def dump_tasks():
            for t in asyncio.all_tasks():
                chain, c = [], t.get_coro()
                while c is not None:
                    frame = getattr(c, "cr_frame", None) or getattr(c, "gi_frame", None)
                    if frame:
                        chain.append(f"{frame.f_code.co_name}:{frame.f_lineno}")
                    c = getattr(c, "cr_await", None) or getattr(c, "gi_yieldfrom", None)
                print(t.get_name(), " -> ".join(chain), flush=True)
        if hasattr(signal, "SIGUSR1"):
            asyncio.get_running_loop().add_signal_handler(signal.SIGUSR1, dump_tasks)
        print(f"Tab5 Web Display {__version__}: showing {url}")
        # While the Tab5's settings are open it accepts and closes connections; keep retrying quietly, and only
        # report changes (connected, lost, a new kind of error).
        last_error = None
        while True:
            try:
                await mirror.session(host)
                error = "closed the connection (settings open?)"
            except asyncio.IncompleteReadError:
                error = "closed the connection (settings open?)"
            except (OSError, *TIMEOUTS) as e:
                error = str(e) or type(e).__name__
            if mirror.live:
                print(f"Tab5 {error}; reconnecting")
                mirror.live = False
            elif error != last_error:
                print(f"waiting for the Tab5 at {host}: {error}")
            last_error = error
            await asyncio.sleep(2)


if __name__ == "__main__":
    if sys.version_info < (3, 9):
        sys.exit("Python 3.9 or newer is needed")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
