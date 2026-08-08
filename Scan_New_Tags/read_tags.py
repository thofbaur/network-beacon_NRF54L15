#!/usr/bin/env python3
"""Read "Device,<name>,<address>" reports from the Scan_New_Tags firmware
over UART and log newly seen devices to a timestamped CSV file.

Requires pyserial: pip install pyserial
"""

import csv
import datetime
import os
import re
import sys

import serial

PORT = "COM11"
BAUDRATE = 115200

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CSV_HEADER = ["name", "addr1", "addr2", "addr3", "addr4", "addr5", "addr6"]

LINE_RE = re.compile(
    r"^Device,(?P<name>[^,]+),(?P<addr>[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})"
)


def main():
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = os.path.join(SCRIPT_DIR, f"tag_adresses_{timestamp}.csv")

    devices = []
    known_addresses = set()

    with open(csv_path, "a", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(CSV_HEADER)
        csv_file.flush()

        with serial.Serial(PORT, BAUDRATE, timeout=1) as ser:
            print(f"Listening on {PORT} at {BAUDRATE} baud, logging to {csv_path}")

            while True:
                raw = ser.readline()
                if not raw:
                    continue

                line = raw.decode("ascii", errors="ignore").strip()
                if not line.startswith("Device,"):
                    continue

                match = LINE_RE.match(line)
                if not match:
                    print(f"Unrecognized line: {line}")
                    continue

                name = match.group("name")
                addr_bytes = tuple(match.group("addr").split(":"))

                if addr_bytes in known_addresses:
                    continue

                known_addresses.add(addr_bytes)
                devices.append({"name": name, "addr": list(addr_bytes)})

                writer.writerow([name, *addr_bytes])
                csv_file.flush()

                print(f"New device: {name} {':'.join(addr_bytes)}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
        sys.exit(0)
