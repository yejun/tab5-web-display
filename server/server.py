#!/usr/bin/env python3
"""Tab5 Web Display server: renders a web page in headless Chromium and mirrors it to an M5Stack Tab5
running the web_display firmware; touches on the Tab5 come back as touch events in the page.

The Tab5 listens on TCP 8765 and this script connects to it, so the PC needs no open port. Only changed
16x16 tiles are sent, grouped into rectangles and encoded as baseline JPEG with full-resolution colour
(4:4:4) so text stays sharp. See firmware/web_display/web_display.ino for the wire format.

Sound (Linux, with PulseAudio or PipeWire): Chromium plays into a private sink, which is recorded and sent to
the Tab5 as UDP (see firmware/web_display/audio.h). Nothing is sent while the page is quiet.

    server.py                          # the home launcher; the Tab5 is found over USB
    server.py --device 192.168.1.50     # Tab5 address given
    server.py --app dashboard          # a built-in app: home, dashboard, clock, notes, almanac, demo, motion
    server.py --url https://example.com
    server.py --file my_page.html      # a local page, reloaded whenever it changes
    server.py --app dashboard --city Berlin --units metric
    server.py --url https://example.com --power-save off   # quickest response, shortest battery life

The built-in apps read their settings from window.TAB5: --city/--lat/--lon/--units, plus any --set key=value
(the clock takes --set zones=Europe/London,Asia/Tokyo,America/New_York). They need no API keys.
"""
import argparse
import asyncio
import base64
import collections
import io
import json
import os
import pathlib
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

import numpy as np
from PIL import Image
from playwright.async_api import async_playwright

__version__ = "1.2.0"

W, H, TILE, PORT = 1280, 720, 16, 8765
# Chromium's rasterisation jitters a few pixels by a few levels between frames. A tile counts as changed if some
# pixel moved by more than NOISE, or if the whole tile shifted on average by more than DRIFT (a slow fade or gradient).
NOISE, DRIFT = 6, 1.5
SETTLE_S = 0.5   # after this long without changes, send whatever still differs at all (exact cleanup pass)
# Drawing cost on the Tab5 in ns per pixel (measured; PSRAM bandwidth is the limit): a rectangle is decoded and
# then copied into the frame buffer by the PPA, a strip (full-height columns) is decoded straight into it.
RECT_NS, STRIP_NS = 75, 27
# Updates closer together than this are an animation. Its moving parts go out with half-resolution colour (4:2:0),
# which the Tab5 draws ~40 % faster, and two updates may be in flight. A tile sent that way is sent again in full
# colour (4:4:4) once it has stood still for SETTLE_S, so text ends up sharp even next to an endless animation.
MOTION_S = 0.2
SOFT_SHARE = 0.75     # a piece goes out in 4:2:0 only if this share of its tiles is moving
SOFT_MIN = 48         # ... and is at least this big both ways: the Tab5's decoder times out on smaller 4:2:0 images
TOUCH_QUIET_S = 0.5   # ... but only one in flight this soon after a touch, where delay matters more than frame rate
RECT = struct.Struct("<4sHHHHI")   # "TAB5" (or "TABS", a strip), x, y, w, h, jpeg length
EVENT = struct.Struct("<BBHHH")    # type, flags, x, y, z
EV_DOWN, EV_MOVE, EV_UP, EV_SHOWN, EV_HELLO = 1, 2, 3, 5, 6
HELLO_SOUND, HELLO_STRIPS, HELLO_FLIPPED = 1, 2, 4
AUDIO = struct.Struct("<4sIHH")    # "TAUD", stream position of the first frame, frames, 0
AUDIO_RATE, AUDIO_FRAMES = 48000, 360   # 7.5 ms of 16-bit stereo per datagram: 1440 bytes, one WiFi frame
AUDIO_TAIL_S = 0.5                 # keep sending this long into silence, so a short pause doesn't restart the stream
POWER_SAVE = {"auto": 0, "off": 1}
SINK_PREFIX = "tab5_web_display_"
APPS_DIR = pathlib.Path(__file__).resolve().with_name("apps")
APPS = ["home", "dashboard", "clock", "notes", "almanac", "demo", "motion"]
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


def changed_tiles(prev, cur, exact=False):
    """Which 16x16 tiles differ, as a (H/TILE, W/TILE) bool array: by more than the rendering noise, or with exact,
    at all."""
    if prev is None:
        return np.ones((H // TILE, W // TILE), bool)
    # per-pixel difference (largest of R, G, B) in uint8, then each tile's max and sum, one axis at a time:
    # about 2 ms per frame where widening to int16 and reducing two axes at once took 24 ms
    d = np.maximum(prev, cur)
    d -= np.minimum(prev, cur)
    d = np.maximum(np.maximum(d[..., 0], d[..., 1]), d[..., 2]).reshape(H // TILE, TILE, W // TILE, TILE)
    peak = d.max(axis=1).max(axis=2)
    if exact:
        return peak > 0
    total = d.sum(axis=1, dtype=np.uint16).sum(axis=2, dtype=np.uint32)
    return (peak > NOISE) | (total > DRIFT * TILE * TILE)


def runs(mask, gap=3):
    """(start, end) of the runs of True in a 1-D mask, joining runs separated by at most gap False."""
    idx = np.flatnonzero(mask)
    out = []
    for i in idx:
        if out and i - out[-1][1] <= gap:
            out[-1][1] = i + 1
        else:
            out.append([i, i + 1])
    return [tuple(r) for r in out]


def dirty_rects(tiles):
    """Rectangles (x, y, w, h) covering the changed tiles: one per run of changed tile rows, split where a row run
    has a wide horizontal gap. All values are multiples of TILE, and widths at least 2 * TILE: the Tab5's JPEG
    decoder garbles some images only 16 pixels wide (every other line, depending on the content)."""
    rects = []
    for r0, r1 in runs(tiles.any(axis=1), gap=0):
        for c0, c1 in runs(tiles[r0:r1].any(axis=0)):
            x, w = c0 * TILE, (c1 - c0) * TILE
            if w < 2 * TILE:
                x, w = min(x, W - 2 * TILE), 2 * TILE
            rects.append((x, r0 * TILE, w, (r1 - r0) * TILE))
    return rects


def plan(tiles, strips):
    """What to send for the changed tiles: ("rect", x, y, w, h) and, if the Tab5 takes them, ("strip", x, 0, w, H)
    for column ranges where redrawing the full height straight into the frame buffer is cheaper."""
    rects = dirty_rects(tiles)
    if not strips:
        return [("rect", *r) for r in rects]
    out = []
    for c0, c1 in runs(tiles.any(axis=0)):
        x0, x1 = c0 * TILE, c1 * TILE
        group = [r for r in rects if x0 <= r[0] < x1]
        if (x1 - x0) * H * STRIP_NS < sum(w * h for _, _, w, h in group) * RECT_NS:
            out.append(("strip", x0, 0, x1 - x0, H))
        else:
            out += [("rect", *r) for r in group]
    return out


# The P4's JPEG decoder expands "TV range" BT.601 YCbCr (Y 16-235, Cb/Cr 16-240) to RGB, while JPEG writers use
# the full 0-255 range; encoding full range crushes dark greys to black. So convert to TV range ourselves: Pillow's
# RGB -> YCbCr is full-range BT.601, and squeezing each channel into TV range is a per-channel lookup table.
TV_RANGE = [round(16 + v * 219 / 255) for v in range(256)] + [round(128 + (v - 128) * 224 / 255) for v in range(256)] * 2


def encode(pixels, quality, subsampling):
    ycc = Image.fromarray(np.ascontiguousarray(pixels)).convert("YCbCr").point(TV_RANGE)
    buf = io.BytesIO()
    ycc.save(buf, "JPEG", quality=quality, subsampling=subsampling)
    return buf.getvalue()


def piece_pixels(img, piece, flipped):
    """The pixels to encode for a plan() piece. A strip is turned into the panel's portrait orientation, so the
    Tab5 can decode it straight into its frame buffer: logical (x, y) is panel (719 - y, x), or (y, 1279 - x)
    when the screen is flipped."""
    kind, x, y, w, h = piece
    if kind == "rect":
        return img[y:y + h, x:x + w]
    cols = img[:, x:x + w].transpose(1, 0, 2)
    return cols[::-1] if flipped else img[::-1, x:x + w].transpose(1, 0, 2)


class SystemAudio:
    """Chromium plays into a private PulseAudio/PipeWire sink (it honours PULSE_SINK); the sink's monitor is
    recorded with parec and sent to the Tab5 as UDP datagrams while there is sound."""

    def __init__(self):
        self.sink = f"{SINK_PREFIX}{os.getpid()}"
        self.module = None
        self.target = None   # (ip, port) of a Tab5 that can play sound, or None

    @staticmethod
    def pactl(*args):
        return subprocess.run(["pactl", *args], capture_output=True, text=True, timeout=5, check=True).stdout

    def open(self):
        if not (shutil.which("pactl") and shutil.which("parec")):
            print("sound: off (needs PulseAudio or PipeWire, with pactl and parec)")
            return False
        try:
            # sinks left behind by a server that was killed
            for line in self.pactl("list", "short", "modules").splitlines():
                idx, _, rest = line.partition("\t")
                if f"sink_name={SINK_PREFIX}" in rest:
                    pid = int(rest.split(f"sink_name={SINK_PREFIX}")[1].split()[0])
                    try:
                        os.kill(pid, 0)
                    except ProcessLookupError:
                        self.pactl("unload-module", idx)
                    except PermissionError:
                        pass
            self.module = self.pactl("load-module", "module-null-sink", f"sink_name={self.sink}",
                                     "sink_properties=device.description=Tab5-Web-Display").strip()
        except (OSError, ValueError, subprocess.SubprocessError) as e:
            print(f"sound: off (could not create an audio sink: {e})")
            return False
        return True

    def close(self):
        if self.module:
            try:
                self.pactl("unload-module", self.module)
            except (OSError, subprocess.SubprocessError):
                pass
            self.module = None

    async def run(self):
        silence = bytes(AUDIO_FRAMES * 4)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setblocking(False)
        proc = await asyncio.create_subprocess_exec(
            "parec", "-d", f"{self.sink}.monitor", "--raw", "--format=s16le", f"--rate={AUDIO_RATE}", "--channels=2",
            "--latency-msec=10", stdout=asyncio.subprocess.PIPE)
        pos, loud_until = 0, 0.0
        try:
            while True:
                pcm = await proc.stdout.readexactly(len(silence))
                now = time.monotonic()
                if pcm != silence:
                    loud_until = now + AUDIO_TAIL_S
                if self.target and now < loud_until:
                    try:
                        sock.sendto(AUDIO.pack(b"TAUD", pos, AUDIO_FRAMES, 0) + pcm, self.target)
                    except OSError:
                        pass   # a lost datagram is a 7.5 ms gap; the next one goes out regardless
                pos = (pos + AUDIO_FRAMES) & 0xFFFFFFFF
        except asyncio.IncompleteReadError:
            print("sound: the recorder stopped")
        finally:
            if proc.returncode is None:
                proc.kill()
            sock.close()


class Mirror:
    """One Chromium page mirrored to the Tab5: frames out, touches in."""

    def __init__(self, page, cdp, args, audio):
        self.page, self.cdp, self.args, self.audio = page, cdp, args, audio
        self.writer = None
        self.dirty = asyncio.Event()
        self.touch_at = None          # when the last touch event arrived, for the touch -> shown latency
        self.latest = None            # newest screencast frame (base64 PNG) not yet decoded
        self.frame = None             # newest decoded frame: what the page looks like now
        self.in_flight = collections.deque()   # (sent at, touch time or None) of updates not yet shown
        self.acked = asyncio.Event()
        self.show_times = []                   # send -> shown round trips, for the stats
        self.strips = self.flipped = False     # from the Tab5's hello
        self.stats_task = None
        self.live, self.host = False, None   # live: the Tab5 is showing our updates
        self.touches, self.touch_wake = collections.deque(), asyncio.Event()   # touches waiting for the page

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

    def queue_touch(self, kind, x, y):
        """Touches reach the page in order but never hold up reading the Tab5's acks: dispatching one waits for
        the page, and a busy page took longer per move than the Tab5 sends them (60/s), so acks queued behind a
        growing backlog. While the page is busy, waiting moves collapse into the newest."""
        if kind == EV_MOVE and self.touches and self.touches[-1][0] == EV_MOVE:
            self.touches[-1] = (kind, x, y)
        else:
            self.touches.append((kind, x, y))
        self.touch_wake.set()

    async def dispatch_touches(self):
        while True:
            await self.touch_wake.wait()
            self.touch_wake.clear()
            while self.touches:
                await self.touch(*self.touches.popleft())

    async def read_events(self, reader):
        while True:
            kind, flags, x, y, _ = EVENT.unpack(await reader.readexactly(EVENT.size))
            if kind == EV_SHOWN:
                if not self.live:
                    self.live = True
                    sound = ("with sound" if self.audio.target else "no sound: the Tab5 firmware is older than 1.1") \
                        if self.audio else "no sound"
                    print(f"showing on Tab5 {self.host}:{PORT} ({sound})")
                if self.in_flight:
                    sent_at, touch_t = self.in_flight.popleft()
                    now = time.monotonic()
                    self.show_times.append(now - sent_at)
                    if touch_t is not None:
                        print(f"touch->shown {1000 * (now - touch_t):4.0f} ms")
                self.acked.set()
            elif kind in (EV_DOWN, EV_MOVE, EV_UP):
                self.queue_touch(kind, x, y)
            elif kind == EV_HELLO:   # firmware 1.1 or later: it takes settings and can play sound
                self.writer.write(RECT.pack(b"TCFG", POWER_SAVE[self.args.power_save], 0, 0, 0, 0))
                if self.audio and flags & HELLO_SOUND:
                    self.audio.target = (self.host, PORT)
                self.strips, self.flipped = bool(flags & HELLO_STRIPS), bool(flags & HELLO_FLIPPED)

    async def session(self, host):
        self.host = host
        reader, writer = await asyncio.open_connection(host, PORT)
        self.writer = writer
        self.in_flight.clear()
        self.touches.clear()
        self.strips = self.flipped = False
        events = asyncio.create_task(self.read_events(reader))
        toucher = asyncio.create_task(self.dispatch_touches())
        prev, last_start, last_sent, needs_settle = None, 0.0, 0.0, False
        soft = np.zeros((H // TILE, W // TILE), bool)   # tiles last sent in half-resolution colour
        changed_at = np.zeros((H // TILE, W // TILE))      # when each tile last changed
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
                # capture and encode while earlier updates may still be on their way to the panel (pipelining);
                # prev already includes them, so the diff is against what the Tab5 is about to show
                t0 = last_start = time.monotonic()
                touch_t = self.touch_at if self.touch_at is not None and self.touch_at < t0 else None
                moving = not settle and t0 - last_sent < MOTION_S
                cur = await self.capture()
                t1 = time.monotonic()
                q = min(95, self.args.quality + 5) if settle else self.args.quality

                def diff_and_encode():   # in a worker thread, so touches, acks and sound keep flowing meanwhile
                    tiles = changed_tiles(prev, cur, exact=settle)
                    changed_at[tiles] = t0
                    still = soft & ~tiles & (t0 - changed_at >= SETTLE_S)   # blurry and no longer moving: sharpen
                    pieces = []   # (piece, subsampling)
                    for piece in plan(tiles | soft if settle else tiles, self.strips):
                        _, x, y, w, h = piece
                        share = tiles[y // TILE:(y + h) // TILE, x // TILE:(x + w) // TILE].mean()
                        soft_ok = moving and share >= SOFT_SHARE and min(w, h) >= SOFT_MIN
                        pieces.append((piece, 2 if soft_ok else 0))
                    if not settle:
                        pieces += [(piece, 0) for piece in plan(still, self.strips)]
                    out = bytearray()
                    for piece, sub in pieces:
                        jpg = encode(piece_pixels(cur, piece, self.flipped), q, sub)
                        out += RECT.pack(b"TABS" if piece[0] == "strip" else b"TAB5", *piece[1:], len(jpg)) + jpg
                    return pieces, out
                pieces, out = await asyncio.to_thread(diff_and_encode)
                t2 = time.monotonic()
                if pieces:
                    out += RECT.pack(b"TAB5", 0, 0, 0, 0, 0)
                    # at most one update in flight, or two during an animation (not right after a touch)
                    quiet = self.touch_at is None or t0 - self.touch_at > TOUCH_QUIET_S
                    limit = 2 if moving and quiet else 1
                    while len(self.in_flight) >= limit:
                        self.acked.clear()
                        await asyncio.wait_for(self.acked.wait(), 5.0)
                    if touch_t is not None:
                        self.touch_at = None
                    self.in_flight.append((time.monotonic(), touch_t))
                    writer.write(out)
                    await asyncio.wait_for(writer.drain(), 5.0)
                    last_sent = t0
                    # remember only what the Tab5 now shows, so sub-threshold drift still adds up and gets sent
                    if prev is None:
                        prev = cur.copy()
                    for (_, x, y, w, h), sub in pieces:   # in order: a later piece may redraw part of an earlier one
                        prev[y:y + h, x:x + w] = cur[y:y + h, x:x + w]
                        soft[y // TILE:(y + h) // TILE, x // TILE:(x + w) // TILE] = sub != 0
                    needs_settle = not settle
                    stats.append((len(out), sum(w * h for (*_, w, h), _ in pieces), t1 - t0, t2 - t1))
                    if self.args.verbose:
                        n_strips = sum(p[0] == "strip" for p, _ in pieces)
                        n_soft = sum(sub != 0 for _, sub in pieces)
                        print(f"{len(pieces) - n_strips} rect(s) {n_strips} strip(s), {n_soft} in 4:2:0 "
                              f"{len(out) / 1024:6.1f} KB | decode {1000 * (t1 - t0):3.0f} ms "
                              f"diff+encode {1000 * (t2 - t1):3.0f} ms in flight {len(self.in_flight)}")
                elif touch_t is not None and t0 - touch_t > 1.0:
                    self.touch_at = None   # the touch changed nothing visible
                if (now := time.monotonic()) - stats_t >= 1.0:
                    await self.report(stats, now - stats_t)
                    stats, stats_t = [], now
        finally:
            events.cancel()
            toucher.cancel()
            writer.close()
            if self.audio:
                self.audio.target = None

    async def report(self, stats, secs):
        """Once a second: print a summary of continuous activity and hand it to the page (window.tab5Stats)."""
        n = len(stats)
        s = {"fps": n / secs, "kbps": sum(b for b, *_ in stats) / 1024 / secs,
             "area": 100 * sum(a for _, a, *_ in stats) / max(n, 1) / (W * H),
             "capture": 1000 * sum(c for *_, c, _ in stats) / max(n, 1), "encode": 1000 * sum(e for *_, e in stats) / max(n, 1)}
        show = 1000 * sum(self.show_times) / max(len(self.show_times), 1)
        self.show_times = []
        if n > 2:
            print(f"{s['fps']:4.1f} fps {s['kbps']:6.0f} KB/s | avg area {s['area']:4.1f}% decode {s['capture']:3.0f} ms "
                  f"diff+encode {s['encode']:3.0f} ms send->shown {show:3.0f} ms")
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
    ap.add_argument("--no-audio", action="store_true", help="don't send the page's sound to the Tab5")
    ap.add_argument("--power-save", choices=list(POWER_SAVE), default="auto",
                    help="Tab5 WiFi power save. auto (default): off only while you touch, the page animates or sound "
                         "plays; off: always off, for the quickest response but shorter battery life")
    ap.add_argument("--quality", type=int, default=88, help="JPEG quality (default 88)")
    ap.add_argument("--max-fps", type=float, default=60, help="cap on updates per second (default 60)")
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

    audio = None if args.no_audio else SystemAudio()
    if audio and not audio.open():
        audio = None
    try:
        await run(args, url, watch, settings, host, audio)
    finally:
        if audio:
            audio.close()


async def run(args, url, watch, settings, host, audio):
    # Playwright mutes Chromium by default; with sound it plays into our sink instead of the PC's speakers
    sound = dict(env={**os.environ, "PULSE_SINK": audio.sink}, ignore_default_args=["--mute-audio"],
                 args=["--autoplay-policy=no-user-gesture-required"]) if audio else {}
    async with async_playwright() as pw:
        # a persistent profile, so the apps' localStorage (notes, clock options) survives restarts
        try:
            ctx = await pw.chromium.launch_persistent_context(
                args.profile, executable_path=args.browser, headless=not args.show,
                viewport={"width": W, "height": H}, device_scale_factor=1, has_touch=True, **sound)
        except Exception as e:
            if "ProcessSingleton" in str(e):   # Chromium allows one instance per profile
                raise SystemExit(f"The browser profile {args.profile} is in use: is another server.py running? "
                                 "Stop it, or pass --profile with another directory.") from None
            raise
        await ctx.add_init_script(f"window.TAB5 = {json.dumps(settings)};")
        page = ctx.pages[0] if ctx.pages else await ctx.new_page()
        try:
            await page.goto(url)
        except Exception as e:   # e.g. no network: Chromium shows its error page, and the user can still navigate
            print(f"could not load {url}: {str(e).splitlines()[0]}")
        cdp = await ctx.new_cdp_session(page)
        mirror = Mirror(page, cdp, args, audio)
        if audio:
            asyncio.create_task(audio.run())

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
    def on_term(*_):   # clean up (the audio sink) on kill as on Ctrl+C
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_term)
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
