#!/usr/bin/env python3
"""Replace "ID: <ID1>," with "ID: <ID2>," within a line-number range of a log file.

The range is (L1 - 3) through (L2 - 4) inclusive, not L1 through L2 directly -
the caller passes the line numbers of the two mismatched contact lines from
transfer_mislabel.md, and the fix has to reach back to the "ID:" header lines
that precede them.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            'Replace "ID: <ID1>," with "ID: <ID2>," on every line between '
            "(L1 - 3) and (L2 - 4) of a log file, in place."
        )
    )
    parser.add_argument("file", type=Path, help="Log file to modify in place.")
    parser.add_argument("l1", type=int, help="L1. The replaced range starts at line L1 - 3.")
    parser.add_argument("l2", type=int, help="L2. The replaced range ends at line L2 - 4.")
    parser.add_argument("id1", help="Beacon ID to replace.")
    parser.add_argument("id2", help="Beacon ID to replace it with.")
    return parser.parse_args(argv)


def relabel_id_range(path: Path, start_line: int, end_line: int, id1: str, id2: str) -> int:
    """Replace "ID: <id1>," with "ID: <id2>," on lines [start_line, end_line] of path.

    Returns the number of occurrences replaced.
    """
    old_text = f"ID: {id1},"
    new_text = f"ID: {id2},"

    lines = path.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True)

    replaced = 0
    for index, line in enumerate(lines, start=1):
        if start_line <= index <= end_line and old_text in line:
            replaced += line.count(old_text)
            lines[index - 1] = line.replace(old_text, new_text)

    path.write_text("".join(lines), encoding="utf-8")
    return replaced


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    start_line = args.l1 - 3
    end_line = args.l2 - 4
    if start_line > end_line:
        print(
            f"Range is empty: L1 - 3 = {start_line} is after L2 - 4 = {end_line}.",
            file=sys.stderr,
        )
        return 1

    replaced = relabel_id_range(args.file, start_line, end_line, args.id1, args.id2)
    print(
        f'Replaced {replaced} occurrence(s) of "ID: {args.id1}," with "ID: {args.id2}," '
        f"on lines {start_line}-{end_line} of {args.file}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
