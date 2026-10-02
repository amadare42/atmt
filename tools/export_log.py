#!/usr/bin/env python3
"""Turn a recorded atmt_dialogs.jsonl into something you can read or browse.

  python tools/export_log.py --log "<game folder>\\atmt_dialogs.jsonl" \
      --out log.md
  python tools/export_log.py --log ... --out log.html --html

The markdown/html output keeps the screenshots (relative paths) so you can both
read the text and flip through the images of each line.
"""

from __future__ import annotations

import argparse
import html
import json
import sys
from collections import OrderedDict

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass


def load(log_path: str, dedupe: bool) -> list[dict]:
    rows: list[dict] = []
    seen = set()
    with open(log_path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "text" not in rec:
                continue
            if dedupe:
                key = rec.get("text", "")
                if key in seen:
                    continue
                seen.add(key)
            rows.append(rec)
    return rows


def to_markdown(rows: list[dict], with_images: bool) -> str:
    out = ["# Trails of Cold Steel - dialog log", ""]
    for rec in rows:
        speaker = rec.get("speaker") or "?"
        text = rec.get("text", "")
        out.append(f"**{speaker}:** {text}  ")
        meta = f"`{rec.get('script','')}` @ 0x{rec.get('offset',0):x}"
        if rec.get("time"):
            meta += f" - {rec['time']}"
        out.append(f"<sub>{meta}</sub>")
        if with_images and rec.get("image"):
            out.append(f"\n![{text[:40]}]({rec['image']})\n")
        out.append("")
    return "\n".join(out)


def to_html(rows: list[dict], with_images: bool) -> str:
    parts = ["<!doctype html><meta charset='utf-8'>",
             "<title>Trails of Cold Steel dialog log</title>",
             "<style>body{background:#14161a;color:#e8e8e8;font:15px/1.5 'Segoe UI',sans-serif;"
             "max-width:900px;margin:2rem auto}h2{font-size:16px;margin:1.4rem 0 .2rem}"
             ".sp{color:#7fd1ff}.meta{color:#777;font-size:12px}"
             "img{max-width:100%;border:1px solid #2a2f36;border-radius:6px;margin:.4rem 0}"
             "table{border-collapse:collapse}td{padding:.15rem .6rem .15rem 0;vertical-align:top}"
             "</style>"]
    for rec in rows:
        speaker = html.escape(rec.get("speaker") or "?")
        text = html.escape(rec.get("text", ""))
        meta = f"{html.escape(rec.get('script',''))} @ 0x{rec.get('offset',0):x}"
        parts.append(f"<h2><span class='sp'>{speaker}</span></h2><div>{text}</div>")
        parts.append(f"<div class='meta'>{meta}</div>")
        if with_images and rec.get("image"):
            parts.append(f"<img src='{html.escape(rec['image'])}' alt=''>")
    return "\n".join(parts)


def to_plain(rows: list[dict]) -> str:
    return "\n".join(
        f"{rec.get('speaker') or '?'}: {rec.get('text','')}" for rec in rows
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", required=True)
    ap.add_argument("--out", default="dialog_log.md")
    ap.add_argument("--format", default="md", choices=["md", "html", "txt"])
    ap.add_argument("--dedupe", action="store_true", help="drop repeated lines")
    ap.add_argument("--with-images", action="store_true", help="include screenshots")
    ap.add_argument("--stats", action="store_true")
    args = ap.parse_args()

    rows = load(args.log, args.dedupe)
    if args.stats:
        by_speaker: dict[str, int] = OrderedDict()
        for rec in rows:
            key = rec.get("speaker") or "(unknown)"
            by_speaker[key] = by_speaker.get(key, 0) + 1
        print(f"{len(rows)} lines")
        for k, v in sorted(by_speaker.items(), key=lambda kv: -kv[1]):
            print(f"  {v:5d}  {k}")
    if args.format == "txt":
        text = to_plain(rows)
    elif args.format == "html":
        text = to_html(rows, args.with_images)
    else:
        text = to_markdown(rows, args.with_images)
    with open(args.out, "w", encoding="utf-8") as fh:
        fh.write(text)
    print(f"wrote {args.out} ({len(rows)} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
