#!/usr/bin/env python3
"""Render the LC 512x384 indexed debug pattern to a portable PPM image.

This mirrors the firmware scaffold in src/machine_lc/lc_video.c and does not use
ROM or disk contents. It is for off-device validation of the indexed framebuffer,
debug CLUT, and RGB conversion path while Tab5 display bring-up is pending.
"""

from __future__ import annotations

import argparse
from pathlib import Path

WIDTH = 512
HEIGHT = 384
CLUT_ENTRIES = 256


def debug_palette() -> list[tuple[int, int, int]]:
    palette: list[tuple[int, int, int]] = []
    for i in range(CLUT_ENTRIES):
        r = i & 0xFF
        g = (i * 5) & 0xFF
        b = 255 - i
        palette.append((r, g, b))
    return palette


def indexed_pixel(x: int, y: int) -> int:
    ramp = (x * 255) // (WIDTH - 1)
    stripe = 0x40 if ((y // 16) & 1) else 0x00
    marker = 0x80 if x < 16 or y < 16 or x >= WIDTH - 16 or y >= HEIGHT - 16 else 0x00
    return (ramp ^ stripe ^ marker) & 0xFF


def fnv1a_update(checksum: int, value: int) -> int:
    checksum ^= value & 0xFF
    return (checksum * 16777619) & 0xFFFFFFFF


def render_ppm(path: Path) -> tuple[int, int]:
    palette = debug_palette()
    indexed_checksum = 2166136261
    rgb_checksum = 2166136261

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(f"P6\n{WIDTH} {HEIGHT}\n255\n".encode("ascii"))
        for y in range(HEIGHT):
            row = bytearray()
            for x in range(WIDTH):
                index = indexed_pixel(x, y)
                indexed_checksum = fnv1a_update(indexed_checksum, index)
                r, g, b = palette[index]
                row.extend((r, g, b))
                rgb_checksum = fnv1a_update(fnv1a_update(fnv1a_update(rgb_checksum, r), g), b)
            f.write(row)

    return indexed_checksum, rgb_checksum


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output",
        nargs="?",
        default="artifacts/lc-video-test-pattern.ppm",
        type=Path,
        help="output PPM path (default: artifacts/lc-video-test-pattern.ppm)",
    )
    args = parser.parse_args()

    indexed_checksum, rgb_checksum = render_ppm(args.output)
    print(f"path: {args.output}")
    print(f"width: {WIDTH}")
    print(f"height: {HEIGHT}")
    print("format: PPM/P6/RGB888")
    print(f"indexed_checksum: 0x{indexed_checksum:08X}")
    print(f"rgb_checksum: 0x{rgb_checksum:08X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
