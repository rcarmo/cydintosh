#!/usr/bin/env python3
"""Inspect local Macintosh LC ROM metadata without dumping copyrighted contents."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

EXPECTED_SIZE = 0x80000
EXPECTED_FIRST_LONG = 0x350EACF0
EXPECTED_SHA256 = "129391cc72f84c2b321709cad8281e30a45e50b3cf6e7afe7434c4d32c7b9d5b"


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    first_long = struct.unpack(">I", data[:4])[0] if len(data) >= 4 else None
    return {
        "path": str(path),
        "size": len(data),
        "size_hex": f"0x{len(data):x}",
        "sha256": hashlib.sha256(data).hexdigest(),
        "md5": hashlib.md5(data).hexdigest(),  # useful for comparing old ROM catalog notes
        "first_long": f"0x{first_long:08X}" if first_long is not None else None,
        "first16_hex": data[:16].hex(" "),
        "matches_expected_size": len(data) == EXPECTED_SIZE,
        "matches_expected_first_long": first_long == EXPECTED_FIRST_LONG,
        "matches_expected_sha256": hashlib.sha256(data).hexdigest() == EXPECTED_SHA256,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", nargs="?", default="vendor/mac-lc.rom", type=Path)
    args = parser.parse_args()

    info = inspect(args.rom)
    for key, value in info.items():
        print(f"{key}: {value}")

    if not (
        info["matches_expected_size"]
        and info["matches_expected_first_long"]
        and info["matches_expected_sha256"]
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
