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
DEFAULT_ENTRY_SCAN_LIMIT = 0x100


@dataclass(frozen=True)
class VectorCandidate:
    file_offset: int
    sp: int
    pc: int
    rom_base: int
    pc_rom_offset: int
    score: int
    opcode: int | None
    opcode_hint: str
    notes: tuple[str, ...]


@dataclass(frozen=True)
class EntryHint:
    file_offset: int
    opcode: int
    hint: str
    target_offset: int | None
    target_opcode: int | None
    target_hint: str


def be32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def be16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def opcode_hint(opcode: int | None) -> str:
    if opcode is None:
        return "unavailable"
    if opcode == 0x4E71:
        return "nop"
    if opcode == 0x4E75:
        return "rts"
    if opcode == 0x4E73:
        return "rte"
    if opcode == 0x4EFA:
        return "jmp-pc-relative"
    if opcode == 0x4EBA:
        return "jsr-pc-relative"
    if (opcode & 0xFF00) == 0x6000:
        return "bra"
    if (opcode & 0xF000) == 0x6000:
        return "bcc/bsr"
    if (opcode & 0xF100) == 0x7000:
        return "moveq"
    if (opcode & 0xF1C0) == 0x41C0:
        return "lea"
    if (opcode & 0xF000) == 0x2000:
        return "move-long-family"
    if (opcode & 0xF000) == 0x3000:
        return "move-word-family"
    if (opcode & 0xF000) == 0x4000:
        return "misc/pea/jsr/jmp-family"
    return "unknown-or-data"


def opcode_hint_is_code(hint: str) -> bool:
    return hint not in {"unknown-or-data", "unavailable"}


def signed16(value: int) -> int:
    return value - 0x10000 if value & 0x8000 else value


def signed8(value: int) -> int:
    value &= 0xFF
    return value - 0x100 if value & 0x80 else value


def pc_relative_target(instruction_offset: int, displacement: int) -> int:
    # 68k PC-relative displacements are relative to the extension word address.
    return instruction_offset + 2 + displacement


def entry_transfer_hint(opcode: int) -> str | None:
    if opcode == 0x4EFA:
        return "jmp-pc-relative"
    if opcode == 0x4EBA:
        return "jsr-pc-relative"
    if opcode == 0x6000:
        return "bra-word"
    if (opcode & 0xFF00) == 0x6000:
        return "bra-byte"
    if (opcode & 0xF000) == 0x6000:
        return "bcc/bsr"
    return None


def entry_transfer_has_word_displacement(opcode: int) -> bool:
    if opcode in {0x4EFA, 0x4EBA}:
        return True
    if (opcode & 0xF000) != 0x6000:
        return False
    return (opcode & 0x00FF) == 0


def entry_transfer_has_byte_displacement(opcode: int) -> bool:
    return (opcode & 0xF000) == 0x6000 and (opcode & 0x00FF) not in {0x00, 0xFF}


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
    result = pc_rom_match(pc, rom_base, rom_size)
    return result[0] if result is not None else None


def pc_rom_match(pc: int, rom_base: int, rom_size: int) -> tuple[int, str] | None:
    if rom_base <= pc < rom_base + rom_size:
        return pc - rom_base, "pc-direct-window"
    # 24-bit aliases are useful while the first LC mode is 68EC020.
    pc24 = pc & 0x00FFFFFF
    base24 = rom_base & 0x00FFFFFF
    if base24 <= pc24 < base24 + rom_size:
        return pc24 - base24, "pc-24bit-alias"
    return None


def sp_score(notes: tuple[str, ...]) -> int:
    return 3 if "sp-near-ram-top" in notes else 1


def pc_match_score(match_kind: str, pc: int, rom_base: int, pc_off: int) -> tuple[int, list[str]]:
    score = 1
    notes = [match_kind]
    if match_kind == "pc-direct-window":
        score += 2
    else:
        score += 1
    if pc_off < 0x1000:
        score += 2
        notes.append("pc-near-rom-start")
    if (pc & 0xFF000000) == (rom_base & 0xFF000000):
        score += 1
        notes.append("pc-matches-base-high-byte")
    return score, notes


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
            match = pc_rom_match(pc, rom_base, len(data))
            if match is None:
                continue
            pc_off, match_kind = match
            pc_score, pc_notes = pc_match_score(match_kind, pc, rom_base, pc_off)
            notes = list(sp_notes) + pc_notes
            score = sp_score(sp_notes) + pc_score
            if offset == 0:
                score += 3
                notes.append("offset-zero-reset-vector")
            if pc_off < len(data):
                notes.append("pc-inside-rom-file")
            opcode = be16(data, pc_off) if pc_off + 2 <= len(data) else None
            hint = opcode_hint(opcode)
            if opcode_hint_is_code(hint):
                score += 3
                notes.append(f"opcode-{hint}")
            candidates.append(
                VectorCandidate(
                    file_offset=offset,
                    sp=sp,
                    pc=pc,
                    rom_base=rom_base,
                    pc_rom_offset=pc_off,
                    score=score,
                    opcode=opcode,
                    opcode_hint=hint,
                    notes=tuple(notes),
                )
            )
    candidates.sort(key=lambda c: (-c.score, c.file_offset, c.rom_base))
    return candidates


def parse_int(value: str) -> int:
    return int(value, 0)


def scan_entry_hints(data: bytes, *, scan_limit: int) -> list[EntryHint]:
    limit = min(len(data), scan_limit)
    hints: list[EntryHint] = []
    for offset in range(0, max(0, limit - 2 + 1), 2):
        opcode = be16(data, offset)
        if opcode == 0x4E70:
            hints.append(
                EntryHint(
                    file_offset=offset,
                    opcode=opcode,
                    hint="reset",
                    target_offset=None,
                    target_opcode=None,
                    target_hint="unavailable",
                )
            )
            continue
        hint = entry_transfer_hint(opcode)
        if hint is None:
            continue
        target_offset: int | None = None
        if entry_transfer_has_word_displacement(opcode) and offset + 4 <= len(data):
            target_offset = pc_relative_target(offset, signed16(be16(data, offset + 2)))
        elif entry_transfer_has_byte_displacement(opcode):
            target_offset = offset + 2 + signed8(opcode)
        target_opcode = None
        target_hint = "unavailable"
        if target_offset is not None and 0 <= target_offset + 2 <= len(data):
            target_opcode = be16(data, target_offset)
            target_hint = opcode_hint(target_opcode)
        hints.append(
            EntryHint(
                file_offset=offset,
                opcode=opcode,
                hint=hint,
                target_offset=target_offset,
                target_opcode=target_opcode,
                target_hint=target_hint,
            )
        )
    return hints


def print_entry_analysis(data: bytes, args: argparse.Namespace) -> None:
    rom_bases = tuple(parse_int(v) for v in args.rom_base)
    print("entry_scan_enabled: True")
    print(f"entry_scan_limit: 0x{min(len(data), args.entry_scan_limit):x}")
    if len(data) >= 8:
        print(f"entry_header_long0: 0x{be32(data, 0):08X}")
        print(f"entry_header_long1: 0x{be32(data, 4):08X}")
    hints = scan_entry_hints(data, scan_limit=args.entry_scan_limit)
    print(f"entry_hints_found: {len(hints)}")
    for index, hint in enumerate(hints[: args.max_entry_hints], start=1):
        target = "unavailable" if hint.target_offset is None else f"0x{hint.target_offset:05X}"
        target_addrs = ""
        if hint.target_offset is not None:
            target_addrs = " " + " ".join(
                f"target_at_0x{base + hint.target_offset:08X}" for base in rom_bases
            )
        target_opcode = (
            f"0x{hint.target_opcode:04X}" if hint.target_opcode is not None else "unavailable"
        )
        print(
            f"entry_hint_{index}: file_offset=0x{hint.file_offset:05X} "
            f"opcode=0x{hint.opcode:04X} hint={hint.hint} target={target}{target_addrs} "
            f"target_opcode={target_opcode} target_hint={hint.target_hint}"
        )


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
            f"score={candidate.score} opcode="
            f"{f'0x{candidate.opcode:04X}' if candidate.opcode is not None else 'unavailable'} "
            f"hint={candidate.opcode_hint} notes={','.join(candidate.notes)}"
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
        "--entry-scan",
        action="store_true",
        help="scan the ROM header area for trampoline/entry instructions such as PC-relative jumps",
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
    parser.add_argument(
        "--entry-scan-limit",
        type=parse_int,
        default=DEFAULT_ENTRY_SCAN_LIMIT,
        help="bytes to scan from start of ROM for entry/trampoline hints (default: 0x100)",
    )
    parser.add_argument(
        "--max-entry-hints",
        type=int,
        default=16,
        help="maximum entry hints to print (default: 16)",
    )
    args = parser.parse_args()

    data = args.rom.read_bytes()
    info = inspect(args.rom)
    for key, value in info.items():
        print(f"{key}: {value}")

    if args.vector_scan:
        print_vector_analysis(data, args)
    if args.entry_scan:
        print_entry_analysis(data, args)

    if not (
        info["matches_expected_size"]
        and info["matches_expected_first_long"]
        and info["matches_expected_sha256"]
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
