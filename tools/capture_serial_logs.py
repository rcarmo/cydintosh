#!/usr/bin/env python3
"""Reset an ESP32 board over serial, capture logs for a fixed duration, and write them to a file.

Example:
  python3 tools/capture_serial_logs.py \
    --port /dev/cu.usbserial-210 \
    --baud 115200 \
    --duration 10 \
    --output logs/boot-log.txt
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import time
from datetime import datetime, timezone

try:
    import serial
except ImportError:
    print("pyserial is required. Install with: pip install pyserial", file=sys.stderr)
    raise


def toggle_reset(ser: serial.Serial, settle: float = 0.25) -> None:
    """Reset ESP32 using DTR/RTS lines.

    For many dev boards:
    - EN/reset is tied to RTS
    - boot strap is tied to DTR

    We avoid bootloader mode and do a normal reset pulse.
    """
    try:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.05)
        ser.rts = False
        time.sleep(settle)
        ser.reset_input_buffer()
    except Exception as exc:
        print(f"warning: reset toggle failed: {exc}", file=sys.stderr)


def capture(port: str, baud: int, duration: float, output: pathlib.Path, encoding: str) -> int:
    output.parent.mkdir(parents=True, exist_ok=True)

    started = datetime.now(timezone.utc)
    header = (
        f"# Serial log capture\n"
        f"# started_utc: {started.isoformat()}\n"
        f"# port: {port}\n"
        f"# baud: {baud}\n"
        f"# duration_sec: {duration}\n"
        f"\n"
    )

    with serial.Serial(port=port, baudrate=baud, timeout=0.1, dsrdtr=False, rtscts=False) as ser:
        ser.reset_input_buffer()
        toggle_reset(ser)

        deadline = time.monotonic() + duration
        chunks: list[bytes] = []
        while time.monotonic() < deadline:
            data = ser.read(4096)
            if data:
                chunks.append(data)

    raw = b"".join(chunks)
    text = raw.decode(encoding, errors="replace")
    output.write_text(header + text, encoding="utf-8")

    print(f"wrote {len(raw)} bytes to {output}")
    return 0


def default_output_path() -> pathlib.Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return pathlib.Path("logs") / f"serial-capture-{stamp}.log"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/cu.usbserial-210")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    parser.add_argument("--duration", type=float, default=10.0, help="Capture duration in seconds (default: 10)")
    parser.add_argument("--output", type=pathlib.Path, default=default_output_path(), help="Output file path")
    parser.add_argument("--encoding", default="utf-8", help="Decode encoding for log text (default: utf-8)")
    args = parser.parse_args()

    return capture(args.port, args.baud, args.duration, args.output, args.encoding)


if __name__ == "__main__":
    raise SystemExit(main())
