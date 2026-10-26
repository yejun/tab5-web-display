#!/usr/bin/env python3
"""Reach the Tab5 from anywhere, through a tunnel or reverse proxy of your own (a Cloudflare Tunnel, ngrok, Tailscale
Funnel, nginx, ...): the Tab5 connects out to this PC, and this stands in for the Tab5 on 127.0.0.1 for server.py.

    server.py --listen 8790 ...         listens on 127.0.0.1:8790 and prints the key for the Tab5's address
    (your tunnel)                       https://tab5.example.com -> http://127.0.0.1:8790
    on the Tab5 (serial):               tunnel https://tab5.example.com/<key>

    python server/tunnel.py --listen 8790     the same on its own, for: server.py --device 127.0.0.1 ...

The address given to the Tab5 ends in a random key (made once, kept in KEY_FILE), and nothing without it gets in.
Through a tunnel it is https (wss); ws:// and http:// addresses work too, for a VPN or the local network.

The Tab5 keeps a WebSocket open to <address>/tab5 (see firmware/web_display/tunnel.h). When server.py connects to
127.0.0.1:8765, this says "attach" on it and joins the two: the server's bytes go to the Tab5 as binary messages, and
the Tab5's come back. For each lane the Tab5 offers it opens <address>/lane, and each is joined to one of the server's
connections to 127.0.0.1:8766.

Sound: the server sends its datagrams to 127.0.0.1:8765 (UDP), and each goes on to the Tab5 as a message of its own on
the main WebSocket. Every message there opens with a channel byte: 0 = the server's bytes, 1 = a sound datagram.
"""
import argparse, asyncio, base64, hashlib, os, pathlib, secrets, socket, struct, time

MAIN_PORT, LANE_PORT = 8765, 8766   # (server.py's; the Tab5 offers no lanes through the tunnel, see tunnel.h)
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
PING_S = 20        # tunnels close a quiet WebSocket (Cloudflare after 100 s)
LANE_WAIT_S = 6
KEY_FILE = pathlib.Path(os.environ.get("XDG_DATA_HOME", pathlib.Path.home() / ".local/share")) / "tab5-web-display-key"


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
    def __init__(self, key, verbose):
        self.key, self.verbose = key, verbose
        self.say = print if verbose else (lambda *_: None)
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
        kind = path[len(self.key) + 2:] if path.startswith(f"/{self.key}/") else None
        if kind not in ("tab5", "lane") or headers.get("upgrade", "").lower() != "websocket" or not key:
            writer.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            writer.close()
            return
        accept = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
        writer.write(f"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     f"Sec-WebSocket-Accept: {accept}\r\n\r\n".encode())
        nodelay(writer)
        writer.transport.set_write_buffer_limits(high=32768)   # (sound waits behind no more of the stream than this)
        ws = Ws(reader, writer)
        if kind == "tab5":
            if self.tab5:
                self.tab5.close()        # the newest wins (a Tab5 that restarted)
            self.tab5 = ws
            self.say("tunnel: the Tab5 is there")
        else:
            self.lanes.put_nowait(ws)

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


def tunnel_key():
    """The key in the Tab5's address: made once, so the address stays the same."""
    try:
        return KEY_FILE.read_text().strip()
    except OSError:
        key = secrets.token_urlsafe(12)
        KEY_FILE.parent.mkdir(parents=True, exist_ok=True)
        KEY_FILE.write_text(key + "\n")
        KEY_FILE.chmod(0o600)
        return key


async def start(listen, verbose=False, host="127.0.0.1"):
    """Start the bridge in the running event loop: the Tab5's WebSockets on listen (host, port), and a stand-in for the
    Tab5 on host for server.py, whose port there must be free. Returns the key."""
    bridge = Bridge(tunnel_key(), verbose)
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
    return bridge.key


def how_to(listen, key):
    return (f"listening for the Tab5 on http://{listen[0]}:{listen[1]}. Point your tunnel there; then on the Tab5 "
            f"(serial): tunnel https://<your tunnel>/{key}")


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="8790", metavar="[HOST:]PORT",
                    help="where the Tab5's WebSockets arrive, from your tunnel (default 127.0.0.1:8790)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the traffic every 10 s")
    args = ap.parse_args()
    listen = parse_listen(args.listen)
    key = await start(listen, args.verbose)
    print(how_to(listen, key) + "\nOn this PC: python server/server.py --device 127.0.0.1 ...")
    await asyncio.Event().wait()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
