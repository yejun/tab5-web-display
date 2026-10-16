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
    server.py --file clip.mp4          # a video, in a loop (with ffmpeg): tap to pause, drag sideways to seek
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
import itertools
import json
import os
import pathlib
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from playwright.async_api import async_playwright

__version__ = "1.3.0"

W, H, TILE, PORT = 1280, 720, 16, 8765
# Chromium's rasterisation jitters a few pixels by a few levels between frames. A tile counts as changed if some
# pixel moved by more than NOISE, or if the whole tile shifted on average by more than DRIFT (a slow fade or gradient).
NOISE, DRIFT = 6, 1.5
SETTLE_S = 0.5   # after this long without changes, send whatever still differs at all (exact cleanup pass)
# Drawing cost on the Tab5 in ns per pixel (measured; PSRAM bandwidth is the limit): a rectangle is decoded and
# then copied into the frame buffer by the PPA, a strip (full-height columns) is decoded straight into it.
RECT_NS, STRIP_NS = 75, 27
# Updates closer together than this are an animation, and two of them may be in flight. Its pieces go out below
# the top quality (--quality): mostly-moving ones rough (lower quality and half-resolution colour, 4:2:0, which the
# Tab5 draws ~40 % faster), the rest at MOTION_Q_MID in full colour (they may hold still text, such as the page
# around a playing video). Tiles sent below the top quality are refined, sent again at the top quality in full
# colour, once they have neither changed nor been resent for SETTLE_S: the oldest first, at most REFINE_TILES per
# update while something else still moves, all at once when the screen is still. So a page that has come to rest is
# always at top quality, even next to an endless animation.
# Measured: on the RGB565 panel, q95 leaves ~0.05 % of pixels visibly off on text-heavy pages, q88 ~0.6 %, q70 ~8 %;
# lower quality saves data (q70: ~20 % less than q88) but hardly any drawing time.
MOTION_S = 0.2
ROUGH_SHARE = 0.75    # a piece goes out rough only if this share of its tiles is moving
BIG_MOTION = 0.10     # moving parts at an adapted quality (from MOTION_Q_BIG, see ADAPT_*) when this share of the
MOTION_Q_BIG, MOTION_Q_SMALL = 70, 85   # screen changes, else MOTION_Q_SMALL, where artifacts on small UI elements show more
MOTION_Q_MID = 88
# Large motion (BIG_MOTION or more of the screen: a video, scrolling) adapts its quality to the link instead of a
# fixed MOTION_Q_BIG: each frame should fit the measured link rate / the frame rate the content has. Detailed footage
# measured 141 KB per frame at q70 and still 89 KB at q40 (which looks fine on the Tab5's 290 ppi panel); a bright,
# glittering sea in a page 450-490 KB at q70, which held YouTube to 12-17 fps before.
ADAPT_Q_MIN, ADAPT_Q_MAX = 35, 85
ADAPT_RATE0, ADAPT_RATE_MAX = 2.5e6, 6e6   # bytes/s: the first guess, and the most the estimate can grow to
REFINE_TILES = (H // TILE) * (W // TILE) // 10
SOFT_MIN = 48         # 4:2:0 only for pieces this big both ways: the Tab5's decoder times out on smaller 4:2:0 images
TOUCH_QUIET_S = 0.5   # ... but only one in flight this soon after a touch, where delay matters more than frame rate
RECT = struct.Struct("<4sHHHHI")   # "TAB5" (or "TABS", a strip), x, y, w, h, jpeg length
EVENT = struct.Struct("<BBHHH")    # type, flags, x, y, z
EV_DOWN, EV_MOVE, EV_UP, EV_SHOWN, EV_HELLO, EV_LANES = 1, 2, 3, 5, 6, 7
# Lanes (firmware 1.3): one TCP connection carries at most ~3 MB/s to the Tab5, because its 64 KB receive window is
# fixed; spread over two connections in chunks, ~4.4 MB/s on WiFi 6. See firmware/web_display/lanes.h (also for why
# not more than two).
LANE_HDR = struct.Struct("<II")    # chunk sequence number, length
LANE_CHUNK = 16384
LANE_OVERDUE_S = 0.15
LANE_OFFER, LANE_ACCEPTED, LANE_REFUSED = 0, 1, 2
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
# the full 0-255 range; encoding full range crushes dark greys to black. So the server converts to TV range itself.
# Three encoders; the fastest one installed is used (or --encoder). Measured on a full-screen video strip / a
# full-screen text page at q95:
#  - "turbo+numba" (PyTurboJPEG and Numba): a Numba kernel computes the TV-range planes straight from the piece, even a
#    strip's rotated view, and TurboJPEG encodes them: 4.0 / 5.7 ms. On video the server's CPU dropped from 48 % of
#    a core to 27 %.
#  - "numba" (Numba only): the same conversion into a YCbCr image, which Pillow encodes: 7.2 / 6.7 ms.
#  - "pillow": Pillow's full-range YCbCr, squeezed per channel by a lookup table: 13.3 / 8.2 ms.
# The Numba conversion is also the exact one: Pillow's double rounding shifts every channel by up to a level.
TV_RANGE = [round(16 + v * 219 / 255) for v in range(256)] + [round(128 + (v - 128) * 224 / 255) for v in range(256)] * 2
try:
    import turbojpeg
    _turbo = turbojpeg.TurboJPEG()
    _TJSAMP = {0: turbojpeg.TJSAMP_444, 2: turbojpeg.TJSAMP_420}
except Exception:   # not installed, or the libturbojpeg library is missing
    _turbo = None
try:
    import numba
except ImportError:
    numba = None

if numba:
    @numba.njit(cache=True, nogil=True)
    def tv_planes(px, sub):
        """TV-range BT.601 Y, Cb, Cr planes (Cb/Cr at half resolution with sub 2) of an RGB array, in one buffer.
        16.16 fixed point; the same numbers as Pillow's full-range conversion squeezed into 16-235 / 16-240."""
        h, w = px.shape[0], px.shape[1]
        cw, ch = (w // 2, h // 2) if sub == 2 else (w, h)
        out = np.empty(h * w + 2 * cw * ch, np.uint8)
        for i in range(h):
            for j in range(w):
                r, g, b = np.int32(px[i, j, 0]), np.int32(px[i, j, 1]), np.int32(px[i, j, 2])
                out[i * w + j] = (16829 * r + 33039 * g + 6416 * b + 1081344) >> 16
        cb0, cr0 = h * w, h * w + cw * ch
        if sub == 2:   # average 2x2 pixels, then convert
            for i in range(ch):
                for j in range(cw):
                    r = np.int32(0)
                    g = np.int32(0)
                    b = np.int32(0)
                    for di in range(2):
                        for dj in range(2):
                            r += px[2 * i + di, 2 * j + dj, 0]
                            g += px[2 * i + di, 2 * j + dj, 1]
                            b += px[2 * i + di, 2 * j + dj, 2]
                    out[cb0 + i * cw + j] = (-9714 * r - 19071 * g + 28784 * b + 33685504) >> 18
                    out[cr0 + i * cw + j] = (28784 * r - 24103 * g - 4681 * b + 33685504) >> 18
        else:
            for i in range(h):
                for j in range(w):
                    r, g, b = np.int32(px[i, j, 0]), np.int32(px[i, j, 1]), np.int32(px[i, j, 2])
                    out[cb0 + i * w + j] = (-9714 * r - 19071 * g + 28784 * b + 8421376) >> 16
                    out[cr0 + i * w + j] = (28784 * r - 24103 * g - 4681 * b + 8421376) >> 16
        return out

    @numba.njit(cache=True, nogil=True)
    def tv_ycbcr(px):
        """The TV-range YCbCr image of an RGB array, for Pillow to encode."""
        h, w = px.shape[0], px.shape[1]
        out = np.empty((h, w, 3), np.uint8)
        for i in range(h):
            for j in range(w):
                r, g, b = np.int32(px[i, j, 0]), np.int32(px[i, j, 1]), np.int32(px[i, j, 2])
                out[i, j, 0] = (16829 * r + 33039 * g + 6416 * b + 1081344) >> 16
                out[i, j, 1] = (-9714 * r - 19071 * g + 28784 * b + 8421376) >> 16
                out[i, j, 2] = (28784 * r - 24103 * g - 4681 * b + 8421376) >> 16
        return out

ENCODERS = ["pillow"] + (["numba"] if numba else []) + (["turbo+numba"] if _turbo and numba else [])
encoder = ENCODERS[-1]


def encode(pixels, quality, subsampling):
    if encoder == "turbo+numba":
        h, w = pixels.shape[:2]
        return _turbo.encode_from_yuv(tv_planes(pixels, subsampling), h, w, quality=quality,
                                      jpeg_subsample=_TJSAMP[subsampling])
    if encoder == "numba":
        ycc = Image.fromarray(tv_ycbcr(pixels), "YCbCr")
    else:
        ycc = Image.fromarray(np.ascontiguousarray(pixels)).convert("YCbCr").point(TV_RANGE)
    buf = io.BytesIO()
    ycc.save(buf, "JPEG", quality=quality, subsampling=subsampling)
    return buf.getvalue()


def select_encoder(name):
    """Use the named encoder ("auto": the fastest installed), and compile Numba's kernel now rather than on the
    first frame (about a second the first time, then cached next to this file)."""
    global encoder
    if name != "auto" and name not in ENCODERS:
        need = "PyTurboJPEG and Numba (pip install PyTurboJPEG numba; PyTurboJPEG also needs the libjpeg-turbo " \
               "library)" if name == "turbo+numba" else "Numba (pip install numba)"
        raise SystemExit(f"--encoder {name} needs {need}")
    encoder = ENCODERS[-1] if name == "auto" else name
    if "numba" in encoder:   # every array type a piece can be: contiguous or a view, writable or not (decoded frames)
        for writable in (True, False):
            a = np.zeros((32, 32, 3), np.uint8)
            a.setflags(write=writable)
            for view in (a, a[:16, :16], a[::-1].transpose(1, 0, 2)):
                encode(view, 90, 2)


def piece_pixels(img, piece, flipped):
    """The pixels to encode for a plan() piece. A strip is turned into the panel's portrait orientation, so the
    Tab5 can decode it straight into its frame buffer: logical (x, y) is panel (719 - y, x), or (y, 1279 - x)
    when the screen is flipped."""
    kind, x, y, w, h = piece
    if kind == "rect":
        return img[y:y + h, x:x + w]
    cols = img[:, x:x + w].transpose(1, 0, 2)
    return cols[::-1] if flipped else img[::-1, x:x + w].transpose(1, 0, 2)


def check_firmware(firmware):
    """Say so when the Tab5's firmware and this server are from different releases. They work together either way,
    but only with the features both have."""
    server = tuple(int(v) for v in __version__.split(".")[:2])
    if firmware is None or firmware[:2] < server:
        print(f"note: the Tab5's firmware is older than this server ({__version__}). It works, but without the newer "
              "features; update the firmware (M5Burner, or tools/build.sh upload, see the README).")
    elif firmware[:2] > server:
        print(f"note: the Tab5's firmware is newer than this server ({__version__}). It works, but without the newer "
              "features; update the server from https://github.com/yejun/tab5-web-display")


class AudioOut:
    """Sends 48 kHz stereo 16-bit PCM to the Tab5 as UDP datagrams of AUDIO_FRAMES frames each."""

    def __init__(self):
        self.target = None   # (ip, port) of a Tab5 that can play sound, or None
        self.pos = 0         # stream position of the next datagram
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)

    def send(self, pcm):
        if self.target:
            try:
                self.sock.sendto(AUDIO.pack(b"TAUD", self.pos, AUDIO_FRAMES, 0) + pcm, self.target)
            except OSError:
                pass   # a lost datagram is a 7.5 ms gap; the next one goes out regardless
        self.skip(AUDIO_FRAMES)

    def skip(self, frames):
        self.pos = (self.pos + frames) & 0xFFFFFFFF

    def restart(self):
        """Jump far enough that the Tab5 starts a new stream, dropping what it still has buffered (after a seek)."""
        self.skip(1 << 16)


class SystemAudio(AudioOut):
    """Chromium plays into a private PulseAudio/PipeWire sink (it honours PULSE_SINK); the sink's monitor is
    recorded with parec and sent to the Tab5 as UDP datagrams while there is sound."""

    def __init__(self):
        super().__init__()
        self.sink = f"{SINK_PREFIX}{os.getpid()}"
        self.module = None

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
        proc = await asyncio.create_subprocess_exec(
            "parec", "-d", f"{self.sink}.monitor", "--raw", "--format=s16le", f"--rate={AUDIO_RATE}", "--channels=2",
            "--latency-msec=10", stdout=asyncio.subprocess.PIPE)
        loud_until = 0.0
        try:
            while True:
                pcm = await proc.stdout.readexactly(len(silence))
                now = time.monotonic()
                if pcm != silence:
                    loud_until = now + AUDIO_TAIL_S
                if now < loud_until:
                    self.send(pcm)
                else:
                    self.skip(AUDIO_FRAMES)
        except asyncio.IncompleteReadError:
            print("sound: the recorder stopped")
        finally:
            if proc.returncode is None:
                proc.kill()


class Mirror:
    """One Chromium page mirrored to the Tab5: frames out, touches in."""

    def __init__(self, page, cdp, args, audio):
        self.page, self.cdp, self.args, self.audio = page, cdp, args, audio
        self.writer = None
        self.dirty = asyncio.Event()
        self.touch_at = None          # when the last touch event arrived, for the touch -> shown latency
        self.latest = None            # newest screencast frame (base64 PNG) not yet decoded
        self.decoding = None          # the task decoding screencast frames
        self.frame = None             # newest decoded frame: what the page looks like now
        self.in_flight = collections.deque()   # (sent at, touch time or None) of updates not yet shown
        self.acked = asyncio.Event()
        self.show_times = []                   # send -> shown round trips, for the stats
        self.strips = self.flipped = False     # from the Tab5's hello
        self.firmware = None                   # (major, minor, patch) from the hello; None: older than 1.1
        self.stats_task = None
        self.live, self.host = False, None   # live: the Tab5 is showing our updates
        self.touches, self.touch_wake = collections.deque(), asyncio.Event()   # touches waiting for the page
        self.lane_offer, self.lane_answer, self.lane_answered = None, None, asyncio.Event()
        self.lanes, self.lane_readers, self.seq = [], [], 0   # lane writers, and the next chunk's number
        self.lanes_broken = False      # a lane setup failed: don't try again in this run
        self.vq = float(MOTION_Q_BIG)  # quality of large motion, adapted to the link
        self.rate = ADAPT_RATE0        # estimated link rate, bytes/s
        self.frames_in, self.fps_in = 0, 30.0   # new frames from the source this second, and per second
        self.ack_wait = 0.0            # seconds spent waiting for "shown" this second (the link, or the Tab5, is full)

    @staticmethod
    def decode_png(data):
        img = Image.open(io.BytesIO(base64.b64decode(data))).convert("RGB")
        if img.size != (W, H):
            img = img.resize((W, H))
        return np.asarray(img)

    def frame_arrived(self, data):
        """A screencast frame (base64 PNG). It is decoded in the background, while the previous one is encoded and
        sent: a detailed frame takes 20-40 ms to decode, and decoded only when needed it held a video in a page to
        ~22 fps although Chromium painted 30. Frames that arrive during a decode collapse into the newest."""
        self.latest = data
        self.frames_in += 1
        if self.decoding is None or self.decoding.done():
            self.decoding = asyncio.create_task(self.decode_latest())

    async def decode_latest(self):
        while self.latest is not None:
            data, self.latest = self.latest, None
            self.frame = await asyncio.to_thread(self.decode_png, data)
            self.dirty.set()

    async def capture(self):
        """What the page looks like now: the newest decoded screencast frame. Chromium sends a lossless frame every
        time it paints, so nothing else is asked of it here. (Page.captureScreenshot while the screencast runs
        occasionally never returned, which froze the whole mirror.)"""
        for _ in range(100):   # nothing painted yet: wait for the first frame
            if self.frame is not None:
                return self.frame
            await asyncio.sleep(0.05)
        raise TimeoutError("no frame from Chromium")

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
            kind, flags, x, y, _z = EVENT.unpack(await reader.readexactly(EVENT.size))
            if kind == EV_SHOWN:
                if not self.live:
                    self.live = True
                    fw = "%d.%d.%d" % self.firmware if self.firmware else "older than 1.1"
                    sound = "with sound" if self.audio and self.audio.target else "no sound"
                    print(f"showing on Tab5 {self.host}:{PORT} (firmware {fw}, {sound})")
                    check_firmware(self.firmware)
                if self.in_flight:
                    sent_at, touch_t = self.in_flight.popleft()
                    now = time.monotonic()
                    self.show_times.append(now - sent_at)
                    if touch_t is not None:
                        print(f"touch->shown {1000 * (now - touch_t):4.0f} ms")
                self.acked.set()
            elif kind in (EV_DOWN, EV_MOVE, EV_UP):
                self.queue_touch(kind, x, y)
            elif kind == EV_LANES:
                if flags == LANE_OFFER:
                    self.lane_offer = (x, y, _z)   # port, token, most lanes
                else:
                    self.lane_answer = flags
                    self.lane_answered.set()
            elif kind == EV_HELLO:   # firmware 1.1 or later: it takes settings and can play sound
                self.writer.write(RECT.pack(b"TCFG", POWER_SAVE[self.args.power_save], 0, 0, 0, 0))
                if self.audio and flags & HELLO_SOUND:
                    self.audio.target = (self.host, PORT)
                self.strips, self.flipped = bool(flags & HELLO_STRIPS), bool(flags & HELLO_FLIPPED)
                self.firmware = (x, y, _z)

    async def session(self, host):
        self.host = host
        reader, writer = await asyncio.open_connection(host, PORT)
        self.writer = writer
        self.connected(True)
        self.in_flight.clear()
        self.touches.clear()
        self.strips = self.flipped = False
        self.firmware = None
        self.lane_offer, self.lanes, self.seq = None, [], 0
        events = asyncio.create_task(self.read_events(reader))
        toucher = asyncio.create_task(self.dispatch_touches())
        prev, last_start, last_sent, needs_settle = None, 0.0, 0.0, False
        rough = np.zeros((H // TILE, W // TILE), bool)  # tiles last sent below the top quality
        rough_at = np.zeros((H // TILE, W // TILE))        # when each was last sent below the top quality
        changed_at = np.zeros((H // TILE, W // TILE))      # when each tile last changed
        stats, stats_t = [], time.monotonic()
        try:
            while not events.done():
                if self.lane_offer and not self.lanes and self.args.lanes > 1 and not self.lanes_broken:
                    await self.open_lanes(host)
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
                top = self.args.quality

                work = [0.0, 0.0]   # when the worker started and finished, for -v

                changed = [0.0]

                def diff_and_encode():   # in a worker thread, so touches, acks and sound keep flowing meanwhile
                    work[0] = time.monotonic()
                    tiles = changed_tiles(prev, cur, exact=settle)
                    changed[0] = tiles.mean()
                    changed_at[tiles] = t0
                    motion_q = self.motion_quality(tiles.mean())
                    pieces = []   # (piece, quality, subsampling)
                    for piece in plan(tiles | rough if settle else tiles, self.strips):
                        _, x, y, w, h = piece
                        share = tiles[y // TILE:(y + h) // TILE, x // TILE:(x + w) // TILE].mean()
                        if not moving:
                            pieces.append((piece, top, 0))
                        elif share >= ROUGH_SHARE:
                            pieces.append((piece, motion_q, 2 if min(w, h) >= SOFT_MIN else 0))
                        else:
                            pieces.append((piece, min(top, MOTION_Q_MID), 0))
                    if not settle:   # refine tiles that have stood still and not been resent, the oldest first
                        still = rough & ~tiles & (t0 - changed_at >= SETTLE_S) & (t0 - rough_at >= SETTLE_S)
                        if still.sum() > REFINE_TILES:
                            ys, xs = np.nonzero(still)
                            oldest = np.argsort(changed_at[ys, xs], kind="stable")[:REFINE_TILES]
                            still = np.zeros_like(still)
                            still[ys[oldest], xs[oldest]] = True
                        pieces += [(piece, top, 0) for piece in plan(still, self.strips)]
                    out = bytearray()
                    for piece, q, sub in pieces:
                        jpg = encode(piece_pixels(cur, piece, self.flipped), q, sub)
                        out += RECT.pack(b"TABS" if piece[0] == "strip" else b"TAB5", *piece[1:], len(jpg)) + jpg
                    work[1] = time.monotonic()
                    return pieces, out
                pieces, out = await asyncio.to_thread(diff_and_encode)
                t2 = time.monotonic()
                if moving and pieces:
                    self.encoded(len(out), changed[0])
                if pieces:
                    out += RECT.pack(b"TAB5", 0, 0, 0, 0, 0)
                    # at most one update in flight, or two during an animation (not right after a touch)
                    quiet = self.touch_at is None or t0 - self.touch_at > TOUCH_QUIET_S
                    limit = 2 if moving and quiet else 1
                    t_wait = time.monotonic()
                    while len(self.in_flight) >= limit:
                        self.acked.clear()
                        if not self.lanes or len(self.in_flight) > limit:
                            await asyncio.wait_for(self.acked.wait(), 5.0)
                            continue
                        # On lanes, a packet lost at the end of a lane's burst has nothing after it to show the gap,
                        # and TCP waits for its retransmission timer while the Tab5 waits for the chunk (measured:
                        # stalls of over a second a few times in four minutes). If "shown" is overdue, one more
                        # update goes out: its data reveals the gap, and the lost packet is resent at once.
                        try:
                            await asyncio.wait_for(self.acked.wait(), LANE_OVERDUE_S)
                        except TIMEOUTS:
                            break
                    if touch_t is not None:
                        self.touch_at = None
                    self.in_flight.append((time.monotonic(), touch_t))
                    t_send = time.monotonic()
                    self.ack_wait += t_send - t_wait
                    await self.send(out)
                    t_sent = time.monotonic()
                    last_sent = t0
                    # remember only what the Tab5 now shows, so sub-threshold drift still adds up and gets sent
                    if prev is None:
                        prev = cur.copy()
                    for (_, x, y, w, h), q, sub in pieces:   # in order: a later piece may redraw part of an earlier one
                        prev[y:y + h, x:x + w] = cur[y:y + h, x:x + w]
                        area = np.s_[y // TILE:(y + h) // TILE, x // TILE:(x + w) // TILE]
                        rough[area] = q < top or sub != 0
                        if rough[area].any():
                            rough_at[area] = t0
                    needs_settle = not settle or rough.any()
                    stats.append((len(out), sum(w * h for (*_, w, h), _, _ in pieces), t1 - t0, t2 - t1))
                    if self.args.verbose:
                        n_strips = sum(p[0] == "strip" for p, _, _ in pieces)
                        n_rough = sum(q < top or sub != 0 for _, q, sub in pieces)
                        print(f"{len(pieces) - n_strips} rect(s) {n_strips} strip(s), {n_rough} rough, "
                              f"{rough.sum()} rough tiles left "
                              f"{len(out) / 1024:6.1f} KB | decode {1000 * (t1 - t0):3.0f} ms "
                              f"diff+encode {1000 * (t2 - t1):3.0f} ms (start {1000 * (work[0] - t1):.1f} work {1000 * (work[1] - work[0]):.1f} resume {1000 * (t2 - work[1]):.1f}) in flight {len(self.in_flight)} | ack wait "
                              f"{1000 * (t_send - t_wait):4.0f} ms send {1000 * (t_sent - t_send):4.0f} ms")
                elif touch_t is not None and t0 - touch_t > 1.0:
                    self.touch_at = None   # the touch changed nothing visible
                if (now := time.monotonic()) - stats_t >= 1.0:
                    await self.report(stats, now - stats_t)
                    stats, stats_t = [], now
        finally:
            events.cancel()
            toucher.cancel()
            writer.close()
            for w in self.lanes:
                w.close()
            self.lanes = []
            self.connected(False)
            if self.audio:
                self.audio.target = None

    async def open_lanes(self, host):
        """Spread the stream over several connections, if the Tab5 offered it (firmware 1.3). If it refuses, the
        stream stays on the main connection; if it doesn't answer, the session restarts without lanes."""
        port, token, most = self.lane_offer
        n = min(self.args.lanes, most)
        self.lane_offer = None
        if n < 2:
            return
        conns = []
        try:
            for i in range(n):
                r, w = await asyncio.wait_for(asyncio.open_connection(host, port), 3)
                w.get_extra_info("socket").setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                w.write(RECT.pack(b"TLAN", i, token, n, 0, 0))
                conns.append((r, w))
            self.lane_answered.clear()
            self.writer.write(RECT.pack(b"TSTR", n, token, 0, 0, 0))
            await asyncio.wait_for(self.lane_answered.wait(), 5)
        except (OSError, *TIMEOUTS) as e:
            for _, w in conns:
                w.close()
            self.lanes_broken = True
            raise ConnectionError(f"lane setup failed ({e or type(e).__name__}); continuing with one connection") from None
        if self.lane_answer != LANE_ACCEPTED:
            for _, w in conns:
                w.close()
            print("the Tab5 refused lanes; continuing with one connection")
            return
        self.lanes, self.lane_readers, self.seq = [w for _, w in conns], [r for r, _ in conns], 0
        if self.args.verbose:
            print(f"stream on {n} lanes")

    async def send(self, data):
        """An update to the Tab5: on the main connection, or cut into chunks dealt round the lanes."""
        if not self.lanes:
            self.writer.write(data)
            await asyncio.wait_for(self.writer.drain(), 5.0)
            return
        view = memoryview(data)
        for i in range(0, len(data), LANE_CHUNK):
            part = view[i:i + LANE_CHUNK]
            lane = self.lanes[self.seq % len(self.lanes)]
            lane.write(LANE_HDR.pack(self.seq & 0xFFFFFFFF, len(part)))
            lane.write(part)
            self.seq += 1
        await asyncio.wait_for(asyncio.gather(*(w.drain() for w in self.lanes)), 5.0)

    def connected(self, up):
        """Called when a session with the Tab5 starts and ends."""

    def motion_quality(self, changed):
        """JPEG quality for the moving parts of an update in which this share of the screen changed."""
        return min(self.args.quality, round(self.vq) if changed >= BIG_MOTION else MOTION_Q_SMALL)

    def encoded(self, size, changed):
        """After each update sent during motion: for large motion, step towards the quality whose frames fit the
        budget (JPEG size roughly doubles from q40 to q80). Small steps, down faster than up: a page's frames vary a
        lot from one to the next (a repeated video frame, a scene cut), and big steps made the quality swing between
        its limits."""
        if changed >= BIG_MOTION:
            budget = self.rate / max(self.fps_in, 1.0)
            step = min(2.0, max(-4.0, 4 * np.log2(budget / size)))
            self.vq = min(ADAPT_Q_MAX, max(ADAPT_Q_MIN, self.vq + step))

    def adapting(self):
        """Whether this second's frame rate says something about the link (not while a video is paused)."""
        return True

    def adapt(self, stats, secs):
        """Once a second: the frame rate the content has, and the link rate. What got through while frames were
        missed for want of the link (the server spent a good part of the second waiting for "shown") is the link
        rate; otherwise there may be more, so the estimate creeps up. Frames missed with no waiting were the PC's
        doing (decoding Chromium's frames), and a lower quality would not bring them back. Only seconds of mostly
        large motion count: a page's small animations say nothing about the link."""
        self.fps_in = min(self.args.max_fps, self.frames_in / secs) if self.frames_in else self.fps_in
        self.frames_in = 0
        waited, self.ack_wait = self.ack_wait / secs, 0.0
        big = sum(a >= BIG_MOTION * W * H for _, a, *_ in stats)
        if not self.adapting() or big < 3 or big < 0.8 * len(stats):
            return
        fps, rate = len(stats) / secs, sum(b for b, *_ in stats) / secs
        if fps < 0.9 * self.fps_in and waited > 0.25:
            self.rate = 0.6 * self.rate + 0.4 * rate
        else:
            self.rate = min(self.rate * 1.05, ADAPT_RATE_MAX)
        if self.args.verbose:
            print(f"large motion: quality {self.vq:.0f}, link estimate {self.rate / 1e6:.2f} MB/s, "
                  f"content {self.fps_in:.0f} fps, waited for the Tab5 {100 * waited:.0f}% of the time")

    async def report(self, stats, secs):
        """Once a second: print a summary of continuous activity and hand it to the page (window.tab5Stats)."""
        self.adapt(stats, secs)
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


# ---- local video files ----------------------------------------------------------------------------------------
# ffmpeg decodes the picture and the sound in two processes; the server keeps the clock (pause, seek, loop) and
# hands each frame to the same pipeline as a page's, so only what changed is sent and the paused picture is
# refined to the top quality. Sound goes to the Tab5 as from a page, without PulseAudio.
VIDEO_FPS = 30          # at most this many frames per second: the Tab5 draws full-screen changes at about 30 fps
BAR_H = 120             # the position bar draws into this much of the bottom of the screen (with its shade)
BAR_X0, BAR_X1 = 32, W - 32
BAR_SHOW_S, BAR_FADE_S = 3.0, 0.4   # shown this long after a touch while playing (always while paused or seeking)
DRAG_PX = 24            # a touch that moves this far sideways seeks, otherwise it is a tap (pause / resume)
SEEK_SPAN_S = 120       # dragging across the whole screen seeks this far (through the whole video if shorter);
                        # dragging on the bar itself puts the position under the finger
PREVIEW_S = 0.15        # while seeking, the frame at the drag position is shown at most this often
ACCENT = (90, 226, 199)  # as in the Tab5's settings
PAGE_SUFFIXES = {".html", ".htm", ".xhtml", ".svg"}


def probe_video(path):
    """(duration in seconds or None, frame rate, has sound) if ffmpeg can play the file as a video, else None."""
    try:
        out = subprocess.run(["ffprobe", "-v", "error", "-of", "json", "-show_entries",
                              "format=duration,format_name:stream=codec_type,avg_frame_rate,r_frame_rate:"
                              "stream_disposition=attached_pic", str(path)],
                             capture_output=True, text=True, timeout=15)
        info = json.loads(out.stdout or "{}")
    except (OSError, subprocess.SubprocessError, ValueError):
        return None
    fmt, streams = info.get("format", {}), info.get("streams", [])
    if "image2" in fmt.get("format_name", "") or fmt.get("format_name", "").endswith("_pipe"):
        return None   # a still image
    video = [s for s in streams if s.get("codec_type") == "video" and not s.get("disposition", {}).get("attached_pic")]
    if not video:
        return None
    rate = VIDEO_FPS
    for key in ("avg_frame_rate", "r_frame_rate"):
        num, _, den = video[0].get(key, "0/0").partition("/")
        try:
            if float(den or 1) > 0 and float(num) / float(den or 1) > 0:
                rate = float(num) / float(den or 1)
                break
        except ValueError:
            pass
    try:
        duration = float(fmt["duration"])
    except (KeyError, ValueError):
        duration = None
    return duration, rate, any(s.get("codec_type") == "audio" for s in streams)


def read_full(f, n):
    """n bytes from a pipe, or None at its end."""
    buf = bytearray(n)
    view, got = memoryview(buf), 0
    while got < n:
        r = f.readinto(view[got:])
        if not r:
            return None
        got += r
    return buf


def clock_text(s, hours=False):
    s = int(s)
    return f"{s // 3600}:{s // 60 % 60:02}:{s % 60:02}" if hours or s >= 3600 else f"{s // 60}:{s % 60:02}"


class Video:
    """A video file played by ffmpeg: a clock that pauses, seeks and loops, and decoder threads that hand out each
    frame and sound datagram when the clock reaches it. Frames are scaled to fit the screen, letterboxed."""

    def __init__(self, path, duration, fps, has_sound, audio, on_frame):
        self.path, self.duration, self.audio, self.on_frame = path, duration, audio if has_sound else None, on_frame
        self.fps = min(fps, VIDEO_FPS)
        self.loop = asyncio.get_running_loop()
        self.gen = 0                  # bumped by every (re)start; older decoder threads then quit
        self.procs = []
        self.halts = {"offline"}      # the clock stands still while any: "pause", "seek", "offline" (no Tab5)
        self.clock = (0.0, time.monotonic(), False)   # (position, at monotonic time, running): one atomic value
        self.skipped = 0              # frames dropped because decoding fell behind

    @property
    def paused(self):
        return "pause" in self.halts

    def position(self):
        pos, t, running = self.clock
        return pos + (time.monotonic() - t if running else 0.0)

    def halt(self, why, on):
        pos = self.position()
        (self.halts.add if on else self.halts.discard)(why)
        self.clock = (pos, time.monotonic(), not self.halts)

    def filters(self):
        return (f"fps={self.fps:.3f},scale={W}:{H}:force_original_aspect_ratio=decrease,"
                f"pad={W}:{H}:(ow-iw)/2:(oh-ih)/2")

    def start(self, pos):
        """(Re)start decoding at pos seconds; the clock keeps running or standing still as before."""
        self.stop()
        self.gen += 1
        self.clock = (pos, time.monotonic(), not self.halts)
        cmd = ["ffmpeg", "-v", "error", "-nostdin", "-ss", f"{pos:.3f}", "-i", str(self.path), "-sn"]
        v = subprocess.Popen(cmd + ["-an", "-vf", self.filters(), "-pix_fmt", "rgb24", "-f", "rawvideo", "-"],
                             stdout=subprocess.PIPE)
        self.procs = [v]
        threading.Thread(target=self.play_video, args=(self.gen, v, pos), daemon=True).start()
        if self.audio:
            a = subprocess.Popen(cmd + ["-vn", "-f", "s16le", "-ar", str(AUDIO_RATE), "-ac", "2", "-"],
                                 stdout=subprocess.PIPE)
            self.procs.append(a)
            threading.Thread(target=self.play_sound, args=(self.gen, a, pos), daemon=True).start()

    def stop(self):
        for p in self.procs:
            p.kill()
        self.procs = []

    def wait_for(self, gen, pts):
        """Sleep until the clock reaches pts; False if this decoder was replaced meanwhile."""
        while gen == self.gen:
            ahead = pts - self.position()
            if ahead <= 0:
                return True
            time.sleep(min(ahead, 0.02))   # while the clock stands still this just keeps checking
        return False

    def play_video(self, gen, proc, start):
        try:
            for i in itertools.count():
                frame = read_full(proc.stdout, W * H * 3)
                if gen != self.gen:
                    return
                if frame is None:   # the end: play it again from the start
                    self.loop.call_soon_threadsafe(self.ended, gen, start, i == 0)
                    return
                pts = start + i / self.fps
                # the first frame shows at once, so a seek while paused shows where it landed
                if i and not self.wait_for(gen, pts):
                    return
                if i and self.position() - pts > 1 / self.fps:
                    self.skipped += 1
                    continue
                self.loop.call_soon_threadsafe(self.on_frame, gen, np.frombuffer(frame, np.uint8).reshape(H, W, 3))
        finally:
            proc.stdout.close()
            proc.wait()

    def play_sound(self, gen, proc, start):
        n, last_sent = AUDIO_FRAMES * 4, 0.0
        self.audio.restart()
        try:
            for k in itertools.count():
                pcm = read_full(proc.stdout, n)
                pts = start + k * AUDIO_FRAMES / AUDIO_RATE
                if pcm is None or not self.wait_for(gen, pts):
                    return
                now = time.monotonic()
                if self.position() - pts > 0.1:   # late, after the clock jumped: leave a gap
                    self.audio.skip(AUDIO_FRAMES)
                    continue
                if now - last_sent > 0.2:   # resumed after a pause: a new stream, not a hold-up to buffer more for
                    self.audio.restart()
                self.audio.send(bytes(pcm))
                last_sent = now
        finally:
            proc.stdout.close()
            proc.wait()

    def ended(self, gen, start, empty):
        if gen != self.gen:
            return
        if empty and start == 0:
            print(f"ffmpeg could not decode {self.path.name}")
            self.gen += 1   # and stay on the last picture
            return
        self.start(0.0)

    def grab(self, pos):
        """The single frame at pos (for the preview while seeking), or None."""
        try:
            out = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-ss", f"{pos:.3f}", "-i", str(self.path),
                                  "-an", "-sn", "-frames:v", "1", "-vf", self.filters(), "-pix_fmt", "rgb24",
                                  "-f", "rawvideo", "-"], capture_output=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            return None
        if len(out.stdout) != W * H * 3:
            return None
        return np.frombuffer(out.stdout, np.uint8).reshape(H, W, 3)


class VideoMirror(Mirror):
    """A video file mirrored to the Tab5: tap to pause and resume, drag sideways to seek. A bar at the bottom shows
    the position while paused or seeking and for a few seconds after a touch."""

    def __init__(self, args, audio, path, info):
        super().__init__(None, None, args, audio)
        self.name = path.name
        self.video = Video(path, *info, audio, self.new_frame)
        self.still = None            # the preview frame while seeking
        self.down = None             # (x, y, time, position) where the current touch started
        self.seeking, self.on_bar, self.target = False, False, 0.0
        self.bar_until = time.monotonic() + BAR_SHOW_S
        self.previewer = None
        self.font = ImageFont.load_default(size=26)
        self.small = ImageFont.load_default(size=21)

    def new_frame(self, gen, frame):
        if gen == self.video.gen:
            self.frame, self.still = frame, None
            self.frames_in += 1
            self.dirty.set()

    def connected(self, up):
        self.video.halt("offline", not up)

    async def push_stats(self, s):
        pass   # no page to show them

    def adapting(self):
        return not self.video.halts

    async def report(self, stats, secs):
        await super().report(stats, secs)
        if self.video.skipped and self.args.verbose:
            print(f"video: {self.video.skipped} frame(s) skipped, decoding fell behind")
        self.video.skipped = 0

    async def capture(self):
        while self.frame is None:
            await asyncio.sleep(0.02)
        base = self.still if self.still is not None else self.frame
        alpha = self.bar_alpha()
        return await asyncio.to_thread(self.compose, base, alpha) if alpha > 0 else base

    def bar_alpha(self):
        if self.video.paused or self.seeking or not self.live:
            return 1.0
        return min(1.0, max(0.0, (self.bar_until - time.monotonic()) / BAR_FADE_S))

    def bar_visible(self):
        return self.bar_alpha() > 0

    def compose(self, base, alpha):
        """The frame with the position bar drawn over its bottom: a shade, the state and time, the file name, and
        the progress line."""
        v, img = self.video, base.copy()
        region = img[H - BAR_H:].astype(np.float32)
        shade = np.linspace(0, 0.65, BAR_H, dtype=np.float32) ** 1.3 * alpha
        region *= 1 - shade[:, None, None]
        layer = Image.new("RGBA", (W, BAR_H), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        pos = self.target if self.seeking else v.position()
        dur = v.duration
        hours = bool(dur and dur >= 3600)
        ty = BAR_H - 66   # text baseline
        if v.paused:                        # two bars: paused
            d.rounded_rectangle((BAR_X0, ty - 20, BAR_X0 + 6, ty), 2, fill=(255, 255, 255, 255))
            d.rounded_rectangle((BAR_X0 + 12, ty - 20, BAR_X0 + 18, ty), 2, fill=(255, 255, 255, 255))
        else:                               # a triangle: playing
            d.polygon([(BAR_X0, ty - 21), (BAR_X0, ty + 1), (BAR_X0 + 18, ty - 10)], fill=(255, 255, 255, 255))
        text = clock_text(pos, hours) + (f" / {clock_text(dur, hours)}" if dur else "")
        d.text((BAR_X0 + 34, ty), text, font=self.font, fill=(255, 255, 255, 255), anchor="ls")
        x = BAR_X0 + 34 + d.textlength(text, font=self.font)
        if self.seeking:
            delta = self.target - self.down[3]
            d.text((x + 16, ty), ("+" if delta >= 0 else "−") + clock_text(abs(delta)), font=self.small,
                   fill=ACCENT + (255,), anchor="ls")
        elif v.paused:
            d.text((x + 16, ty), "Paused", font=self.small, fill=(255, 255, 255, 170), anchor="ls")
        name = self.name
        while len(name) > 4 and d.textlength(name, font=self.small) > W / 2:
            name = name[:-2].rstrip() + "…" if not name.endswith("…") else name[:-2] + "…"
        d.text((BAR_X1, ty), name, font=self.small, fill=(255, 255, 255, 170), anchor="rs")
        if dur:
            ly, thick = BAR_H - 32, 6 if self.seeking else 4
            px = BAR_X0 + (BAR_X1 - BAR_X0) * min(1.0, max(0.0, pos / dur))
            d.rounded_rectangle((BAR_X0, ly - thick // 2, BAR_X1, ly + thick // 2), thick // 2, fill=(255, 255, 255, 80))
            d.rounded_rectangle((BAR_X0, ly - thick // 2, px, ly + thick // 2), thick // 2, fill=ACCENT + (255,))
            r = 11 if self.seeking else 8
            d.ellipse((px - r, ly - r, px + r, ly + r), fill=(255, 255, 255, 255))
        rgba = np.asarray(layer).astype(np.float32)
        a = rgba[..., 3:] / 255 * alpha
        region = region * (1 - a) + rgba[..., :3] * a
        img[H - BAR_H:] = region.clip(0, 255).astype(np.uint8)
        return img

    def seek_to(self, x):
        dur, (x0, _, _, pos0) = self.video.duration, self.down
        if self.on_bar:
            t = (x - BAR_X0) / (BAR_X1 - BAR_X0) * dur
        else:
            t = pos0 + (x - x0) / W * min(dur, SEEK_SPAN_S)
        # not into the last half second: letting go at the end plays the end, then loops
        return min(max(t, 0.0), max(dur - 0.5, 0.0))

    async def preview(self):
        shown = None
        while self.seeking:
            if self.target != shown:
                shown = self.target
                frame = await asyncio.to_thread(self.video.grab, shown)
                if self.seeking and frame is not None:
                    self.still = frame
                    self.dirty.set()
            await asyncio.sleep(PREVIEW_S)

    async def touch(self, kind, x, y):
        now, v = time.monotonic(), self.video
        if kind == EV_DOWN:
            self.down = (x, y, now, v.position())
            self.on_bar = bool(v.duration) and self.bar_visible() and y >= H - BAR_H
        elif self.down is None:
            return
        elif kind == EV_MOVE:
            if not self.seeking and v.duration and abs(x - self.down[0]) >= DRAG_PX:
                self.seeking = True
                v.halt("seek", True)
                self.previewer = asyncio.create_task(self.preview())
            if self.seeking:
                self.target = self.seek_to(x)
        elif kind == EV_UP:
            if self.seeking:
                self.seeking = False
                v.halts.discard("seek")
                v.start(self.target)
            elif self.on_bar:   # a tap on the bar jumps there
                v.start(self.seek_to(x))
            else:
                v.halt("pause", not v.paused)
            self.down = None
        if self.args.verbose:
            print(f"touch {kind} {x},{y}: paused {v.paused} seeking {self.seeking} at {v.position():.1f} s")
        self.bar_until = now + BAR_SHOW_S
        self.touch_at = now
        self.dirty.set()


async def run_video(args, path, info, host):
    audio = None if args.no_audio else AudioOut()
    mirror = VideoMirror(args, audio, path, info)
    duration, fps, has_sound = info
    print(f"Tab5 Web Display {__version__}: playing {path.name} ("
          f"{clock_text(duration) if duration else 'unknown length'}, {min(fps, VIDEO_FPS):.3g} fps, "
          f"{'with' if has_sound and audio else 'no'} sound); tap the Tab5 to pause, drag sideways to seek")
    mirror.video.start(0.0)
    try:
        await serve(mirror, host)
    finally:
        mirror.video.stop()


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
    """Chromium, Chrome or Edge: on the PATH (Linux), or where their installers put them (Windows, macOS)."""
    for name in ("chromium", "chromium-browser", "google-chrome-stable", "google-chrome", "chrome",
                 "microsoft-edge-stable", "microsoft-edge", "msedge"):
        if path := shutil.which(name):
            return path
    places = []   # Chrome first, then Chromium, then Edge, each wherever it is installed
    if sys.platform == "win32":
        roots = [r for r in map(os.environ.get, ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA")) if r]
        for exe in ("Google/Chrome/Application/chrome.exe", "Chromium/Application/chrome.exe",
                    "Microsoft/Edge/Application/msedge.exe"):
            places += [pathlib.Path(root, exe) for root in roots]
    elif sys.platform == "darwin":
        for exe in ("Google Chrome.app/Contents/MacOS/Google Chrome", "Chromium.app/Contents/MacOS/Chromium",
                    "Microsoft Edge.app/Contents/MacOS/Microsoft Edge"):
            places += [root / exe for root in (pathlib.Path("/Applications"), pathlib.Path.home() / "Applications")]
    for path in places:
        if path.is_file():
            return str(path)
    return None   # Playwright's own Chromium (playwright install chromium)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    ap.add_argument("--device", help="Tab5 IP address (default: ask the Tab5 over USB)")
    ap.add_argument("--port", help="Tab5 USB serial port, used to find its IP (default: found automatically)")
    what = ap.add_mutually_exclusive_group()
    what.add_argument("--app", default="home", help=f"built-in app: {', '.join(APPS)} (default home)")
    what.add_argument("--url", help="any web page")
    what.add_argument("--file", help="a local HTML file (reloaded when it, or a file next to it, changes), or a video "
                                     "file, played in a loop (needs ffmpeg): tap to pause, drag sideways to seek")
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
    ap.add_argument("--quality", type=int, default=95,
                    help="JPEG quality once the page is still (default 95); moving parts are sent rougher first")
    ap.add_argument("--max-fps", type=float, default=60, help="cap on updates per second (default 60)")
    ap.add_argument("--encoder", choices=["auto", "pillow", "numba", "turbo+numba"], default="auto",
                    help="JPEG encoder (default auto: turbo+numba with PyTurboJPEG and Numba installed, numba with "
                         "Numba, else pillow)")
    ap.add_argument("--lanes", type=int, default=2, choices=[1, 2],
                    help="TCP connections to spread the stream over, with firmware 1.3 or later (default 2; 1 = one "
                         "connection, as before)")
    ap.add_argument("--browser", default=find_browser(), help="Chromium, Chrome or Edge executable (default: found on the PATH or in the usual install folders)")
    ap.add_argument("--show", action="store_true", help="show the browser window too (for debugging)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    video = None
    if args.url:
        url, watch = args.url, None
    elif args.file:
        watch = pathlib.Path(args.file).resolve()
        if not watch.is_file():
            raise SystemExit(f"no such file: {watch}")
        url = watch.as_uri()
        if watch.suffix.lower() not in PAGE_SUFFIXES:
            if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
                print("note: not a web page, and playing videos needs ffmpeg (with ffprobe) on the PATH; "
                      "opening it in the browser")
            elif info := probe_video(watch):
                video = info
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
    select_encoder(args.encoder)
    print(f"JPEG encoder: {encoder}" + ("" if encoder == "turbo+numba" else
          " (faster with PyTurboJPEG and Numba installed, see the README)"))
    if video:
        await run_video(args, watch, video, host)
        return

    audio = None if args.no_audio else SystemAudio()
    if audio and not audio.open():
        audio = None
    try:
        await run(args, url, watch, settings, host, audio)
    finally:
        if audio:
            audio.close()


async def launch(pw, args, audio):
    """The browser, with a persistent profile so the apps' localStorage (notes, clock options) survives restarts.

    It presents itself like a normal Chrome: some sites treat a browser that announces "HeadlessChrome" or
    navigator.webdriver as a bot (YouTube stops playback after a minute or so). The user agent is the browser's own
    with "Headless" removed, read on a first launch."""
    opts = dict(executable_path=args.browser, headless=not args.show, viewport={"width": W, "height": H},
                device_scale_factor=1, has_touch=True, args=["--disable-blink-features=AutomationControlled"])
    if audio:   # Playwright mutes Chromium by default; with sound it plays into our sink instead of the PC's speakers
        opts.update(env={**os.environ, "PULSE_SINK": audio.sink}, ignore_default_args=["--mute-audio"])
        opts["args"].append("--autoplay-policy=no-user-gesture-required")
    try:
        ctx = await pw.chromium.launch_persistent_context(args.profile, **opts)
        page = ctx.pages[0] if ctx.pages else await ctx.new_page()
        ua = await page.evaluate("navigator.userAgent")
        if "HeadlessChrome" in ua:
            await ctx.close()
            ctx = await pw.chromium.launch_persistent_context(args.profile, user_agent=ua.replace("HeadlessChrome", "Chrome"),
                                                              **opts)
        return ctx
    except Exception as e:
        if "ProcessSingleton" in str(e):   # Chromium allows one instance per profile
            raise SystemExit(f"The browser profile {args.profile} is in use: is another server.py running? "
                             "Stop it, or pass --profile with another directory.") from None
        raise


async def run(args, url, watch, settings, host, audio):
    async with async_playwright() as pw:
        ctx = await launch(pw, args, audio)
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
            mirror.frame_arrived(ev["data"])
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
        await serve(mirror, host)


async def serve(mirror, host):
    """Mirror to the Tab5 for good. While its settings are open it accepts and closes connections; keep retrying
    quietly, and only report changes (connected, lost, a new kind of error)."""
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
