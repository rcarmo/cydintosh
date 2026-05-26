#!/usr/bin/env python3
"""Reset an ESP32 board over serial, capture logs for a fixed duration, and write them to a file.

Example:
  python3 tools/capture_serial_logs.py \
    --port /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0 \
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


def parse_bool(value: str) -> bool:
    value = value.strip().lower()
    if value in {"1", "true", "yes", "on", "high"}:
        return True
    if value in {"0", "false", "no", "off", "low"}:
        return False
    raise argparse.ArgumentTypeError(f"invalid boolean value: {value}")


def toggle_reset(
    ser: serial.Serial,
    *,
    dtr_during_reset: bool = False,
    clear_after_reset: bool = True,
    settle: float = 0.25,
) -> None:
    """Reset ESP32 using DTR/RTS lines.

    For many dev boards:
    - EN/reset is tied to RTS
    - boot strap is tied to DTR

    Some USB-Serial/JTAG boards invert or interpret DTR differently. The Tab5
    enters download mode when DTR is low/false during reset, so callers can force
    DTR high/true for a normal boot.
    """
    try:
        ser.dtr = dtr_during_reset
        ser.rts = True
        time.sleep(0.05)
        ser.rts = False
        time.sleep(settle)
        if clear_after_reset:
            ser.reset_input_buffer()
    except Exception as exc:
        print(f"warning: reset toggle failed: {exc}", file=sys.stderr)


def capture(
    port: str,
    baud: int,
    duration: float,
    output: pathlib.Path,
    encoding: str,
    *,
    reset: bool,
    dtr_during_reset: bool,
    clear_after_reset: bool,
) -> int:
    output.parent.mkdir(parents=True, exist_ok=True)

    started = datetime.now(timezone.utc)
    header = (
        f"# Serial log capture\n"
        f"# started_utc: {started.isoformat()}\n"
        f"# port: {port}\n"
        f"# baud: {baud}\n"
        f"# duration_sec: {duration}\n"
        f"# reset: {reset}\n"
        f"# dtr_during_reset: {dtr_during_reset}\n"
        f"# clear_after_reset: {clear_after_reset}\n"
        f"\n"
    )

    with serial.Serial(port=port, baudrate=baud, timeout=0.1, dsrdtr=False, rtscts=False) as ser:
        if clear_after_reset:
            ser.reset_input_buffer()
        if reset:
            toggle_reset(
                ser,
                dtr_during_reset=dtr_during_reset,
                clear_after_reset=clear_after_reset,
            )

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
    parser.add_argument("--port", required=True, help="Serial port, e.g. /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    parser.add_argument("--duration", type=float, default=10.0, help="Capture duration in seconds (default: 10)")
    parser.add_argument("--output", type=pathlib.Path, default=default_output_path(), help="Output file path")
    parser.add_argument("--encoding", default="utf-8", help="Decode encoding for log text (default: utf-8)")
    parser.add_argument("--no-reset", action="store_true", help="do not toggle reset before capture")
    parser.add_argument(
        "--dtr-during-reset",
        type=parse_bool,
        default=False,
        help="DTR level to hold during reset; use 'true'/'high' for Tab5 normal boot",
    )
    parser.add_argument(
        "--no-clear-after-reset",
        action="store_true",
        help="do not clear the input buffer after reset; useful for very early boot logs",
    )
    args = parser.parse_args()

    return capture(
        args.port,
        args.baud,
        args.duration,
        args.output,
        args.encoding,
        reset=not args.no_reset,
        dtr_during_reset=args.dtr_during_reset,
        clear_after_reset=not args.no_clear_after_reset,
    )


if __name__ == "__main__":
    raise SystemExit(main())
