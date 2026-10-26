#!/usr/bin/env python3
"""Reach the Tab5 from anywhere, through a tunnel or reverse proxy of your own (a Cloudflare Tunnel, ngrok, Tailscale
Funnel, nginx, ...): the Tab5 connects out to this PC, and this stands in for the Tab5 on 127.0.0.1 for server.py.

    server.py --listen 8790 ...         listens on 127.0.0.1:8790
    (your tunnel)                       https://tab5.example.com -> http://127.0.0.1:8790
    on the Tab5                         settings: Remote on, https://tab5.example.com (serial: tunnel <address>)

    python server/tunnel.py --listen 8790     the same on its own, for: server.py --device 127.0.0.1 ...

Pairing: each Tab5 sends its random ID (X-Tab5-Device) and shows a six-digit code made from it. A Tab5 this PC doesn't
know gets nothing until it is allowed: asked here on the terminal ("Pair the Tab5 with code 482 913? [y/N]"), with
server.py --pair 482913 from another one, or over USB (server.py pairs a Tab5 plugged in when it starts). The paired
Tab5s are kept, as hashes of their IDs, in DEVICES_FILE; server.py --devices lists them, --unpair <code> removes one.

The Tab5 keeps a WebSocket open to <address>/tab5 (see firmware/web_display/tunnel.h). When server.py connects to
127.0.0.1:8765, this says "attach" on it and joins the two: the server's bytes go to the Tab5 as binary messages, and
the Tab5's come back. For each lane the Tab5 offers it opens <address>/lane, and each is joined to one of the server's
connections to 127.0.0.1:8766.

Sound: the server sends its datagrams to 127.0.0.1:8765 (UDP), and each goes on to the Tab5 as a message of its own on
the main WebSocket. Every message there opens with a channel byte: 0 = the server's bytes, 1 = a sound datagram.
"""
import argparse, asyncio, base64, datetime, hashlib, json, os, pathlib, re, socket, struct, sys, threading, time

MAIN_PORT, LANE_PORT = 8765, 8766   # (server.py's; the Tab5 offers no lanes through the tunnel, see tunnel.h)
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PING_S = 20        # tunnels close a quiet WebSocket (Cloudflare after 100 s)
LANE_WAIT_S = 6
DATA_DIR = pathlib.Path(os.environ.get("XDG_DATA_HOME", pathlib.Path.home() / ".local/share"))
DEVICES_FILE = DATA_DIR / "tab5-web-display-devices.json"   # the paired Tab5s
PENDING_FILE = DATA_DIR / "tab5-web-display-pending.json"   # a Tab5 asking to pair (for --pair)
ASK_S, ASK_UNATTENDED_S = 60, 300   # how long a pairing request waits for an answer (on the terminal / without one)


# ---- pairing ----

def device_hash(device_id):
    return hashlib.sha256(device_id.encode()).hexdigest()


def pairing_code(h):
    """The six digits the Tab5 shows, from its ID's hash (as tunnel.h makes them)."""
    c = int(h[:8], 16) % 1000000
    return f"{c // 1000:03d} {c % 1000:03d}"


def load(path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, indent=1) + "\n")
    tmp.chmod(0o600)
    tmp.replace(path)


def add_device(h, how):
    devices = load(DEVICES_FILE)
    devices[h] = {"code": pairing_code(h), "paired": datetime.date.today().isoformat(), "how": how}
    save(DEVICES_FILE, devices)


def find(entries, code):
    code = code.replace(" ", "")
    return [h for h, e in entries.items() if e.get("code", "").replace(" ", "") == code]


def pair_command(code):
    """server.py --pair <code>: allow the Tab5 that is asking with that code."""
    pending = load(PENDING_FILE)
    hs = find(pending, code)
    if not hs:
        raise SystemExit(f"no Tab5 with code {code} is asking to pair (is server.py --listen running, and the Tab5's "
                         "remote mode on?)")
    add_device(hs[0], "with --pair")
    print(f"paired the Tab5 with code {pairing_code(hs[0])}")


def devices_command():
    devices = load(DEVICES_FILE)
    if not devices:
        print(f"no paired Tab5s ({DEVICES_FILE})")
    for e in devices.values():
        print(f"{e.get('code')}   paired {e.get('paired')} {e.get('how', '')}")


def unpair_command(code):
    devices = load(DEVICES_FILE)
    hs = find(devices, code)
    if not hs:
        raise SystemExit(f"no paired Tab5 with code {code}; server.py --devices lists them")
    for h in hs:
        del devices[h]
    save(DEVICES_FILE, devices)
    print(f"unpaired the Tab5 with code {pairing_code(hs[0])}")


def unmask(data, mask):
    n = len(data)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "big") ^ int.from_bytes(key, "big")).to_bytes(n, "big")


def nodelay(writer):
    sock = writer.get_extra_info("socket")
    if sock is not None:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


class Ws:
    """The server side of one of the Tab5's WebSockets. Data messages go to inbox; None there means it closed."""

    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.inbox, self.dead, self.last_rx = asyncio.Queue(), False, time.monotonic()
        self.tasks = [asyncio.create_task(self.read()), asyncio.create_task(self.ping())]

    def send(self, op, data=b""):
        n = len(data)
        hdr = bytes([0x80 | op]) + (bytes([n]) if n < 126 else struct.pack("!BH", 126, n) if n < 65536 else struct.pack("!BQ", 127, n))
        self.writer.write(hdr)
        self.writer.write(data)

    async def read(self):
        r = self.reader
        try:
            while True:
                h = await r.readexactly(2)
                op, n = h[0] & 15, h[1] & 127
                if n == 126:
                    n = struct.unpack("!H", await r.readexactly(2))[0]
                elif n == 127:
                    n = struct.unpack("!Q", await r.readexactly(8))[0]
                mask = await r.readexactly(4) if h[1] & 128 else None
                data = await r.readexactly(n)
                if mask:
                    data = unmask(data, mask)
                self.last_rx = time.monotonic()
                if op == 8:
                    break
                if op == 9:
                    self.send(10, data)
                elif op in (0, 2):
                    self.inbox.put_nowait(data)
        except (OSError, asyncio.IncompleteReadError):
            pass
        finally:
            self.close()

    async def ping(self):
        while not self.dead:
            await asyncio.sleep(PING_S)
            if time.monotonic() - self.last_rx > 3 * PING_S:   # (the Tab5 answers each ping)
                self.close()
            elif not self.dead:
                self.send(9)

    def close(self):
        if not self.dead:
            self.dead = True
            self.inbox.put_nowait(None)
            self.writer.close()
            for t in self.tasks:
                if t is not asyncio.current_task():
                    t.cancel()


class Bridge:
    def __init__(self, verbose):
        self.verbose = verbose
        self.say = print if verbose else (lambda *_: None)
        self.asking = asyncio.Lock()     # one pairing question at a time
        self.lines = None                # the terminal's lines, while there is one
        self.tab5 = None                 # the Tab5's WebSocket, waiting for the server
        self.lanes = asyncio.Queue()     # lane WebSockets, waiting for the server's lane connections
        self.joined = None               # the main WebSocket of the current session (sound goes there)
        self.sent = self.received = self.sound = 0

    async def on_http(self, reader, writer):
        """The tunnel's requests: the Tab5's WebSockets."""
        try:
            head = (await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)).decode("latin-1")
        except (OSError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError):
            writer.close()
            return
        lines = head.split("\r\n")
        path = lines[0].split(" ")[1] if lines[0].count(" ") >= 2 else ""
        headers = {k.strip().lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:] if ":" in l)}
        key = headers.get("sec-websocket-key")
        kind = path.rpartition("/")[2].partition("?")[0]   # (after any path of the tunnel's own)
        device = headers.get("x-tab5-device", "")
        if (kind not in ("tab5", "lane") or headers.get("upgrade", "").lower() != "websocket" or not key or
                not re.fullmatch(r"[0-9a-f]{32}", device)):
            writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            writer.close()
            return
        accept = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
        writer.write(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     f"Sec-WebSocket-Accept: {accept}\r\n\r\n".encode())
        nodelay(writer)
        writer.transport.set_write_buffer_limits(high=32768)   # (sound waits behind no more of the stream than this)
        ws, h = Ws(reader, writer), device_hash(device)
        if h not in load(DEVICES_FILE):
            if kind == "tab5":
                asyncio.create_task(self.ask(ws, h))
            else:
                ws.close()
            return
        if kind == "tab5":
            ws.send(1, b"ok")
            self.waiting(ws)
        else:
            self.lanes.put_nowait(ws)

    def waiting(self, ws):
        if self.tab5:
            self.tab5.close()            # the newest wins (a Tab5 that restarted)
        self.tab5 = ws
        self.say("tunnel: the Tab5 is there")

    async def ask(self, ws, h):
        """A Tab5 this PC doesn't know: allowed (on the terminal, with --pair, or over USB meanwhile), or not."""
        code = pairing_code(h)
        ws.send(1, b"pair")
        async with self.asking:
            if ws.dead:
                return
            pending = load(PENDING_FILE)
            pending[h] = {"code": code, "since": time.time()}
            save(PENDING_FILE, pending)
            try:
                allowed = await self.answer(code, h, ws)
            finally:
                pending = load(PENDING_FILE)
                pending.pop(h, None)
                save(PENDING_FILE, pending)
        if allowed and h not in load(DEVICES_FILE):
            add_device(h, "on the terminal")
        if allowed and not ws.dead:
            print(f"tunnel: paired the Tab5 with code {code}")
            ws.send(1, b"ok")
            self.waiting(ws)
        elif not ws.dead:
            ws.send(1, b"denied")
            await asyncio.sleep(0.5)
            ws.close()

    async def answer(self, code, h, ws):
        tty = self.lines is not None
        while tty and not self.lines.empty():   # (whatever was typed before the question)
            self.lines.get_nowait()
        print(f"\ntunnel: a Tab5 asks to pair, with code {code}. " +
              ("Pair it? [y/N] " if tty else f"To allow it: server.py --pair {code.replace(' ', '')}"), end="" if tty else "\n",
              flush=True)
        end = time.monotonic() + (ASK_S if tty else ASK_UNATTENDED_S)
        while time.monotonic() < end and not ws.dead:
            if h in load(DEVICES_FILE):   # allowed some other way
                if tty:
                    print()
                return True
            if not tty:
                await asyncio.sleep(1)
                continue
            try:
                line = await asyncio.wait_for(self.lines.get(), 1)
            except asyncio.TimeoutError:
                continue
            if line.strip().lower() in ("y", "yes"):
                return True
            print("tunnel: not paired")
            return False
        print("\ntunnel: not paired (no answer)" if not ws.dead else "\ntunnel: the Tab5 went away")
        return False

    def read_terminal(self, loop):
        """Lines typed on the terminal, for the pairing question (a thread: waiting on stdin works everywhere)."""
        self.lines = asyncio.Queue()
        def run():
            for line in sys.stdin:
                loop.call_soon_threadsafe(self.lines.put_nowait, line)
        threading.Thread(target=run, daemon=True).start()

    async def join(self, reader, writer, ws, channel=b""):
        async def up():                  # server -> Tab5
            while data := await reader.read(65536):
                ws.send(2, channel + data)
                self.sent += len(data)
                await ws.writer.drain()
        async def down():                # Tab5 -> server
            while (data := await ws.inbox.get()) is not None:
                writer.write(data)
                self.received += len(data)
                await writer.drain()
        tasks = [asyncio.create_task(up()), asyncio.create_task(down())]
        try:
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        except (OSError, ConnectionError):
            pass
        for t in tasks:
            t.cancel()
        ws.close()
        writer.close()

    async def on_main(self, reader, writer):
        """server.py connected: join it to the waiting Tab5, if there is one (the server retries every 2 s)."""
        ws, self.tab5 = self.tab5, None
        if not ws or ws.dead:
            writer.close()
            return
        nodelay(writer)
        self.say("tunnel: server.py joined to the Tab5")
        ws.send(1, b"attach")
        self.joined = ws
        try:
            await self.join(reader, writer, ws, b"\x00")
        finally:
            if self.joined is ws:
                self.joined = None
        self.say("tunnel: server.py and the Tab5 parted")

    def on_sound(self, data):
        ws = self.joined
        if ws and not ws.dead and data[:4] == b"TAUD":
            ws.send(2, b"\x01" + data)
            self.sound += len(data)

    async def on_lane(self, reader, writer):
        nodelay(writer)
        end = time.monotonic() + LANE_WAIT_S
        while True:
            try:
                ws = await asyncio.wait_for(self.lanes.get(), max(0.01, end - time.monotonic()))
            except asyncio.TimeoutError:
                print("tunnel: a lane from server.py found no lane from the Tab5")
                writer.close()
                return
            if not ws.dead:
                break
        await self.join(reader, writer, ws)

    async def report(self):
        last = time.monotonic()
        while True:
            await asyncio.sleep(10)
            now = time.monotonic()
            if self.sent or self.received or self.sound:
                print(f"tunnel: to the Tab5 {self.sent / (now - last) / 1e6:.2f} MB/s and sound {self.sound / (now - last) / 1e3:.0f} KB/s, "
                      f"back {self.received / (now - last) / 1e3:.1f} KB/s")
            self.sent = self.received = self.sound = 0
            last = now


def parse_listen(text):
    """"8790" or "0.0.0.0:8790" -> (host, port); the host defaults to 127.0.0.1 (a tunnel on this PC)."""
    host, _, port = text.rpartition(":")
    if not port.isdigit():
        raise SystemExit(f"--listen takes [HOST:]PORT, got {text!r}")
    return host.strip("[]") or "127.0.0.1", int(port)


async def start(listen, verbose=False, host="127.0.0.1"):
    """Start the bridge in the running event loop: the Tab5's WebSockets on listen (host, port), and a stand-in for the
    Tab5 on host for server.py, whose port there must be free."""
    bridge = Bridge(verbose)
    if sys.stdin is not None and sys.stdin.isatty():
        bridge.read_terminal(asyncio.get_running_loop())
    try:
        await asyncio.start_server(bridge.on_http, *listen)
    except OSError as e:
        raise SystemExit(f"can't listen on {listen[0]}:{listen[1]} ({e.strerror})") from None
    try:
        await asyncio.start_server(bridge.on_main, host, MAIN_PORT)
        class SoundIn(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                bridge.on_sound(data)
        await asyncio.get_running_loop().create_datagram_endpoint(SoundIn, local_addr=(host, MAIN_PORT))
    except OSError as e:
        raise SystemExit(f"can't stand in for the Tab5 on {host}:{MAIN_PORT} ({e.strerror}); is another server.py "
                         "running?") from None
    try:
        await asyncio.start_server(bridge.on_lane, host, LANE_PORT)
    except OSError:
        pass   # (the Tab5 offers no lanes through a tunnel)
    if verbose:
        asyncio.create_task(bridge.report())


def how_to(listen):
    n = len(load(DEVICES_FILE))
    return (f"listening for the Tab5 on http://{listen[0]}:{listen[1]}: point your tunnel there, and give the Tab5 "
            f"its https address (settings: Remote). {n} Tab5{'' if n == 1 else 's'} paired.")


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="8790", metavar="[HOST:]PORT",
                    help="where the Tab5's WebSockets arrive, from your tunnel (default 127.0.0.1:8790)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the traffic every 10 s")
    ap.add_argument("--pair", metavar="CODE", help="allow the Tab5 asking to pair with this code")
    ap.add_argument("--devices", action="store_true", help="list the paired Tab5s")
    ap.add_argument("--unpair", metavar="CODE", help="forget a paired Tab5")
    args = ap.parse_args()
    if args.pair or args.devices or args.unpair:
        return pair_command(args.pair) if args.pair else devices_command() if args.devices else unpair_command(args.unpair)
    listen = parse_listen(args.listen)
    await start(listen, args.verbose)
    print(how_to(listen) + "\nOn this PC: python server/server.py --device 127.0.0.1 ...")
    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
