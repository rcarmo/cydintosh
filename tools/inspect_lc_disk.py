#!/usr/bin/env python3
"""Inspect local Macintosh LC boot disk image metadata without dumping contents."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

KNOWN_SIZES = {
    800 * 1024: "800K Macintosh floppy",
    1440 * 1024: "1.44MB Macintosh HD floppy",
}


def be16(data: bytes, offset: int) -> int | None:
    if len(data) < offset + 2:
        return None
    return struct.unpack_from(">H", data, offset)[0]


def ascii_or_hex(raw: bytes) -> str:
    if all(32 <= b <= 126 for b in raw):
        return raw.decode("ascii")
    return raw.hex(" ")


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    size = len(data)
    mdb_sig = data[1024:1026] if size >= 1026 else b""
    boot_sig = data[:2] if size >= 2 else b""
    alloc_block_size = be16(data, 1024 + 20)
    alloc_block_count = be16(data, 1024 + 18)
    return {
        "path": str(path),
        "size": size,
        "size_hex": f"0x{size:x}",
        "known_size": KNOWN_SIZES.get(size, "unknown/custom"),
        "sha256": hashlib.sha256(data).hexdigest(),
        "md5": hashlib.md5(data).hexdigest(),
        "boot_block_signature": ascii_or_hex(boot_sig),
        "hfs_mdb_signature_at_1024": ascii_or_hex(mdb_sig),
        "looks_hfs": mdb_sig in (b"BD", b"H+", b"HX"),
        "alloc_block_count_metadata": alloc_block_count,
        "alloc_block_size_metadata": alloc_block_size,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("disk", nargs="?", default="vendor/lc-disk.img", type=Path)
    parser.add_argument(
        "--allow-missing",
        action="store_true",
        help="print expected local path and return success if the image is absent",
    )
    args = parser.parse_args()

    if not args.disk.exists():
        print(f"path: {args.disk}")
        print("present: False")
        print("expected_local_only: vendor/lc-disk.img")
        print("note: disk image is user-supplied and must not be committed")
        return 0 if args.allow_missing else 1

    print("present: True")
    info = inspect(args.disk)
    for key, value in info.items():
        print(f"{key}: {value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
