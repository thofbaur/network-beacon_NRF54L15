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
DEFAULT_SELF_REPORTS_CSV = "self_reports.csv"
DEFAULT_ECO_SESSIONS_CSV = "eco_sessions.csv"

# Bit layout of the radio/storage fault byte forwarded as "Status: N" (see
# shared/common_include.h and Production_HowTo.md, Part 2). All bits are
# "1 = fault". Bit 6 is only meaningful alongside bit 5 (see DECISIONS.md,
# "Motion Bring-Up Failure Splits Into Two Advertised Bits"): 1 means
# motion_init() never once saw the ADXL367 answer on the I2C bus (still a
# power/timing/wiring question); 0 (with bit 5 still 1) means the chip
# answered fine but bring-up failed some other way afterwards. Bit 7 is
# reserved (always 0 in practice).
ERROR_BIT_LABELS = (
    "RADIO_STATUS_SCAN_ERROR",
    "RADIO_STATUS_NUS_ERROR",
    "STORAGE_STATUS_STORAGE_FULL",
    "STORAGE_STATUS_PARAM_ERROR",
    "STORAGE_STATUS_STORAGE_ERROR",
    "RADIO_STATUS_MOTION_UNAVAILABLE",
    "RADIO_STATUS_MOTION_PROBE_TIMEOUT",
    "RESERVED_BIT7",
)

ID_FIELD_RE = re.compile(r"^\s*ID:\s*(.+?)\s*$")
CURRENT_TIMER_RE = re.compile(r"^Current Timer:\s*(\d+)$")
VOLTAGE_RE = re.compile(r"^Voltage:\s*(\d+)$")
STATUS_RE = re.compile(r"^Status:\s*(\d+)$")
CONTACT_RE = re.compile(r"^ID2:\s*(\S+),\s*Timer:\s*(\d+),\s*RSSI:\s*(-?\d+)$")
SELF_REPORT_RE = re.compile(r"^Self-report time:\s*(\d+)$")
ECO_SESSION_RE = re.compile(r"^Eco Session Enter:\s*(\d+),\s*Leave:\s*(\d+)$")


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


def _timer_to_timestamp(reference_timer: int, reference_timestamp: datetime, timer: int) -> datetime:
    """Resolve a beacon-relative timer value to a real local timestamp.

    Contacts, self-reports, and eco sessions all report past events as timer
    ticks relative to the beacon's own clock; the preceding "Current Timer"
    line ties that clock to a real timestamp.
    """
    return reference_timestamp - timedelta(seconds=reference_timer - timer)


def aggregate_into(
    lines: list[LogLine],
    summaries: dict[str, BeaconSummary],
    current_timer_ref: dict[str, tuple[int, datetime]],
    contacts: list[tuple[str, str, datetime]],
    self_reports: list[tuple[str, datetime]],
    eco_sessions: list[tuple[str, datetime, datetime]],
) -> tuple[int, int, int]:
    """Fold lines into the given (possibly already populated) aggregation state.

    Lets callers process a batch of new lines on top of state carried over
    from earlier batches, instead of rebuilding everything from scratch.
    Returns the number of contact, self-report, and eco session entries
    skipped for lack of a preceding Current Timer reference.
    """
    skipped_contacts = 0
    skipped_self_reports = 0
    skipped_eco_sessions = 0

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
            contact_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, int(timer_str))
            id1, id2 = sorted((line.beacon_id, other_id), key=id_sort_key)
            contacts.append((id1, id2, contact_timestamp))
            continue

        match = SELF_REPORT_RE.match(line.rest)
        if match:
            ref = current_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_self_reports += 1
                continue

            reference_timer, reference_timestamp = ref
            report_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, int(match.group(1)))
            self_reports.append((line.beacon_id, report_timestamp))
            continue

        match = ECO_SESSION_RE.match(line.rest)
        if match:
            ref = current_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_eco_sessions += 1
                continue

            reference_timer, reference_timestamp = ref
            enter_timer, exit_timer = (int(group) for group in match.groups())
            enter_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, enter_timer)
            exit_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, exit_timer)
            eco_sessions.append((line.beacon_id, enter_timestamp, exit_timestamp))
            continue

    return skipped_contacts, skipped_self_reports, skipped_eco_sessions


def aggregate(lines: list[LogLine]):
    summaries: dict[str, BeaconSummary] = {}
    current_timer_ref: dict[str, tuple[int, datetime]] = {}
    contacts: list[tuple[str, str, datetime]] = []
    self_reports: list[tuple[str, datetime]] = []
    eco_sessions: list[tuple[str, datetime, datetime]] = []
    skipped_contacts, skipped_self_reports, skipped_eco_sessions = aggregate_into(
        lines, summaries, current_timer_ref, contacts, self_reports, eco_sessions
    )
    return (
        summaries,
        contacts,
        self_reports,
        eco_sessions,
        skipped_contacts,
        skipped_self_reports,
        skipped_eco_sessions,
    )


def _read_new_lines(path: Path, offset: int) -> tuple[list[str], int]:
    """Read text appended to path since offset, without touching earlier bytes."""
    with path.open("rb") as raw_file:
        raw_file.seek(offset)
        data = raw_file.read()
    text = data.decode("utf-8", errors="replace")
    return text.splitlines(), offset + len(data)


class IncrementalPostProcessor:
    """Rebuilds the summary/contacts CSVs from only newly-appended log bytes.

    Meant to be called repeatedly over the lifetime of a logging session
    (e.g. once per beacon disconnect). Each call re-reads a log file only if
    it grew since the previous call, and folds the new lines on top of
    aggregation state carried over from earlier calls.
    """

    def __init__(
        self,
        log_dir: Path,
        summary_csv: Path,
        contacts_csv: Path,
        self_reports_csv: Path,
        eco_sessions_csv: Path,
    ) -> None:
        self.log_dir = log_dir
        self.summary_csv = summary_csv
        self.contacts_csv = contacts_csv
        self.self_reports_csv = self_reports_csv
        self.eco_sessions_csv = eco_sessions_csv
        self._offsets: dict[Path, int] = {}
        self.summaries: dict[str, BeaconSummary] = {}
        self.current_timer_ref: dict[str, tuple[int, datetime]] = {}
        self.contacts: list[tuple[str, str, datetime]] = []
        self.self_reports: list[tuple[str, datetime]] = []
        self.eco_sessions: list[tuple[str, datetime, datetime]] = []
        self.skipped_contacts = 0
        self.skipped_self_reports = 0
        self.skipped_eco_sessions = 0

    def process(self) -> bool:
        """Process newly-changed log files and rewrite the CSVs if anything changed.

        Returns whether any new lines were found.
        """
        new_lines: list[LogLine] = []
        for path in sorted(self.log_dir.glob("*.log")):
            offset = self._offsets.get(path, 0)
            size = path.stat().st_size
            if size <= offset:
                continue

            raw_lines, new_offset = _read_new_lines(path, offset)
            self._offsets[path] = new_offset
            for raw_line in raw_lines:
                parsed = parse_line(raw_line)
                if parsed is not None:
                    new_lines.append(parsed)

        if not new_lines:
            return False

        new_lines.sort(key=lambda line: line.timestamp)
        skipped_contacts, skipped_self_reports, skipped_eco_sessions = aggregate_into(
            new_lines,
            self.summaries,
            self.current_timer_ref,
            self.contacts,
            self.self_reports,
            self.eco_sessions,
        )
        self.skipped_contacts += skipped_contacts
        self.skipped_self_reports += skipped_self_reports
        self.skipped_eco_sessions += skipped_eco_sessions

        write_summary_csv(self.summary_csv, self.summaries)
        write_contacts_csv(self.contacts_csv, self.contacts)
        write_self_reports_csv(self.self_reports_csv, self.self_reports)
        write_eco_sessions_csv(self.eco_sessions_csv, self.eco_sessions)
        return True


def decode_error_bits(status_byte: Optional[int]) -> list[Optional[int]]:
    if status_byte is None:
        return [None] * len(ERROR_BIT_LABELS)
    return [(status_byte >> bit) & 1 for bit in range(len(ERROR_BIT_LABELS))]


def write_summary_csv(path: Path, summaries: dict[str, BeaconSummary]) -> None:
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(
            ["ID", "Last Seen Local Time", "Last Battery (mV)"]
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
        writer.writerow(["ID1", "ID2", "Contact Local Time"])
        for id1, id2, contact_timestamp in sorted(contacts, key=lambda row: row[2]):
            writer.writerow([id1, id2, contact_timestamp.strftime(TIMESTAMP_FORMAT)])


def write_self_reports_csv(path: Path, self_reports: list[tuple[str, datetime]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["ID", "Local Time"])
        for beacon_id, local_time in sorted(self_reports, key=lambda row: row[1]):
            writer.writerow([beacon_id, local_time.strftime(TIMESTAMP_FORMAT)])


def write_eco_sessions_csv(path: Path, eco_sessions: list[tuple[str, datetime, datetime]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["ID", "Enter Local Time", "Exit Local Time"])
        for beacon_id, enter_time, exit_time in sorted(eco_sessions, key=lambda row: row[1]):
            writer.writerow(
                [beacon_id, enter_time.strftime(TIMESTAMP_FORMAT), exit_time.strftime(TIMESTAMP_FORMAT)]
            )


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
    parser.add_argument(
        "--self-reports-csv",
        type=Path,
        default=None,
        help=f"Output path for the self-report CSV. Default: <log-dir>/{DEFAULT_SELF_REPORTS_CSV}",
    )
    parser.add_argument(
        "--eco-sessions-csv",
        type=Path,
        default=None,
        help=f"Output path for the eco session CSV. Default: <log-dir>/{DEFAULT_ECO_SESSIONS_CSV}",
    )
    args = parser.parse_args(argv)
    if args.summary_csv is None:
        args.summary_csv = args.log_dir / DEFAULT_SUMMARY_CSV
    if args.contacts_csv is None:
        args.contacts_csv = args.log_dir / DEFAULT_CONTACTS_CSV
    if args.self_reports_csv is None:
        args.self_reports_csv = args.log_dir / DEFAULT_SELF_REPORTS_CSV
    if args.eco_sessions_csv is None:
        args.eco_sessions_csv = args.log_dir / DEFAULT_ECO_SESSIONS_CSV
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    log_paths = sorted(args.log_dir.glob("*.log"))
    if not log_paths:
        print(f"No .log files found in {args.log_dir}", file=sys.stderr)
        return 1

    lines = read_log_lines(log_paths)
    (
        summaries,
        contacts,
        self_reports,
        eco_sessions,
        skipped_contacts,
        skipped_self_reports,
        skipped_eco_sessions,
    ) = aggregate(lines)

    write_summary_csv(args.summary_csv, summaries)
    write_contacts_csv(args.contacts_csv, contacts)
    write_self_reports_csv(args.self_reports_csv, self_reports)
    write_eco_sessions_csv(args.eco_sessions_csv, eco_sessions)

    print(f"Processed {len(log_paths)} log file(s), {len(lines)} parsed lines.")
    print(f"Wrote {len(summaries)} beacon summary rows to {args.summary_csv}")
    print(f"Wrote {len(contacts)} contact rows to {args.contacts_csv}")
    print(f"Wrote {len(self_reports)} self-report rows to {args.self_reports_csv}")
    print(f"Wrote {len(eco_sessions)} eco session rows to {args.eco_sessions_csv}")
    if skipped_contacts:
        print(
            f"Skipped {skipped_contacts} contact entries with no preceding "
            "Current Timer reference for that ID."
        )
    if skipped_self_reports:
        print(
            f"Skipped {skipped_self_reports} self-report entries with no preceding "
            "Current Timer reference for that ID."
        )
    if skipped_eco_sessions:
        print(
            f"Skipped {skipped_eco_sessions} eco session entries with no preceding "
            "Current Timer reference for that ID."
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
