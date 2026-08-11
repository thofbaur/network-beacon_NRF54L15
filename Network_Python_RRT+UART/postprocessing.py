#!/usr/bin/env python3
"""Turn dsa_logger.py's *.log files into per-ID and per-contact CSV summaries."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional

TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S"
DEFAULT_SUMMARY_CSV = "beacon_summary.csv"
DEFAULT_CONTACTS_CSV = "contacts.csv"

# Bit layout of the radio/storage fault byte forwarded as "Status: N" (see
# shared/common_include.h and Production_HowTo.md, Part 2). All bits are
# "1 = fault". Bits 6-7 are reserved (always 0 in practice).
ERROR_BIT_LABELS = (
    "RADIO_STATUS_SCAN_ERROR",
    "RADIO_STATUS_NUS_ERROR",
    "STORAGE_STATUS_STORAGE_FULL",
    "STORAGE_STATUS_PARAM_ERROR",
    "STORAGE_STATUS_STORAGE_ERROR",
    "RADIO_STATUS_MOTION_UNAVAILABLE",
    "RESERVED_BIT6",
    "RESERVED_BIT7",
)

ID_FIELD_RE = re.compile(r"^\s*ID:\s*(.+?)\s*$")
CURRENT_TIMER_RE = re.compile(r"^Current Timer:\s*(\d+)$")
VOLTAGE_RE = re.compile(r"^Voltage:\s*(\d+)$")
STATUS_RE = re.compile(r"^Status:\s*(\d+)$")
CONTACT_RE = re.compile(r"^ID2:\s*(\S+),\s*Timer:\s*(\d+),\s*RSSI:\s*(-?\d+)$")


@dataclass(frozen=True)
class LogLine:
    timestamp: datetime
    beacon_id: str
    rest: str


@dataclass
class BeaconSummary:
    beacon_id: str
    last_seen: Optional[datetime] = None
    last_voltage_mv: Optional[int] = None
    last_status_byte: Optional[int] = None


def id_sort_key(beacon_id: str):
    try:
        return (0, int(beacon_id))
    except ValueError:
        return (1, beacon_id)


def parse_line(raw_line: str) -> Optional[LogLine]:
    parts = raw_line.rstrip("\r\n").split(",", 2)
    if len(parts) < 3:
        return None

    timestamp_str, id_field, rest = parts
    try:
        timestamp = datetime.strptime(timestamp_str.strip(), TIMESTAMP_FORMAT)
    except ValueError:
        return None

    id_match = ID_FIELD_RE.match(id_field)
    if not id_match:
        return None

    return LogLine(timestamp, id_match.group(1), rest.strip())


def read_log_lines(log_paths: list[Path]) -> list[LogLine]:
    lines: list[LogLine] = []
    for path in log_paths:
        with path.open(encoding="utf-8", errors="replace") as log_file:
            for raw_line in log_file:
                parsed = parse_line(raw_line)
                if parsed is not None:
                    lines.append(parsed)

    # Stable sort: files are chronological internally, so ties (lines sharing
    # the same second, e.g. one connect burst) keep their original order -
    # which matters because a "Current Timer" line must stay ahead of the
    # data-set lines that reference it.
    lines.sort(key=lambda line: line.timestamp)
    return lines


def aggregate(lines: list[LogLine]):
    summaries: dict[str, BeaconSummary] = {}
    current_timer_ref: dict[str, tuple[int, datetime]] = {}
    contacts: list[tuple[str, str, datetime]] = []
    skipped_contacts = 0

    for line in lines:
        summary = summaries.setdefault(line.beacon_id, BeaconSummary(line.beacon_id))
        summary.last_seen = line.timestamp

        match = CURRENT_TIMER_RE.match(line.rest)
        if match:
            current_timer_ref[line.beacon_id] = (int(match.group(1)), line.timestamp)
            continue

        match = VOLTAGE_RE.match(line.rest)
        if match:
            summary.last_voltage_mv = int(match.group(1))
            continue

        match = STATUS_RE.match(line.rest)
        if match:
            summary.last_status_byte = int(match.group(1))
            continue

        match = CONTACT_RE.match(line.rest)
        if match:
            other_id, timer_str, _rssi = match.groups()
            ref = current_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_contacts += 1
                continue

            reference_timer, reference_timestamp = ref
            contact_timestamp = reference_timestamp - timedelta(
                seconds=reference_timer - int(timer_str)
            )
            id1, id2 = sorted((line.beacon_id, other_id), key=id_sort_key)
            contacts.append((id1, id2, contact_timestamp))

    return summaries, contacts, skipped_contacts


def decode_error_bits(status_byte: Optional[int]) -> list[Optional[int]]:
    if status_byte is None:
        return [None] * len(ERROR_BIT_LABELS)
    return [(status_byte >> bit) & 1 for bit in range(len(ERROR_BIT_LABELS))]


def write_summary_csv(path: Path, summaries: dict[str, BeaconSummary]) -> None:
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(
            ["ID", "Last Seen", "Last Battery (mV)"]
            + [f"Error: {label}" for label in ERROR_BIT_LABELS]
        )
        for beacon_id in sorted(summaries, key=id_sort_key):
            summary = summaries[beacon_id]
            last_seen = summary.last_seen.strftime(TIMESTAMP_FORMAT) if summary.last_seen else ""
            writer.writerow(
                [beacon_id, last_seen, summary.last_voltage_mv]
                + decode_error_bits(summary.last_status_byte)
            )


def write_contacts_csv(path: Path, contacts: list[tuple[str, str, datetime]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["ID1", "ID2", "Contact"])
        for id1, id2, contact_timestamp in sorted(contacts, key=lambda row: row[2]):
            writer.writerow([id1, id2, contact_timestamp.strftime(TIMESTAMP_FORMAT)])


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Post-process dsa_logger.py *.log files into per-ID and per-contact CSVs."
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="Directory containing *.log files. Default: this script's directory.",
    )
    parser.add_argument(
        "--summary-csv",
        type=Path,
        default=None,
        help=f"Output path for the per-ID summary CSV. Default: <log-dir>/{DEFAULT_SUMMARY_CSV}",
    )
    parser.add_argument(
        "--contacts-csv",
        type=Path,
        default=None,
        help=f"Output path for the contacts CSV. Default: <log-dir>/{DEFAULT_CONTACTS_CSV}",
    )
    args = parser.parse_args(argv)
    if args.summary_csv is None:
        args.summary_csv = args.log_dir / DEFAULT_SUMMARY_CSV
    if args.contacts_csv is None:
        args.contacts_csv = args.log_dir / DEFAULT_CONTACTS_CSV
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    log_paths = sorted(args.log_dir.glob("*.log"))
    if not log_paths:
        print(f"No .log files found in {args.log_dir}", file=sys.stderr)
        return 1

    lines = read_log_lines(log_paths)
    summaries, contacts, skipped_contacts = aggregate(lines)

    write_summary_csv(args.summary_csv, summaries)
    write_contacts_csv(args.contacts_csv, contacts)

    print(f"Processed {len(log_paths)} log file(s), {len(lines)} parsed lines.")
    print(f"Wrote {len(summaries)} beacon summary rows to {args.summary_csv}")
    print(f"Wrote {len(contacts)} contact rows to {args.contacts_csv}")
    if skipped_contacts:
        print(
            f"Skipped {skipped_contacts} contact entries with no preceding "
            "Current Timer reference for that ID."
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
