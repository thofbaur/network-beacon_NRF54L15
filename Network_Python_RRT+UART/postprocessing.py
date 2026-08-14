#!/usr/bin/env python3
"""Turn dsa_logger.py's *.log files into per-ID and per-contact CSV summaries."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from datetime import date, datetime, timedelta
from pathlib import Path
from typing import Optional

TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S"
DEFAULT_SUMMARY_CSV = "beacon_summary.csv"
DEFAULT_CONTACTS_CSV = "contacts.csv"
DEFAULT_SELF_REPORTS_CSV = "self_reports.csv"
DEFAULT_ECO_SESSIONS_CSV = "eco_sessions.csv"

# Sanity bounds for this deployment. A beacon occasionally dumps a handful of
# corrupted trailing records (stale/uninitialized flash bytes read past its
# real stored contact count) - these show up as contacts with a wildly
# out-of-range timer (resolving to a timestamp months away) and/or an ID
# that isn't part of the fielded roster. See contacts_20261008.csv, traced
# to a beacon 44<->71 record with Timer: 4801652 that resolved to 2026-10-08.
DEFAULT_VALID_ID_MIN = 1
DEFAULT_VALID_ID_MAX = 170
DEFAULT_VALID_EXTRA_IDS = frozenset({252, 253, 254})
DEFAULT_VALID_DATE_START = date(2026, 8, 13)
DEFAULT_VALID_DATE_END = date(2026, 8, 29)
DEFAULT_VALID_RSSI_MIN = -110
DEFAULT_VALID_RSSI_MAX = -20

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


def is_valid_beacon_id(
    beacon_id: str,
    id_min: int = DEFAULT_VALID_ID_MIN,
    id_max: int = DEFAULT_VALID_ID_MAX,
    extra_ids: frozenset[int] = DEFAULT_VALID_EXTRA_IDS,
) -> bool:
    """Whether beacon_id is part of this deployment's fielded device roster."""
    try:
        id_int = int(beacon_id)
    except ValueError:
        return False
    return id_min <= id_int <= id_max or id_int in extra_ids


def is_valid_rssi(
    rssi: int,
    rssi_min: int = DEFAULT_VALID_RSSI_MIN,
    rssi_max: int = DEFAULT_VALID_RSSI_MAX,
) -> bool:
    """Whether rssi (dBm, negative) falls within the radio's plausible range."""
    return rssi_min <= rssi <= rssi_max


def is_within_valid_span(timestamp: datetime, valid_date_start: date, valid_date_end: date) -> bool:
    """Whether timestamp's calendar date falls within [valid_date_start, valid_date_end]."""
    return valid_date_start <= timestamp.date() <= valid_date_end


def is_plausible_event_timestamp(
    timestamp: datetime,
    reference_timestamp: datetime,
    valid_date_start: date,
    valid_date_end: date,
) -> bool:
    """Whether a timer-resolved past event's timestamp is plausible.

    A contact/self-report/eco-session is always resolved relative to a
    "Current Timer" reference, which stands for "now" for that record - so
    the event can never be later than reference_timestamp. Corrupted timer
    values that undershoot the reference only slightly can still land inside
    the valid date span by chance, so both checks are needed.
    """
    return timestamp <= reference_timestamp and is_within_valid_span(
        timestamp, valid_date_start, valid_date_end
    )


def aggregate_into(
    lines: list[LogLine],
    summaries: dict[str, BeaconSummary],
    current_timer_ref: dict[str, tuple[int, datetime]],
    contacts: list[tuple[str, str, int, datetime]],
    self_reports: list[tuple[str, datetime]],
    eco_sessions: list[tuple[str, datetime, datetime]],
    valid_date_start: date = DEFAULT_VALID_DATE_START,
    valid_date_end: date = DEFAULT_VALID_DATE_END,
) -> tuple[int, int, int, int]:
    """Fold lines into the given (possibly already populated) aggregation state.

    Lets callers process a batch of new lines on top of state carried over
    from earlier batches, instead of rebuilding everything from scratch.
    Returns the number of contact, self-report, and eco session entries
    skipped for lack of a preceding Current Timer reference, plus the number
    of entries dropped as corrupted: an out-of-roster beacon ID, an
    out-of-range RSSI, or a timer-resolved timestamp outside
    [valid_date_start, valid_date_end].
    """
    skipped_contacts = 0
    skipped_self_reports = 0
    skipped_eco_sessions = 0
    skipped_invalid = 0

    for line in lines:
        if not is_valid_beacon_id(line.beacon_id):
            skipped_invalid += 1
            continue

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
            other_id, timer_str, rssi_str = match.groups()
            rssi = int(rssi_str)
            if not is_valid_beacon_id(other_id) or not is_valid_rssi(rssi):
                skipped_invalid += 1
                continue

            ref = current_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_contacts += 1
                continue

            reference_timer, reference_timestamp = ref
            contact_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, int(timer_str))
            if not is_plausible_event_timestamp(
                contact_timestamp, reference_timestamp, valid_date_start, valid_date_end
            ):
                skipped_invalid += 1
                continue

            id1, id2 = sorted((line.beacon_id, other_id), key=id_sort_key)
            contacts.append((id1, id2, rssi, contact_timestamp))
            continue

        match = SELF_REPORT_RE.match(line.rest)
        if match:
            ref = current_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_self_reports += 1
                continue

            reference_timer, reference_timestamp = ref
            report_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, int(match.group(1)))
            if not is_plausible_event_timestamp(
                report_timestamp, reference_timestamp, valid_date_start, valid_date_end
            ):
                skipped_invalid += 1
                continue

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
            if not (
                is_plausible_event_timestamp(enter_timestamp, reference_timestamp, valid_date_start, valid_date_end)
                and is_plausible_event_timestamp(
                    exit_timestamp, reference_timestamp, valid_date_start, valid_date_end
                )
            ):
                skipped_invalid += 1
                continue

            eco_sessions.append((line.beacon_id, enter_timestamp, exit_timestamp))
            continue

    return skipped_contacts, skipped_self_reports, skipped_eco_sessions, skipped_invalid


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
        valid_date_start: date = DEFAULT_VALID_DATE_START,
        valid_date_end: date = DEFAULT_VALID_DATE_END,
    ) -> None:
        self.log_dir = log_dir
        self.summary_csv = summary_csv
        self.contacts_csv = contacts_csv
        self.self_reports_csv = self_reports_csv
        self.eco_sessions_csv = eco_sessions_csv
        self.valid_date_start = valid_date_start
        self.valid_date_end = valid_date_end
        self._offsets: dict[Path, int] = {}
        self.summaries: dict[str, BeaconSummary] = {}
        self.current_timer_ref: dict[str, tuple[int, datetime]] = {}
        self.contacts: list[tuple[str, str, int, datetime]] = []
        self.self_reports: list[tuple[str, datetime]] = []
        self.eco_sessions: list[tuple[str, datetime, datetime]] = []
        self.skipped_contacts = 0
        self.skipped_self_reports = 0
        self.skipped_eco_sessions = 0
        self.skipped_invalid = 0

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
        skipped_contacts, skipped_self_reports, skipped_eco_sessions, skipped_invalid = aggregate_into(
            new_lines,
            self.summaries,
            self.current_timer_ref,
            self.contacts,
            self.self_reports,
            self.eco_sessions,
            self.valid_date_start,
            self.valid_date_end,
        )
        self.skipped_contacts += skipped_contacts
        self.skipped_self_reports += skipped_self_reports
        self.skipped_eco_sessions += skipped_eco_sessions
        self.skipped_invalid += skipped_invalid

        write_summary_csv(self.summary_csv, self.summaries)
        write_contacts_csv(self.contacts_csv, self.contacts)
        write_self_reports_csv(self.self_reports_csv, self.self_reports)
        write_eco_sessions_csv(self.eco_sessions_csv, self.eco_sessions)
        return True


def decode_error_bits(status_byte: Optional[int]) -> list[Optional[int]]:
    if status_byte is None:
        return [None] * len(ERROR_BIT_LABELS)
    return [(status_byte >> bit) & 1 for bit in range(len(ERROR_BIT_LABELS))]


def encode_error_bits(bit_values: list[str]) -> Optional[int]:
    """Inverse of decode_error_bits, for reading a previously-written summary CSV back in."""
    if all(not value for value in bit_values):
        return None
    return sum(1 << bit for bit, value in enumerate(bit_values) if value == "1")


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


def read_summary_csv(path: Path) -> dict[str, BeaconSummary]:
    """Read a previously-written summary CSV back into BeaconSummary state.

    Lets a run seed itself from prior output so beacons/log files that have
    since been archived away aren't dropped from the summary.
    """
    summaries: dict[str, BeaconSummary] = {}
    if not path.exists():
        return summaries

    with path.open(encoding="utf-8", newline="") as csv_file:
        for row in csv.DictReader(csv_file):
            beacon_id = row["ID"]
            last_seen = (
                datetime.strptime(row["Last Seen Local Time"], TIMESTAMP_FORMAT)
                if row.get("Last Seen Local Time")
                else None
            )
            last_voltage_mv = int(row["Last Battery (mV)"]) if row.get("Last Battery (mV)") else None
            last_status_byte = encode_error_bits(
                [row.get(f"Error: {label}", "") for label in ERROR_BIT_LABELS]
            )
            summaries[beacon_id] = BeaconSummary(beacon_id, last_seen, last_voltage_mv, last_status_byte)
    return summaries


def contacts_csv_path_for_day(base_path: Path, day: date) -> Path:
    """Derive the per-day contacts CSV path from the base --contacts-csv path.

    e.g. base "contacts.csv" + 2026-08-14 -> "contacts_20260814.csv" in the
    same directory.
    """
    return base_path.with_name(f"{base_path.stem}_{day.strftime('%Y%m%d')}{base_path.suffix}")


def write_contacts_csv(base_path: Path, contacts: list[tuple[str, str, int, datetime]]) -> set[Path]:
    """Write one contacts CSV per calendar day of contact_timestamp.

    Returns the set of paths written.
    """
    by_day: dict[date, list[tuple[str, str, int, datetime]]] = {}
    for contact in contacts:
        by_day.setdefault(contact[3].date(), []).append(contact)

    written_paths: set[Path] = set()
    for day, day_contacts in by_day.items():
        path = contacts_csv_path_for_day(base_path, day)
        written_paths.add(path)
        with path.open("w", encoding="utf-8", newline="") as csv_file:
            writer = csv.writer(csv_file)
            writer.writerow(["ID1", "ID2", "RSSI", "Contact Local Time"])
            for id1, id2, rssi, contact_timestamp in sorted(day_contacts, key=lambda row: row[3]):
                writer.writerow([id1, id2, rssi, contact_timestamp.strftime(TIMESTAMP_FORMAT)])
    return written_paths


def write_self_reports_csv(path: Path, self_reports: list[tuple[str, datetime]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["ID", "Local Time"])
        for beacon_id, local_time in sorted(self_reports, key=lambda row: row[1]):
            writer.writerow([beacon_id, local_time.strftime(TIMESTAMP_FORMAT)])


def read_self_reports_csv(path: Path) -> list[tuple[str, datetime]]:
    """Read a previously-written self-reports CSV back in, to merge with on top of."""
    if not path.exists():
        return []

    with path.open(encoding="utf-8", newline="") as csv_file:
        return [
            (row["ID"], datetime.strptime(row["Local Time"], TIMESTAMP_FORMAT))
            for row in csv.DictReader(csv_file)
        ]


def write_eco_sessions_csv(path: Path, eco_sessions: list[tuple[str, datetime, datetime]]) -> None:
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["ID", "Enter Local Time", "Exit Local Time"])
        for beacon_id, enter_time, exit_time in sorted(eco_sessions, key=lambda row: row[1]):
            writer.writerow(
                [beacon_id, enter_time.strftime(TIMESTAMP_FORMAT), exit_time.strftime(TIMESTAMP_FORMAT)]
            )


def read_eco_sessions_csv(path: Path) -> list[tuple[str, datetime, datetime]]:
    """Read a previously-written eco-sessions CSV back in, to merge with on top of."""
    if not path.exists():
        return []

    with path.open(encoding="utf-8", newline="") as csv_file:
        return [
            (
                row["ID"],
                datetime.strptime(row["Enter Local Time"], TIMESTAMP_FORMAT),
                datetime.strptime(row["Exit Local Time"], TIMESTAMP_FORMAT),
            )
            for row in csv.DictReader(csv_file)
        ]


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
        help=(
            "Base path for the per-day contacts CSVs; one file is written per "
            "calendar day as <stem>_YYYYMMDD<suffix> next to it, e.g. "
            f"contacts_20260101.csv. Default base: <log-dir>/{DEFAULT_CONTACTS_CSV}"
        ),
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
    parser.add_argument(
        "--valid-date-start",
        type=date.fromisoformat,
        default=DEFAULT_VALID_DATE_START,
        help=(
            "Earliest calendar date (YYYY-MM-DD) accepted for a timer-resolved "
            "contact/self-report/eco-session timestamp; anything before this is "
            f"treated as corrupted flash data and dropped. Default: {DEFAULT_VALID_DATE_START.isoformat()}"
        ),
    )
    parser.add_argument(
        "--valid-date-end",
        type=date.fromisoformat,
        default=DEFAULT_VALID_DATE_END,
        help=(
            "Latest calendar date (YYYY-MM-DD) accepted for a timer-resolved "
            "contact/self-report/eco-session timestamp; anything after this is "
            f"treated as corrupted flash data and dropped. Default: {DEFAULT_VALID_DATE_END.isoformat()}"
        ),
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

    # Seed from whatever the last run already wrote, so beacons/events whose
    # log files have since been archived away aren't lost from these three.
    # Contacts don't need this: they're split per day and a day's CSV is
    # already final once no more logs for that day remain to be processed.
    summaries = read_summary_csv(args.summary_csv)
    self_reports = read_self_reports_csv(args.self_reports_csv)
    eco_sessions = read_eco_sessions_csv(args.eco_sessions_csv)
    current_timer_ref: dict[str, tuple[int, datetime]] = {}
    contacts: list[tuple[str, str, int, datetime]] = []

    skipped_contacts, skipped_self_reports, skipped_eco_sessions, skipped_invalid = aggregate_into(
        lines, summaries, current_timer_ref, contacts, self_reports, eco_sessions,
        args.valid_date_start, args.valid_date_end,
    )

    # Guard against duplicate rows if a log file gets reprocessed before it's
    # archived away.
    self_reports = list(dict.fromkeys(self_reports))
    eco_sessions = list(dict.fromkeys(eco_sessions))

    write_summary_csv(args.summary_csv, summaries)
    contacts_paths = write_contacts_csv(args.contacts_csv, contacts)
    write_self_reports_csv(args.self_reports_csv, self_reports)
    write_eco_sessions_csv(args.eco_sessions_csv, eco_sessions)

    print(f"Processed {len(log_paths)} log file(s), {len(lines)} parsed lines.")
    print(f"Wrote {len(summaries)} beacon summary rows to {args.summary_csv}")
    if contacts_paths:
        print(
            f"Wrote {len(contacts)} contact rows across {len(contacts_paths)} "
            f"daily CSV(s) in {args.contacts_csv.parent}: "
            + ", ".join(sorted(path.name for path in contacts_paths))
        )
    else:
        print(f"Wrote 0 contact rows (no daily CSVs written)")
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
    if skipped_invalid:
        print(
            f"Skipped {skipped_invalid} entries as corrupted flash data: beacon ID outside "
            f"{DEFAULT_VALID_ID_MIN}-{DEFAULT_VALID_ID_MAX} or {sorted(DEFAULT_VALID_EXTRA_IDS)}, "
            f"RSSI outside {DEFAULT_VALID_RSSI_MIN} to {DEFAULT_VALID_RSSI_MAX}, "
            f"or a resolved timestamp outside {args.valid_date_start} to {args.valid_date_end}."
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
