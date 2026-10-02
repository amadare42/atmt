#!/usr/bin/env python3
"""PE probing helpers for ed8.exe (Trails of Cold Steel PC, 32-bit).

Usage examples (run from repo root, ed8.exe path via --exe or TOCS_EXE env):

  python tools/pe_probe.py imports
  python tools/pe_probe.py strings --match dat_us
  python tools/pe_probe.py xrefs --string dat_us
  python tools/pe_probe.py disasm --va 0x5a0631 --before 0x40 --after 0x80

Only read-only operations; nothing is modified.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from dataclasses import dataclass

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM

try:  # game strings may contain non-codepage characters
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

DEFAULT_EXE = os.environ.get(
    "TOCS_EXE", r"C:\Program Files (x86)\Steam\steamapps\common\Trails of Cold Steel\ed8.exe"
)


@dataclass
class Exe:
    path: str
    pe: pefile.PE
    image_base: int
    text_va: int
    text_size: int
    data: bytes

    @classmethod
    def load(cls, path: str) -> "Exe":
        pe = pefile.PE(path, fast_load=False)
        data = open(path, "rb").read()
        text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
        return cls(
            path=path,
            pe=pe,
            image_base=pe.OPTIONAL_HEADER.ImageBase,
            text_va=text.VirtualAddress + pe.OPTIONAL_HEADER.ImageBase,
            text_size=text.Misc_VirtualSize,
            data=data,
        )

    def va_to_offset(self, va: int) -> int | None:
        rva = va - self.image_base
        for s in self.pe.sections:
            if s.VirtualAddress <= rva < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
                return s.PointerToRawData + (rva - s.VirtualAddress)
        return None

    def offset_to_va(self, off: int) -> int | None:
        for s in self.pe.sections:
            if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
                return self.image_base + s.VirtualAddress + (off - s.PointerToRawData)
        return None
    def cstring_at_va(self, va: int) -> str | None:
        off = self.va_to_offset(va)
        if off is None:
            return None
        end = self.data.find(b"\x00", off)
        if end < 0:
            return None
        return self.data[off:end].decode("utf-8", "replace")




def cmd_imports(exe: Exe, args) -> None:
    for entry in exe.pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode("ascii", "replace")
        names = [
            imp.name.decode("ascii", "replace") if imp.name else f"ordinal#{imp.ordinal}"
            for imp in entry.imports
        ]
        print(f"{dll}: {len(names)} imports")
        if args.verbose:
            for n in names:
                print(f"    {n}")


def cmd_refs(exe: Exe, args) -> None:
    """Scan .text for 4-byte little-endian references to given VAs (or a --string)."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text_off = exe.va_to_offset(exe.text_va)
    text = exe.data[text_off : text_off + exe.text_size]

    targets: list[tuple[int, str]] = []
    if args.string:
        needle = args.string.encode("ascii")
        for m in re.finditer(re.escape(needle), exe.data):
            va = exe.offset_to_va(m.start())
            if va is not None:
                targets.append((va, exe.cstring_at_va(va) or ""))
    for v in args.va:
        targets.append((v, exe.cstring_at_va(v) or ""))

    for target, label in targets:
        pat = struct.pack("<I", target)
        hits = []
        start = 0
        while True:
            idx = text.find(pat, start)
            if idx < 0:
                break
            hits.append(exe.text_va + idx)
            start = idx + 1
        print(f"# 0x{target:08x} {label!r}: {len(hits)} refs")
        for h in hits:
            # disassemble a few instructions ending at the reference
            off = exe.va_to_offset(h - 12)
            raw = exe.data[off : off + 16]
            insns = list(md.disasm(raw, h - 12))
            text_repr = " | ".join(f"{i.mnemonic} {i.op_str}" for i in insns)
            print(f"    ref@0x{h:08x}  {text_repr}")


def cmd_strings(exe: Exe, args) -> None:
    """Find ASCII literals matching a regex and print their VA."""
    pat = re.compile(args.match.encode("ascii"))
    for m in pat.finditer(exe.data):
        va = exe.offset_to_va(m.start())
        if va is None:
            continue
        s = exe.cstring_at_va(va) or ""
        print(f"va=0x{va:08x} off=0x{m.start():08x} len={len(s)} {s!r}")


def cmd_xrefs(exe: Exe, args) -> None:
    """Find code references to a string literal (push imm32 / mov reg, imm32)."""
    needle = args.string.encode("ascii")
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text_off = exe.va_to_offset(exe.text_va)
    text = exe.data[text_off : text_off + exe.text_size]

    targets = set()
    for m in re.finditer(re.escape(needle), exe.data):
        va = exe.offset_to_va(m.start())
        if va is not None:
            targets.add(va)
    if not targets:
        print(f"literal {args.string!r} not found in image")
        return
    print(f"literal {args.string!r} at: {', '.join(f'0x{t:08x}' for t in sorted(targets))}")

    for insn in md.disasm(text, exe.text_va):
        for op in insn.operands:
            if op.type == CS_OP_IMM and op.imm in targets:
                print(f"0x{insn.address:08x}: {insn.mnemonic} {insn.op_str}")
                break


def cmd_hex(exe: Exe, args) -> None:
    """Find raw byte patterns in .text and disassemble a little around each hit."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    text_off = exe.va_to_offset(exe.text_va)
    text = exe.data[text_off : text_off + exe.text_size]
    pat = bytes(int(b, 16) for b in args.pattern.split())
    hits = []
    start = 0
    while len(hits) < args.limit:
        idx = text.find(pat, start)
        if idx < 0:
            break
        hits.append(exe.text_va + idx)
        start = idx + 1
    print(f"# pattern {args.pattern}: {len(hits)} hit(s)")
    for h in hits:
        print(f"--- 0x{h:08x} " + "-" * 40)
        off = exe.va_to_offset(h - args.context)
        raw = exe.data[off : off + args.context * 2]
        insns = list(md.disasm(raw, h - args.context))
        for insn in insns:
            marker = ">" if insn.address == h else " "
            print(f"{marker} {insn.address:08x}: {insn.bytes.hex():<16} "
                  f"{insn.mnemonic:<8} {insn.op_str}")


def cmd_calls(exe: Exe, args) -> None:
    """Find direct call/jmp sites (E8/E9 rel32) that target the given VAs."""
    text_off = exe.va_to_offset(exe.text_va)
    text = exe.data[text_off : text_off + exe.text_size]
    for target in args.va:
        hits = []
        for i in range(len(text) - 5):
            op = text[i]
            if op not in (0xE8, 0xE9):
                continue
            rel = int.from_bytes(text[i + 1 : i + 5], "little", signed=True)
            if exe.text_va + i + 5 + rel == target:
                hits.append(exe.text_va + i)
        print(f"# 0x{target:08x}: {len(hits)} direct calls")
        for h in hits:
            insn_off = exe.va_to_offset(h - 10)
            md = Cs(CS_ARCH_X86, CS_MODE_32)
            insns = list(md.disasm(exe.data[insn_off : insn_off + 14], h - 10))
            ctx = " | ".join(f"{x.mnemonic} {x.op_str}" for x in insns)
            print(f"    call@0x{h:08x}  func~0x{h - 10:08x}  {ctx}")


def find_function_start(exe: Exe, va: int, limit: int = 0x800) -> int:
    """Best-effort function start: prefer the last int3 padding, else the last
    MSVC prologue (`push ebp; mov ebp, esp`) before `va`."""
    off = exe.va_to_offset(va)
    lo = max(0, off - limit)
    idx = exe.data.rfind(b"\xcc\xcc", lo, off)
    if idx >= 0:
        while idx < off and exe.data[idx + 1] == 0xCC:
            idx += 1
        return exe.offset_to_va(idx + 1) or va
    prologue = exe.data.rfind(b"\x55\x8b\xec", lo, off)
    if prologue >= 0:
        return exe.offset_to_va(prologue) or va
    return va


def cmd_bytes(exe: Exe, args) -> None:
    """Dump raw bytes at a virtual address (jump tables, vtables, structs...)."""
    off = exe.va_to_offset(args.va)
    if off is None:
        print("address not mapped")
        return
    data = exe.data[off : off + args.len]
    for i in range(0, len(data), 16):
        row = data[i : i + 16]
        print(f"{args.va + i:08x}: " + " ".join(f"{b:02x}" for b in row))
    if args.as_dwords:
        words = [int.from_bytes(data[i : i + 4], "little")
                 for i in range(0, len(data) - 3, 4)]
        print("dwords: " + " ".join(f"0x{w:08x}" for w in words))


def cmd_disasm(exe: Exe, args) -> None:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    start = args.va - args.before
    if args.from_func or args.before == 0:
        start = find_function_start(exe, args.va)
    end = args.va + args.after
    off = exe.va_to_offset(start)
    if off is None:
        print("address not mapped")
        return
    raw = exe.data[off : off + (end - start)]
    for insn in md.disasm(raw, start):
        marker = ">" if args.va <= insn.address < end else " "
        calls = ""
        if insn.mnemonic in ("call", "jmp") and insn.operands:
            tgt = insn.operands[0]
            if tgt.type == CS_OP_IMM:
                s = exe.cstring_at_va(tgt.imm)
                if s:
                    calls = f"   ; -> {s!r}"
        print(
            f"{marker} {insn.address:08x}: {insn.bytes.hex():<20} "
            f"{insn.mnemonic:<8} {insn.op_str}{calls}"
        )


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--exe", default=DEFAULT_EXE)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("imports")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(func=cmd_imports)

    p = sub.add_parser("strings")
    p.add_argument("--match", required=True)
    p.set_defaults(func=cmd_strings)

    p = sub.add_parser("xrefs")
    p.add_argument("--string", required=True)
    p.set_defaults(func=cmd_xrefs)

    p = sub.add_parser("refs")
    p.add_argument("--string", default=None)
    p.add_argument("--va", type=lambda x: int(x, 0), action="append", default=[])
    p.set_defaults(func=cmd_refs)

    p = sub.add_parser("hex")
    p.add_argument("--pattern", required=True, help="hex bytes, e.g. '83 f8 23'")
    p.add_argument("--context", type=lambda x: int(x, 0), default=0x20)
    p.add_argument("--limit", type=int, default=10)
    p.set_defaults(func=cmd_hex)

    p = sub.add_parser("bytes")
    p.add_argument("--va", type=lambda x: int(x, 0), required=True)
    p.add_argument("--len", type=int, default=64)
    p.add_argument("--as-dwords", action="store_true")
    p.set_defaults(func=cmd_bytes)

    p = sub.add_parser("calls")
    p.add_argument("--va", type=lambda x: int(x, 0), action="append", required=True)
    p.set_defaults(func=cmd_calls)

    p = sub.add_parser("disasm")
    p.add_argument("--va", type=lambda x: int(x, 0), required=True)
    p.add_argument("--before", type=lambda x: int(x, 0), default=0x20)
    p.add_argument("--after", type=lambda x: int(x, 0), default=0x60)
    p.add_argument("--from-func", action="store_true",
                   help="disassemble from the detected function start")
    p.set_defaults(func=cmd_disasm)

    args = ap.parse_args()
    if not os.path.exists(args.exe):
        print(f"game exe not found: {args.exe}", file=sys.stderr)
        return 2
    exe = Exe.load(args.exe)
    print(
        f"# {args.exe}  imagebase=0x{exe.image_base:08x} "
        f".text=0x{exe.text_va:08x}+0x{exe.text_size:x}"
    )
    args.func(exe, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

    def cstring_at_va(self, va: int) -> str | None:
        off = self.va_to_offset(va)
        if off is None:
            return None
        end = self.data.find(b"\x00", off)
        if end < 0:
            return None
        return self.data[off:end].decode("utf-8", "replace")
