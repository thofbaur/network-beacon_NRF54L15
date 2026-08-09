#!/usr/bin/env python3
"""Read all "tag_adresses_*.csv" files in this directory and generate
"ids.c" with one known_device_table-style block per tag, using the
physical_id column from the CSV as the assigned id (not consecutive
numbering). The address bytes are written in reversed order, matching the
byte order expected by radio_ids.c's known_device_table.

Duplicate physical_ids and duplicate addresses are reported on the
terminal but do not stop the script; every row is still written to
"ids.c".
"""

import csv
import glob
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

ADDR_COLUMNS = [f"addr{i}" for i in range(1, 7)]

BLOCK_TEMPLATE = """    {{
        .addr = {{
            .type = BT_ADDR_LE_RANDOM,
            .a = {{ .val = {{ {bytes_} }} }}  // {name}
        }},
        .id = {id},
    }},
"""


def read_rows():
    rows = []

    csv_paths = sorted(glob.glob(os.path.join(SCRIPT_DIR, "tag_adresses_*.csv")))
    for csv_path in csv_paths:
        with open(csv_path, newline="") as csv_file:
            reader = csv.DictReader(csv_file)
            for row in reader:
                addr = tuple(row[col].strip().upper() for col in ADDR_COLUMNS)
                physical_id = row["physical_id"].strip()
                rows.append((physical_id, addr, csv_path))

    return rows


def report_duplicates(rows):
    seen_ids = {}
    seen_addrs = {}

    for physical_id, addr, csv_path in rows:
        seen_ids.setdefault(physical_id, []).append(csv_path)
        seen_addrs.setdefault(addr, []).append(csv_path)

    dup_ids = {pid: paths for pid, paths in seen_ids.items() if len(paths) > 1}
    dup_addrs = {addr: paths for addr, paths in seen_addrs.items() if len(paths) > 1}

    if dup_ids:
        print(f"Duplicate physical_id values ({len(dup_ids)}):")
        for pid, paths in dup_ids.items():
            print(f"  id {pid}: {len(paths)} occurrences in {sorted(set(paths))}")

    if dup_addrs:
        print(f"Duplicate addresses ({len(dup_addrs)}):")
        for addr, paths in dup_addrs.items():
            addr_str = ":".join(addr)
            print(f"  {addr_str}: {len(paths)} occurrences in {sorted(set(paths))}")

    if not dup_ids and not dup_addrs:
        print("No duplicate ids or addresses found.")


def format_block(addr, tag_id):
    reversed_bytes = ", ".join(f"0x{byte}" for byte in reversed(addr))
    return BLOCK_TEMPLATE.format(bytes_=reversed_bytes, name="Tag", id=tag_id)


def main():
    rows = read_rows()

    report_duplicates(rows)

    out_path = os.path.join(SCRIPT_DIR, "ids.c")
    with open(out_path, "w") as out_file:
        for physical_id, addr, _csv_path in rows:
            out_file.write(format_block(addr, int(physical_id)))

    print(f"Wrote {len(rows)} entries to {out_path}")


if __name__ == "__main__":
    main()
