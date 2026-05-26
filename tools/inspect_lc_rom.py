#!/usr/bin/env python3
"""Inspect local Macintosh LC ROM metadata without dumping copyrighted contents."""

from __future__ import annotations

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

EXPECTED_SIZE = 0x80000
EXPECTED_FIRST_LONG = 0x350EACF0
EXPECTED_SHA256 = "129391cc72f84c2b321709cad8281e30a45e50b3cf6e7afe7434c4d32c7b9d5b"

DEFAULT_RAM_SIZE = 0x400000
DEFAULT_ROM_BASES = (0x00400000, 0x40800000)
DEFAULT_SCAN_LIMIT = 0x4000


@dataclass(frozen=True)
class VectorCandidate:
    file_offset: int
    sp: int
    pc: int
    rom_base: int
    pc_rom_offset: int
    score: int
    notes: tuple[str, ...]


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    first_long = be32(data, 0) if len(data) >= 4 else None
    sha256 = hashlib.sha256(data).hexdigest()
    return {
        "path": str(path),
        "size": len(data),
        "size_hex": f"0x{len(data):x}",
        "sha256": sha256,
        "md5": hashlib.md5(data).hexdigest(),  # useful for comparing old ROM catalog notes
        "first_long": f"0x{first_long:08X}" if first_long is not None else None,
        "first16_hex": data[:16].hex(" "),
        "matches_expected_size": len(data) == EXPECTED_SIZE,
        "matches_expected_first_long": first_long == EXPECTED_FIRST_LONG,
        "matches_expected_sha256": sha256 == EXPECTED_SHA256,
    }


def plausible_sp(value: int, ram_size: int) -> tuple[bool, tuple[str, ...]]:
    notes: list[str] = []
    if value == 0:
        return False, ("sp-zero",)
    if value & 1:
        return False, ("sp-odd",)
    if value <= ram_size:
        if value >= ram_size - 0x10000:
            notes.append("sp-near-ram-top")
        else:
            notes.append("sp-in-ram")
        return True, tuple(notes)
    return False, ("sp-outside-initial-ram",)


def pc_rom_offset(pc: int, rom_base: int, rom_size: int) -> int | None:
    if rom_base <= pc < rom_base + rom_size:
        return pc - rom_base
    # 24-bit aliases are useful while the first LC mode is 68EC020.
    pc24 = pc & 0x00FFFFFF
    base24 = rom_base & 0x00FFFFFF
    if base24 <= pc24 < base24 + rom_size:
        return pc24 - base24
    return None


def scan_vector_candidates(
    data: bytes,
    *,
    rom_bases: Iterable[int],
    ram_size: int,
    scan_limit: int,
) -> list[VectorCandidate]:
    limit = min(len(data), scan_limit)
    candidates: list[VectorCandidate] = []
    for offset in range(0, max(0, limit - 8 + 1), 4):
        sp = be32(data, offset)
        pc = be32(data, offset + 4)
        sp_ok, sp_notes = plausible_sp(sp, ram_size)
        if not sp_ok:
            continue
        if pc & 1:
            continue
        for rom_base in rom_bases:
            pc_off = pc_rom_offset(pc, rom_base, len(data))
            if pc_off is None:
                continue
            notes = list(sp_notes)
            score = 1
            if offset == 0:
                score += 3
                notes.append("offset-zero-reset-vector")
            if pc_off < 0x1000:
                score += 2
                notes.append("pc-near-rom-start")
            if pc_off < len(data):
                score += 1
                notes.append("pc-inside-rom-file")
            if (pc & 0xFF000000) == (rom_base & 0xFF000000):
                score += 1
                notes.append("pc-matches-base-high-byte")
            candidates.append(
                VectorCandidate(
                    file_offset=offset,
                    sp=sp,
                    pc=pc,
                    rom_base=rom_base,
                    pc_rom_offset=pc_off,
                    score=score,
                    notes=tuple(notes),
                )
            )
    candidates.sort(key=lambda c: (-c.score, c.file_offset, c.rom_base))
    return candidates


def parse_int(value: str) -> int:
    return int(value, 0)


def print_vector_analysis(data: bytes, args: argparse.Namespace) -> None:
    rom_bases = tuple(parse_int(v) for v in args.rom_base)
    print("vector_scan_enabled: True")
    print(f"vector_scan_limit: 0x{min(len(data), args.scan_limit):x}")
    print(f"vector_initial_ram_size: 0x{args.ram_size:x}")
    print("vector_rom_bases: " + ", ".join(f"0x{base:08X}" for base in rom_bases))

    if len(data) >= 8:
        raw_sp = be32(data, 0)
        raw_pc = be32(data, 4)
        sp_ok, sp_notes = plausible_sp(raw_sp, args.ram_size)
        print(f"offset0_reset_sp_plausible: {sp_ok} ({'/'.join(sp_notes)})")
        for base in rom_bases:
            off = pc_rom_offset(raw_pc, base, len(data))
            print(
                f"offset0_pc_in_rom_base_0x{base:08X}: "
                f"{off is not None}" + (f" pc_rom_offset=0x{off:x}" if off is not None else "")
            )

    candidates = scan_vector_candidates(
        data,
        rom_bases=rom_bases,
        ram_size=args.ram_size,
        scan_limit=args.scan_limit,
    )
    print(f"vector_candidates_found: {len(candidates)}")
    for index, candidate in enumerate(candidates[: args.max_candidates], start=1):
        print(
            "vector_candidate_"
            f"{index}: file_offset=0x{candidate.file_offset:05X} "
            f"sp=0x{candidate.sp:08X} pc=0x{candidate.pc:08X} "
            f"rom_base=0x{candidate.rom_base:08X} pc_rom_offset=0x{candidate.pc_rom_offset:05X} "
            f"score={candidate.score} notes={','.join(candidate.notes)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", nargs="?", default="vendor/mac-lc.rom", type=Path)
    parser.add_argument(
        "--vector-scan",
        action="store_true",
        help="scan for plausible reset-vector SP/PC pairs and ROM window candidates",
    )
    parser.add_argument(
        "--scan-limit",
        type=parse_int,
        default=DEFAULT_SCAN_LIMIT,
        help="bytes to scan from start of ROM for vector-like pairs (default: 0x4000)",
    )
    parser.add_argument(
        "--ram-size",
        type=parse_int,
        default=DEFAULT_RAM_SIZE,
        help="initial guest RAM size used for SP plausibility (default: 0x400000)",
    )
    parser.add_argument(
        "--rom-base",
        action="append",
        default=[f"0x{base:x}" for base in DEFAULT_ROM_BASES],
        help="candidate ROM base; repeatable (defaults: 0x00400000 and 0x40800000)",
    )
    parser.add_argument(
        "--max-candidates",
        type=int,
        default=12,
        help="maximum vector candidates to print (default: 12)",
    )
    args = parser.parse_args()

    data = args.rom.read_bytes()
    info = inspect(args.rom)
    for key, value in info.items():
        print(f"{key}: {value}")

    if args.vector_scan:
        print_vector_analysis(data, args)

    if not (
        info["matches_expected_size"]
        and info["matches_expected_first_long"]
        and info["matches_expected_sha256"]
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
