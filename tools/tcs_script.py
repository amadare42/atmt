#!/usr/bin/env python3
"""Trails of Cold Steel (Sen1 PC) script `.dat` dialog extraction.

The compiled scripts in `data/scripts/{scena,talk,book,ani,battle,ui}/dat_us/*.dat`
contain the *localized* text inline. Dialog text is stored as a run of bytes that
starts after a control byte (< 0x20) and ends at the next control byte; inside the
run, inline `#` control codes appear (e.g. `#K`, `#E6`, `#M`, `#A`, `#x[...]`).
This is derived from TwnKey's SenScriptsDecompiler (`reading_dialog()` in
`headers/CS1InstructionsSet.h`).

Commands:
  context   : dump raw bytes around a chosen offset (for reverse engineering)
  extract   : print all dialog lines of one script file
  scan      : walk game script folders and build a JSONL catalog
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

DEFAULT_ROOT = os.environ.get(
    "TOCS_DATA", r"C:\Program Files (x86)\Steam\steamapps\common\Trails of Cold Steel\data"
)

try:  # game text is UTF-8; make console output robust on non-UTF8 codepages
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

# A dialog operand is a run of bytes >= 0x20 (plus inline #-codes); it never
# contains a control byte. Function/variable names in scripts are also >= 0x20,
# so we additionally require the run to look like natural language.
MIN_RUN = 12
_CODE_RE = re.compile(rb"#(?:[A-Za-z](?:\[[^\]]*\])?|\d+)")
_CODE_RE_STR = re.compile(r"#(?:[A-Za-z](?:\[[^\]]*\])?|\d+)")
# characters that appear in localized game text
_TEXTY = set(b"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,!?'\"#-_:;()[]&%/<>+=*~")


def looks_like_dialog(run: bytes) -> bool:
    """Heuristic gate: is this byte run natural language rather than a data blob?"""
    if len(run) < MIN_RUN:
        return False
    letters = sum(1 for b in run if 65 <= b <= 90 or 97 <= b <= 122)
    if letters < 6:
        return False
    texty = sum(1 for b in run if b in _TEXTY)
    if texty / len(run) < 0.85:
        return False
    if _CODE_RE.search(run) or b" " in run:
        return True
    # single word lines are rare; accept only with sentence punctuation
    return bool(re.search(rb"[.!?]\s*$", run))


# Short lines are real dialog too - "..." (a silent page) and "#1P*yawn*" are both message
# operands - but the length gate above exists to keep function/label names out, so they are
# accepted only when the stream *marks* them as operands.
MIN_RUN_SHORT = 3        # "..." is three bytes
MIN_TEXT_SHORT = 3       # ...and its readable text at least this long (drops "#E0#M0")
_SHORT_PUNCT = re.compile(rb"[.!?,;:'\"*~_-]")


def is_message_operand(data: bytes, start: int) -> bool:
    """True when the run at `start` is a message text operand.

    The script stream marks a text operand with `11 <u32>` immediately before it (measured in
    the running game: `11 fe 20 00 00` + "...You are my classmate..."), and the byte before
    that header is itself a control byte. That marker is what makes short dialog safe to
    index without dragging in the names the length gate is there for.
    """
    if start < 5:
        return False
    if data[start - 5] != 0x11:
        return False
    return start < 6 or data[start - 6] < 0x20


def looks_like_short_dialog(data: bytes, start: int, run: bytes) -> bool:
    """A run that is too short for looks_like_dialog but is a marked message operand."""
    if len(run) < MIN_RUN_SHORT:
        return False
    if not is_message_operand(data, start):
        return False
    if len(plain(run)) < MIN_TEXT_SHORT:
        return False          # only codes / nothing readable
    # Same idea as the long-run rule ("single word lines are rare; accept only with sentence
    # punctuation"): require a space or punctuation, which drops the script names and labels
    # that happen to sit behind a `11` byte ("c0500", "Init", "Celine") while keeping the
    # real short pages ("...", "*yawn*", "Umm...", "Oh, Emma!").
    if b" " not in run and not _SHORT_PUNCT.search(run):
        return False
    # nothing that could be a data blob: every byte has to be text-ish (0x01 = line break)
    return all(b in _TEXTY or b == 0x01 for b in run)


def find_dialog_runs(data: bytes) -> list[tuple[int, bytes]]:
    runs: list[tuple[int, bytes]] = []
    start = None
    for i, b in enumerate(data):
        if b >= 0x20:
            if start is None:
                start = i
        else:
            if start is not None:
                run = data[start:i]
                if looks_like_dialog(run) or looks_like_short_dialog(data, start, run):
                    runs.append((start, run))
                start = None
    if start is not None:
        run = data[start:]
        if looks_like_dialog(run) or looks_like_short_dialog(data, start, run):
            runs.append((start, run))
    return runs


def pretty(text: bytes) -> str:
    """Human readable version: keep #-codes, escape newlines."""
    s = text.decode("utf-8", "replace")
    return s.replace("\r\n", "\\n").replace("\n", "\\n")


def _strip_codes_str(s: str) -> str:
    """Strip the inline #-codes (what the player reads)."""
    out: list[str] = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "#":
            j = i + 1
            # `#<digits><Letter>` is a single code (#0T, #1P, #5S), so the first letter after
            # the digits still belongs to it. Without this, "#1P*yawn*" became "P*yawn*" and
            # "#5SDad?!" became "PDad?!".
            digits = j < n and s[j].isdigit()
            while j < n and s[j].isalnum():
                if j > i + 1 and s[j].isupper():
                    nxt = s[j + 1] if j + 1 < n else ""
                    if nxt == "" or nxt.islower() or not nxt.isalnum():
                        if not (digits and j == i + 2):
                            break  # this letter starts a word
                j += 1
            if j < n and s[j] == "[":
                depth = 0
                while j < n:
                    if s[j] == "[":
                        depth += 1
                    elif s[j] == "]":
                        depth -= 1
                        if depth == 0:
                            j += 1
                            break
                    j += 1
            i = j
            continue
        out.append(" " if c in "\r\n" else c)
        i += 1
    return re.sub(r"\s+", " ", "".join(out)).strip()


def plain(text: bytes) -> str:
    """Readable text with inline control codes removed."""
    s = text.decode("utf-8", "replace") if isinstance(text, (bytes, bytearray)) else text
    return _strip_codes_str(s)


def cmd_context(args) -> None:
    data = open(args.file, "rb").read()
    off = args.offset
    lo = max(0, off - args.before)
    hi = min(len(data), off + args.after)
    chunk = data[lo:hi]
    for i in range(0, len(chunk), 16):
        row = chunk[i : i + 16]
        va = lo + i
        hexs = " ".join(f"{b:02x}" for b in row)
        asc = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in row)
        mark = " <" if lo + i <= off < lo + i + 16 else "  "
        print(f"{va:08x}{mark} {hexs:<48} {asc}")

def cmd_extract(args) -> None:
    data = open(args.file, "rb").read()
    runs = find_dialog_runs(data)
    if args.json:
        for off, run in runs:
            print(
                json.dumps(
                    {
                        "offset": off,
                        "raw": run.decode("utf-8", "replace"),
                        "text": plain(run),
                    },
                    ensure_ascii=False,
                )
            )
    else:
        print(f"{args.file}: {len(runs)} dialog lines")
        for off, run in runs[: args.limit]:
            print(f"  0x{off:06x}  {pretty(run)}")






def iter_script_files(root: str, categories: list[str] | None) -> list[str]:
    base = os.path.join(root, "scripts")
    out = []
    for dirpath, _dirnames, filenames in os.walk(base):
        cat = os.path.basename(os.path.dirname(dirpath))
        if categories and cat not in categories:
            continue
        for fn in filenames:
            if fn.lower().endswith(".dat"):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def cmd_scan(args) -> None:
    files = iter_script_files(args.root, args.categories)
    total = 0
    with open(args.out, "w", encoding="utf-8") as fh:
        for path in files:
            data = open(path, "rb").read()
            rel = os.path.relpath(path, args.root).replace("\\", "/")
            for off, run in find_dialog_runs(data):
                total += 1
                fh.write(
                    json.dumps(
                        {
                            "script": rel,
                            "offset": off,
                            "raw": run.decode("utf-8", "replace"),
                            "text": plain(run),
                        },
                        ensure_ascii=False,
                    )
                    + "\n"
                )
    print(f"scanned {len(files)} files, wrote {total} dialog lines to {args.out}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("context", help="hexdump around an offset")
    p.add_argument("--file", required=True)
    p.add_argument("--offset", type=lambda x: int(x, 0), required=True)
    p.add_argument("--before", type=int, default=48)
    p.add_argument("--after", type=int, default=160)
    p.set_defaults(func=cmd_context)

    p = sub.add_parser("extract", help="extract dialog from one script")
    p.add_argument("--file", required=True)
    p.add_argument("--json", action="store_true")
    p.add_argument("--limit", type=int, default=40)
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser("scan", help="build a JSONL catalog for all scripts")
    p.add_argument("--root", default=DEFAULT_ROOT)
    p.add_argument("--out", default="catalog.jsonl")
    p.add_argument("--categories", nargs="*", default=None,
                   help="restrict to e.g. scena talk book")
    p.set_defaults(func=cmd_scan)

    args = ap.parse_args()
    args.func(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

def cmd_extract(args) -> None:
    data = open(args.file, "rb").read()
    runs = find_dialog_runs(data)
    if args.json:
        for off, run in runs:
            print(json.dumps({"offset": off, "raw": run.decode("utf-8", "replace"),
                              "text": plain(run)}, ensure_ascii=False))
    else:
        print(f"{args.file}: {len(runs)} dialog lines")
        for off, run in runs[: args.limit]:
            print(f"  0x{off:06x}  {pretty(run)}")
