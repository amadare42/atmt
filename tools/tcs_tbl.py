#!/usr/bin/env python3
"""Trails of Cold Steel (Sen1 PC) text table (`t_*.tbl`) parser.

Format (confirmed against SenPatcher's SenLib/Sen1/tbl.cpp):
    u16 entryCount
    repeat entryCount times:
        name      : null-terminated UTF-8 string (the "dataset"/group key)
        dataLen   : u16
        data      : dataLen raw bytes; for most tables this is
                    u16 subIndex (-1 = none) followed by a null-terminated string

Commands:
  list  --file t_name.tbl            print entries
  dump  --root <data/text/dat_us>    dump every table into a JSON file
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

DEFAULT_ROOT = os.environ.get(
    "TOCS_DATA", r"C:\Program Files (x86)\Steam\steamapps\common\Trails of Cold Steel\data"
)

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass


def parse_tbl(data: bytes) -> list[dict]:
    pos = 0
    (count,) = struct.unpack_from("<H", data, pos)
    pos += 2
    entries = []
    for _ in range(count):
        end = data.index(b"\x00", pos)
        name = data[pos:end].decode("utf-8", "replace")
        pos = end + 1
        (dlen,) = struct.unpack_from("<H", data, pos)
        pos += 2
        blob = data[pos : pos + dlen]
        pos += dlen
        sub_index = None
        text = None
        if dlen >= 2:
            (sub_index,) = struct.unpack_from("<h", blob, 0)
            body = blob[2:]
            nul = body.find(b"\x00")
            if nul >= 0:
                body = body[:nul]
            text = body.decode("utf-8", "replace")
        entries.append({"name": name, "sub_index": sub_index, "text": text,
                        "raw_hex": blob.hex()})
    return entries


def cmd_list(args) -> None:
    entries = parse_tbl(open(args.file, "rb").read())
    print(f"{args.file}: {len(entries)} entries")
    for i, e in enumerate(entries[: args.limit]):
        print(f"  [{i:4d}] {e['name']!r} idx={e['sub_index']} -> {e['text']!r}")


def cmd_dump(args) -> None:
    out = {}
    files = sorted(
        os.path.join(args.root, f)
        for f in os.listdir(args.root)
        if f.lower().endswith(".tbl")
    )
    for path in files:
        out[os.path.basename(path)] = parse_tbl(open(path, "rb").read())
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(out, fh, ensure_ascii=False, indent=1)
    tables = ", ".join(f"{k}({len(v)})" for k, v in out.items())
    print(f"wrote {len(out)} tables to {args.out}: {tables}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list")
    p.add_argument("--file", required=True)
    p.add_argument("--limit", type=int, default=40)
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("dump")
    p.add_argument("--root", default=os.path.join(DEFAULT_ROOT, "text", "dat_us"))
    p.add_argument("--out", default="tables.json")
    p.set_defaults(func=cmd_dump)

    args = ap.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
