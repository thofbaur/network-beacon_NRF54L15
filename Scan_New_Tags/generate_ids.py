#!/usr/bin/env python3
"""Read all "tag_adresses_*.csv" files in this directory, drop duplicate
addresses, and generate "ids.c" with one known_device_table-style block per
unique tag. The address bytes are written in reversed order, matching the
byte order expected by radio_ids.c's known_device_table.
"""

import argparse
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


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--start-id",
        type=int,
        default=1,
        help="ID assigned to the first entry, incrementing by 1 after that (default: 1)",
    )
    return parser.parse_args()


def read_addresses():
    addresses = []
    seen_addresses = set()

    csv_paths = sorted(glob.glob(os.path.join(SCRIPT_DIR, "tag_adresses_*.csv")))
    for csv_path in csv_paths:
        with open(csv_path, newline="") as csv_file:
            reader = csv.DictReader(csv_file)
            for row in reader:
                addr = tuple(row[col].strip().upper() for col in ADDR_COLUMNS)

                if addr in seen_addresses:
                    continue
                seen_addresses.add(addr)

                addresses.append(addr)

    return addresses


def format_block(addr, tag_id):
    reversed_bytes = ", ".join(f"0x{byte}" for byte in reversed(addr))
    return BLOCK_TEMPLATE.format(bytes_=reversed_bytes, name="Tag", id=tag_id)


def main():
    args = parse_args()
    addresses = read_addresses()

    out_path = os.path.join(SCRIPT_DIR, "ids.c")
    with open(out_path, "w") as out_file:
        for offset, addr in enumerate(addresses):
            out_file.write(format_block(addr, args.start_id + offset))

    print(f"Wrote {len(addresses)} unique entries to {out_path}")


if __name__ == "__main__":
    main()
