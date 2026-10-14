#!/usr/bin/env python3
"""Save what the Tab5's panel shows (its frame buffer, read over USB serial) as a landscape PNG.

    tools/shot.py out.png [--port PORT] [--rot 1|3]
"""
import argparse
import time

import numpy as np
import serial
from serial.tools import list_ports
from PIL import Image


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--port", help="USB serial port (default: the first Espressif device)")
    ap.add_argument("--rot", type=int, default=1, choices=(1, 3), help="display rotation set on the Tab5")
    args = ap.parse_args()
    if not args.port:
        ports = [p.device for p in list_ports.comports() if p.vid == 0x303A]
        if not ports:
            raise SystemExit("no Tab5 found on USB; pass --port")
        args.port = ports[0]
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, 115200, 2
    s.dtr = s.rts = True
    s.open()
    s.reset_input_buffer()
    s.write(b"F\n")
    end = time.monotonic() + 5
    while time.monotonic() < end:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("FB "):
            break
    else:
        raise SystemExit("no frame buffer header from the Tab5")
    fw, fh, n = map(int, line.split()[1:])
    data, end = b"", time.monotonic() + 60
    while len(data) < n and time.monotonic() < end:
        data += s.read(n - len(data))
    if len(data) != n:
        raise SystemExit(f"short read: {len(data)} of {n} bytes")
    px = np.frombuffer(data, "<u2").reshape(fh, fw)
    rgb = np.stack([(px >> 11) << 3, ((px >> 5) & 63) << 2, (px & 31) << 3], axis=2).astype(np.uint8)
    # panel (portrait) -> landscape: rotation 1 maps logical (x, y) to panel (719 - y, x)
    img = np.rot90(rgb, 1) if args.rot == 1 else np.rot90(rgb, -1)
    Image.fromarray(img).save(args.out)
    print(f"saved {args.out}")


if __name__ == "__main__":
    main()
