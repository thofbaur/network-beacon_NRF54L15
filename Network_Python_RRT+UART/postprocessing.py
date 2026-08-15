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
DEFAULT_CURRENT_ISSUES_CSV = "current_issues.csv"
DEFAULT_SANITY_FINDINGS_CSV = "sanity_findings.csv"
DEFAULT_STALE_HOURS = 36
DEFAULT_LOW_BATTERY_MV = 2650

# Names of the sanity checks that can skip an entry during aggregation, in
# the order they should appear as sanity_findings.csv columns.
CHECK_BEACON_ID_RANGE = "Beacon ID out of range"
CHECK_ID2_RANGE = "Contact ID2 out of range"
CHECK_RSSI_RANGE = "Contact RSSI out of range"
CHECK_CONTACT_NO_REF = "Contact missing Current Timer"
CHECK_CONTACT_TIMESTAMP = "Contact timestamp implausible"
CHECK_SELF_REPORT_NO_REF = "Self-report missing Current Timer"
CHECK_SELF_REPORT_TIMESTAMP = "Self-report timestamp implausible"
CHECK_ECO_SESSION_NO_REF = "Eco-session missing Current Timer"
CHECK_ECO_SESSION_TIMESTAMP = "Eco-session timestamp implausible"
SANITY_CHECK_NAMES = (
    CHECK_BEACON_ID_RANGE,
    CHECK_ID2_RANGE,
    CHECK_RSSI_RANGE,
    CHECK_CONTACT_NO_REF,
    CHECK_CONTACT_TIMESTAMP,
    CHECK_SELF_REPORT_NO_REF,
    CHECK_SELF_REPORT_TIMESTAMP,
    CHECK_ECO_SESSION_NO_REF,
    CHECK_ECO_SESSION_TIMESTAMP,
)

# Sanity bounds for this deployment. A beacon occasionally dumps a handful of
# corrupted trailing records (stale/uninitialized flash bytes read past its
# real stored contact count) - these show up as contacts with a wildly
# out-of-range timer (resolving to a timestamp months away) and/or an ID
# that isn't part of the fielded roster. See contacts_20261008.csv, traced
# to a beacon 44<->71 record with Timer: 4801652 that resolved to 2026-10-08.
DEFAULT_VALID_ID_MIN = 0
DEFAULT_VALID_ID_MAX = 170
DEFAULT_VALID_EXTRA_IDS = frozenset({252, 253, 254})
DEFAULT_VALID_DATE_START = date(2026, 8, 12)
DEFAULT_VALID_DATE_END = date(2026, 8, 29)
DEFAULT_VALID_RSSI_MIN = -110
DEFAULT_VALID_RSSI_MAX = -10

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
    source: Path


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


def parse_line(raw_line: str, source: Path) -> Optional[LogLine]:
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

    return LogLine(timestamp, id_match.group(1), rest.strip(), source)


def read_log_lines(log_paths: list[Path]) -> list[LogLine]:
    lines: list[LogLine] = []
    for path in log_paths:
        with path.open(encoding="utf-8", errors="replace") as log_file:
            for raw_line in log_file:
                parsed = parse_line(raw_line, path)
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
    message_timestamp: datetime,
    valid_date_start: date,
    valid_date_end: date,
) -> bool:
    """Whether a timer-resolved past event's timestamp is plausible.

    A contact/self-report/eco-session is resolved relative to a "Current
    Timer" reference, but that reference may now be a later one than the
    event itself (see aggregate_into's fallback to a following reference
    when no preceding one exists) - so the event's resolved timestamp is no
    longer bounded by the reference's own timestamp. It's still bounded by
    something more fundamental: it can never be later than message_timestamp,
    the wall-clock moment this log line itself was captured, since an event
    can't be reported before it happens. Corrupted timer values that
    undershoot only slightly can still land inside the valid date span by
    chance, so both checks are needed.
    """
    return timestamp <= message_timestamp and is_within_valid_span(
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
    skipped_by_source: Optional[dict[Path, dict[str, int]]] = None,
) -> tuple[int, int, int, int]:
    """Fold lines into the given (possibly already populated) aggregation state.

    Lets callers process a batch of new lines on top of state carried over
    from earlier batches, instead of rebuilding everything from scratch.
    Returns the number of contact, self-report, and eco session entries
    skipped for lack of any Current Timer reference (neither a preceding nor
    a following one, within this batch), plus the number of entries dropped
    as corrupted: an out-of-roster beacon ID, an out-of-range RSSI, or a
    timer-resolved timestamp outside [valid_date_start, valid_date_end]. If
    skipped_by_source is given, skipped_by_source[source][check_name] (one of
    the CHECK_* / SANITY_CHECK_NAMES constants) is incremented for every
    entry skipped for that reason.

    A beacon's own clock can reset (e.g. reboot) while it still holds a
    backlog of contacts stored under the old tick count, making their timer
    values exceed the next "Current Timer" reference even though they're
    real, legitimate history. So a preceding reference is preferred, but a
    following one (the next "Current Timer" for that beacon within this same
    batch) is used when no preceding one is available yet, rather than
    dropping the entry outright.
    """
    skipped_contacts = 0
    skipped_self_reports = 0
    skipped_eco_sessions = 0
    skipped_invalid = 0

    def note_skip(source: Path, check_name: str) -> None:
        if skipped_by_source is not None:
            by_check = skipped_by_source.setdefault(source, {})
            by_check[check_name] = by_check.get(check_name, 0) + 1

    # Earliest "Current Timer" occurrence per beacon within this batch, used
    # as a fallback reference for entries with no preceding one yet. Since
    # this is the first such line for that beacon in the whole batch, any
    # entry still lacking a preceding reference at the point it's processed
    # must chronologically precede it - so this is exactly "the next one".
    first_timer_ref: dict[str, tuple[int, datetime]] = {}
    for line in lines:
        if line.beacon_id in first_timer_ref or not is_valid_beacon_id(line.beacon_id):
            continue
        match = CURRENT_TIMER_RE.match(line.rest)
        if match:
            first_timer_ref[line.beacon_id] = (int(match.group(1)), line.timestamp)

    for line in lines:
        if not is_valid_beacon_id(line.beacon_id):
            skipped_invalid += 1
            note_skip(line.source, CHECK_BEACON_ID_RANGE)
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
            if not is_valid_beacon_id(other_id):
                skipped_invalid += 1
                note_skip(line.source, CHECK_ID2_RANGE)
                continue
            if not is_valid_rssi(rssi):
                skipped_invalid += 1
                note_skip(line.source, CHECK_RSSI_RANGE)
                continue

            ref = current_timer_ref.get(line.beacon_id) or first_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_contacts += 1
                note_skip(line.source, CHECK_CONTACT_NO_REF)
                continue

            reference_timer, reference_timestamp = ref
            contact_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, int(timer_str))
            if not is_plausible_event_timestamp(
                contact_timestamp, line.timestamp, valid_date_start, valid_date_end
            ):
                skipped_invalid += 1
                note_skip(line.source, CHECK_CONTACT_TIMESTAMP)
                continue

            id1, id2 = sorted((line.beacon_id, other_id), key=id_sort_key)
            contacts.append((id1, id2, rssi, contact_timestamp))
            continue

        match = SELF_REPORT_RE.match(line.rest)
        if match:
            ref = current_timer_ref.get(line.beacon_id) or first_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_self_reports += 1
                note_skip(line.source, CHECK_SELF_REPORT_NO_REF)
                continue

            reference_timer, reference_timestamp = ref
            report_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, int(match.group(1)))
            if not is_plausible_event_timestamp(
                report_timestamp, line.timestamp, valid_date_start, valid_date_end
            ):
                skipped_invalid += 1
                note_skip(line.source, CHECK_SELF_REPORT_TIMESTAMP)
                continue

            self_reports.append((line.beacon_id, report_timestamp))
            continue

        match = ECO_SESSION_RE.match(line.rest)
        if match:
            ref = current_timer_ref.get(line.beacon_id) or first_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_eco_sessions += 1
                note_skip(line.source, CHECK_ECO_SESSION_NO_REF)
                continue

            reference_timer, reference_timestamp = ref
            enter_timer, exit_timer = (int(group) for group in match.groups())
            enter_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, enter_timer)
            exit_timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, exit_timer)
            if not (
                is_plausible_event_timestamp(enter_timestamp, line.timestamp, valid_date_start, valid_date_end)
                and is_plausible_event_timestamp(
                    exit_timestamp, line.timestamp, valid_date_start, valid_date_end
                )
            ):
                skipped_invalid += 1
                note_skip(line.source, CHECK_ECO_SESSION_TIMESTAMP)
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
        current_issues_csv: Path,
        valid_date_start: date = DEFAULT_VALID_DATE_START,
        valid_date_end: date = DEFAULT_VALID_DATE_END,
        stale_after: timedelta = timedelta(hours=DEFAULT_STALE_HOURS),
        low_battery_mv: int = DEFAULT_LOW_BATTERY_MV,
    ) -> None:
        self.log_dir = log_dir
        self.summary_csv = summary_csv
        self.contacts_csv = contacts_csv
        self.self_reports_csv = self_reports_csv
        self.eco_sessions_csv = eco_sessions_csv
        self.current_issues_csv = current_issues_csv
        self.valid_date_start = valid_date_start
        self.valid_date_end = valid_date_end
        self.stale_after = stale_after
        self.low_battery_mv = low_battery_mv
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
                parsed = parse_line(raw_line, path)
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
        write_current_issues_csv(
            self.current_issues_csv, self.summaries, datetime.now(), self.stale_after, self.low_battery_mv
        )
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


def describe_beacon_issues(
    summary: BeaconSummary,
    now: datetime,
    stale_after: timedelta = timedelta(hours=DEFAULT_STALE_HOURS),
    low_battery_mv: int = DEFAULT_LOW_BATTERY_MV,
) -> list[str]:
    """Plain-text list of a beacon's current issues, if any: never/not-recently seen,
    low battery, and any fault bits set in its last reported Status byte."""
    issues: list[str] = []

    if summary.last_seen is None:
        issues.append("Never seen")
    elif now - summary.last_seen > stale_after:
        stale_hours = stale_after.total_seconds() / 3600
        issues.append(
            f"Not seen in over {stale_hours:g}h (last seen {summary.last_seen.strftime(TIMESTAMP_FORMAT)})"
        )

    if summary.last_voltage_mv is not None and summary.last_voltage_mv < low_battery_mv:
        issues.append(f"Low battery: {summary.last_voltage_mv} mV (below {low_battery_mv} mV)")

    for label, bit_value in zip(ERROR_BIT_LABELS, decode_error_bits(summary.last_status_byte)):
        if bit_value:
            issues.append(label)

    return issues


def write_current_issues_csv(
    path: Path,
    summaries: dict[str, BeaconSummary],
    now: datetime,
    stale_after: timedelta = timedelta(hours=DEFAULT_STALE_HOURS),
    low_battery_mv: int = DEFAULT_LOW_BATTERY_MV,
) -> int:
    """Write one row per (beacon, issue) for every beacon that currently has one.

    Returns the number of distinct beacons with at least one issue.
    """
    beacon_ids_with_issues: set[str] = set()
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["ID", "Issue"])
        for beacon_id in sorted(summaries, key=id_sort_key):
            for issue in describe_beacon_issues(summaries[beacon_id], now, stale_after, low_battery_mv):
                writer.writerow([beacon_id, issue])
                beacon_ids_with_issues.add(beacon_id)

    return len(beacon_ids_with_issues)


def write_sanity_findings_csv(
    path: Path,
    log_paths: list[Path],
    skipped_by_source: dict[Path, dict[str, int]],
) -> None:
    """Write one row per log file: how many entries each sanity check skipped.

    Every log file gets a row, including ones with nothing skipped, so the
    table is a complete record of what was read, not just where problems
    were found.
    """
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["Log File", *SANITY_CHECK_NAMES, "Total"])
        for log_path in log_paths:
            by_check = skipped_by_source.get(log_path, {})
            counts = [by_check.get(check_name, 0) for check_name in SANITY_CHECK_NAMES]
            writer.writerow([log_path.name, *counts, sum(counts)])


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
        "--current-issues-csv",
        type=Path,
        default=None,
        help=(
            "Output path for the current-issues CSV (one row per beacon/issue: "
            "fault bits, not seen recently, low battery). "
            f"Default: <log-dir>/{DEFAULT_CURRENT_ISSUES_CSV}"
        ),
    )
    parser.add_argument(
        "--sanity-findings-csv",
        type=Path,
        default=None,
        help=(
            "Output path for the sanity-findings CSV: one row per log file "
            "read, with a column per sanity check showing how many entries it "
            f"skipped. Default: <log-dir>/{DEFAULT_SANITY_FINDINGS_CSV}"
        ),
    )
    parser.add_argument(
        "--stale-hours",
        type=float,
        default=DEFAULT_STALE_HOURS,
        help=(
            "Flag a beacon as an issue if it hasn't been seen within this many "
            f"hours. Default: {DEFAULT_STALE_HOURS}"
        ),
    )
    parser.add_argument(
        "--low-battery-mv",
        type=int,
        default=DEFAULT_LOW_BATTERY_MV,
        help=(
            "Flag a beacon as an issue if its last reported battery voltage is "
            f"below this many mV. Default: {DEFAULT_LOW_BATTERY_MV}"
        ),
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
    if args.current_issues_csv is None:
        args.current_issues_csv = args.log_dir / DEFAULT_CURRENT_ISSUES_CSV
    if args.sanity_findings_csv is None:
        args.sanity_findings_csv = args.log_dir / DEFAULT_SANITY_FINDINGS_CSV
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
    skipped_by_source: dict[Path, dict[str, int]] = {}

    skipped_contacts, skipped_self_reports, skipped_eco_sessions, skipped_invalid = aggregate_into(
        lines, summaries, current_timer_ref, contacts, self_reports, eco_sessions,
        args.valid_date_start, args.valid_date_end, skipped_by_source,
    )

    # Guard against duplicate rows if a log file gets reprocessed before it's
    # archived away.
    self_reports = list(dict.fromkeys(self_reports))
    eco_sessions = list(dict.fromkeys(eco_sessions))

    write_summary_csv(args.summary_csv, summaries)
    contacts_paths = write_contacts_csv(args.contacts_csv, contacts)
    write_self_reports_csv(args.self_reports_csv, self_reports)
    write_eco_sessions_csv(args.eco_sessions_csv, eco_sessions)
    issue_count = write_current_issues_csv(
        args.current_issues_csv,
        summaries,
        datetime.now(),
        timedelta(hours=args.stale_hours),
        args.low_battery_mv,
    )
    write_sanity_findings_csv(args.sanity_findings_csv, log_paths, skipped_by_source)

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
    print(f"Wrote {issue_count} beacon(s) with current issues to {args.current_issues_csv}")
    print(f"Wrote sanity findings for {len(log_paths)} log file(s) to {args.sanity_findings_csv}")
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
    total_skipped = skipped_contacts + skipped_self_reports + skipped_eco_sessions + skipped_invalid
    if total_skipped:
        print("Skipped entries by log file:")
        for path in log_paths:
            print(f"  {path.name}: {sum(skipped_by_source.get(path, {}).values())}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
