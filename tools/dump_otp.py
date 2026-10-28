#!/usr/bin/env python3
"""Read a raw OTP dump from the GDEP073E01 / SPD1657A over serial.

Prerequisite: build and flash the firmware with -DEPD_DUMP_OTP=1, e.g.

    PLATFORMIO_BUILD_FLAGS="-DEPD_DUMP_OTP=1" pio run -t upload
    python3 tools/dump_otp.py --port /dev/ttyUSB0 -o assets/otp.bin

The firmware prints "[otp] BEGIN 96000" followed by 96,000 raw bytes and
"[otp] END". Only read commands (ROTP 0x92 / DSP 0x11) are used.

Note: on the ESP32-L(C73) + DESPI-C73 wiring the first five 400-byte OTP pages
are reproducible; later pages can contain bit errors depending on the unit and
read timing. See docs/otp-readback.md.
"""

from __future__ import annotations

import argparse
import hashlib
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:  # pragma: no cover
    sys.exit("pyserial is required: python3 -m pip install pyserial")

DEFAULT_SIZE = 96000


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("-o", "--output", type=Path, default=Path("otp.bin"))
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE)
    parser.add_argument("--timeout", type=float, default=40.0)
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1)
    # Normal-boot reset pulse on the ESP32 auto-reset circuit (EN=RTS, IO0=DTR).
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    time.sleep(0.1)
    ser.reset_input_buffer()

    marker = f"[otp] BEGIN {args.size}"
    start = time.time()
    while time.time() - start < 15:
        line = ser.readline()
        if not line:
            continue
        text = line.decode("utf-8", "replace").strip()
        if text:
            print(text)
        if marker in text:
            break
    else:
        ser.close()
        sys.exit(f"marker not seen: {marker!r}")

    data = bytearray()
    start = time.time()
    while len(data) < args.size and time.time() - start < args.timeout:
        chunk = ser.read(min(4096, args.size - len(data)))
        if chunk:
            data.extend(chunk)
        else:
            time.sleep(0.01)

    tail = b""
    start = time.time()
    while b"[otp] END" not in tail and time.time() - start < 5:
        tail += ser.read(64)
    ser.close()

    if len(data) != args.size:
        sys.exit(f"short dump: {len(data)} of {args.size} bytes; tail={tail!r}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(f"saved {args.output}: {len(data)} bytes")
    print("sha256:", hashlib.sha256(data).hexdigest())
    print("first 32:", data[:32].hex(" ").upper())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
