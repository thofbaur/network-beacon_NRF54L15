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
DEFAULT_LOG_DIR_NAME = "Logs"
DEFAULT_OUTPUT_DIR_NAME = "Output"
DEFAULT_SUMMARY_CSV = "beacon_summary.csv"
DEFAULT_CONTACTS_CSV = "contacts.csv"
DEFAULT_SELF_REPORTS_CSV = "self_reports.csv"
DEFAULT_ECO_SESSIONS_CSV = "eco_sessions.csv"
DEFAULT_MEASUREMENTS_CSV = "measurements.csv"
DEFAULT_CURRENT_ISSUES_CSV = "current_issues.csv"
DEFAULT_SANITY_FINDINGS_CSV = "sanity_findings.csv"
DEFAULT_TRANSFER_MISLABEL_MD = "transfer_mislabel.md"
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
# Unlike the checks above, these don't cause an entry to be skipped during
# aggregation - they count how often an entry fell back to a following
# Current Timer reference for lack of a preceding one (see aggregate_into's
# reboot-backlog comment), purely for visibility into how often that
# fallback path is exercised.
CHECK_CONTACT_FALLBACK_REF = "Contact used following (no preceding) Current Timer"
CHECK_SELF_REPORT_FALLBACK_REF = "Self-report used following (no preceding) Current Timer"
CHECK_ECO_SESSION_FALLBACK_REF = "Eco-session used following (no preceding) Current Timer"
# Also purely informational: an entry whose timestamp came out implausibly in
# the future against the preceding/following reference above, but was
# recovered by retrying against the *previous* (pre-reboot) Current Timer
# reference instead - see resolve_event_timestamp.
CHECK_CONTACT_TIMESTAMP_RECOVERED = "Contact timestamp recovered via previous Current Timer"
CHECK_SELF_REPORT_TIMESTAMP_RECOVERED = "Self-report timestamp recovered via previous Current Timer"
CHECK_ECO_SESSION_TIMESTAMP_RECOVERED = "Eco-session timestamp recovered via previous Current Timer"
# Unlike the checks above, this one doesn't cause an entry to be skipped
# during aggregation - it flags back-to-back contact lines for manual review
# (see find_adjacent_id_mismatches / transfer_mislabel.md) while leaving both
# entries in the output.
CHECK_TRANSFER_MISLABEL = "Potential contact transfer mislabeling"
SANITY_CHECK_NAMES = (
    CHECK_BEACON_ID_RANGE,
    CHECK_ID2_RANGE,
    CHECK_RSSI_RANGE,
    CHECK_CONTACT_NO_REF,
    CHECK_CONTACT_FALLBACK_REF,
    CHECK_CONTACT_TIMESTAMP_RECOVERED,
    CHECK_CONTACT_TIMESTAMP,
    CHECK_SELF_REPORT_NO_REF,
    CHECK_SELF_REPORT_FALLBACK_REF,
    CHECK_SELF_REPORT_TIMESTAMP_RECOVERED,
    CHECK_SELF_REPORT_TIMESTAMP,
    CHECK_ECO_SESSION_NO_REF,
    CHECK_ECO_SESSION_FALLBACK_REF,
    CHECK_ECO_SESSION_TIMESTAMP_RECOVERED,
    CHECK_ECO_SESSION_TIMESTAMP,
    CHECK_TRANSFER_MISLABEL,
)

# Sanity bounds for this deployment. A beacon occasionally dumps a handful of
# corrupted trailing records (stale/uninitialized flash bytes read past its
# real stored contact count) - these show up as contacts with a wildly
# out-of-range timer (resolving to a timestamp months away) and/or an ID
# that isn't part of the fielded roster. See contacts_20261008.csv, traced
# to a beacon 44<->71 record with Timer: 4801652 that resolved to 2026-10-08.
DEFAULT_VALID_ID_MIN = 1
DEFAULT_VALID_ID_MAX = 170
DEFAULT_VALID_EXTRA_IDS = frozenset({252, 253, 254})
DEFAULT_VALID_RSSI_MIN = -110
DEFAULT_VALID_RSSI_MAX = -10

# How far past message_timestamp a timer-resolved event's timestamp may fall
# and still be considered plausible. A resolved timestamp strictly can't be
# later than the moment the log line was captured, but a few seconds of
# overshoot is routine (see is_plausible_event_timestamp) rather than a sign
# of corruption, so a small tolerance avoids flagging those as implausible.
DEFAULT_TIMESTAMP_TOLERANCE_SECONDS = 600

# Ceiling on the Timer increase between two back-to-back contact lines
# reporting different beacon IDs for find_adjacent_id_mismatches to treat it
# as a potential mislabeling: the Timer must not decrease, and must not
# increase by this much or more. See find_adjacent_id_mismatches for the
# full rationale.
DEFAULT_MAX_TIMER_INCREASE = 60

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
CONTACT_COUNT_RE = re.compile(r"^Contact Count:\s*(\d+)$")
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
    line_number: int


@dataclass
class BeaconSummary:
    beacon_id: str
    last_seen: Optional[datetime] = None
    last_voltage_mv: Optional[int] = None
    last_status_byte: Optional[int] = None


@dataclass(frozen=True)
class Measurement:
    """One "Current Timer" readout for a beacon and the connection burst around it."""

    beacon_id: str
    timestamp: datetime
    voltage_mv: Optional[int]
    contact_count: Optional[int]
    timer_zero: datetime


@dataclass(frozen=True)
class ValidTimeSpan:
    start: datetime
    end: datetime


# The evaluation only covers this deployment window; a timer-resolved
# timestamp outside it is treated as corrupted flash data and dropped (see
# DEFAULT_VALID_ID_MIN/MAX above for the same reasoning applied to IDs).
DEFAULT_VALID_SPAN = ValidTimeSpan(datetime(2026, 8, 12, 0, 0, 0), datetime(2026, 8, 29, 9, 0, 0))


def id_sort_key(beacon_id: str):
    try:
        return (0, int(beacon_id))
    except ValueError:
        return (1, beacon_id)


def parse_line(raw_line: str, source: Path, line_number: int) -> Optional[LogLine]:
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

    return LogLine(timestamp, id_match.group(1), rest.strip(), source, line_number)


def read_log_lines(log_paths: list[Path]) -> list[LogLine]:
    lines: list[LogLine] = []
    for path in log_paths:
        with path.open(encoding="utf-8", errors="replace") as log_file:
            for line_number, raw_line in enumerate(log_file, start=1):
                parsed = parse_line(raw_line, path, line_number)
                if parsed is not None:
                    lines.append(parsed)

    # Stable sort: files are chronological internally, so ties (lines sharing
    # the same second, e.g. one connect burst) keep their original order -
    # which matters because a "Current Timer" line must stay ahead of the
    # data-set lines that reference it.
    lines.sort(key=lambda line: line.timestamp)
    return lines


def format_log_line(line: LogLine) -> str:
    """Reconstruct a LogLine's original raw log-file text (sans newline)."""
    return f"{line.timestamp.strftime(TIMESTAMP_FORMAT)},ID: {line.beacon_id},{line.rest}"


def find_adjacent_id_mismatches(
    lines: list[LogLine],
    max_timer_increase: int = DEFAULT_MAX_TIMER_INCREASE,
) -> list[tuple[LogLine, LogLine]]:
    """Find pairs of back-to-back contact lines with different beacon IDs.

    A beacon dumps its whole stored contact list in one connection burst, so
    consecutive contact lines (ID2/Timer/RSSI) normally share the same "ID:"
    - the reporting beacon. Two contact lines reporting different IDs with no
    line of any kind between them (not even a "Transfer complete", "Status",
    "Current Timer", "Contact Count", or "Voltage" line - those mark a
    legitimate handover to the next connection) is a potential mislabeling:
    a contact attributed to the wrong beacon, e.g. by a connection race in
    dsa_logger.py. A genuine handover's Timer resets to the next beacon's own
    clock, unrelated to the previous one's, so it's unlikely to land in the
    narrow "held steady or crept up a little" band a real within-dump step
    would; the Timer must not decrease, and must not increase by
    max_timer_increase or more, to count as a potential mislabeling. This
    doesn't imply the data should be dropped (unlike SANITY_CHECK_NAMES),
    just flagged for manual review.
    """
    mismatches = []
    for previous, current in zip(lines, lines[1:]):
        previous_match = CONTACT_RE.match(previous.rest)
        current_match = CONTACT_RE.match(current.rest)
        if not previous_match or not current_match:
            continue
        if previous.beacon_id == current.beacon_id:
            continue
        timer_delta = int(current_match.group(2)) - int(previous_match.group(2))
        if not (0 <= timer_delta < max_timer_increase):
            continue
        mismatches.append((previous, current))
    return mismatches


def count_mismatches_by_source(mismatches: list[tuple[LogLine, LogLine]]) -> dict[Path, int]:
    """Tally find_adjacent_id_mismatches occurrences per source log file.

    An occurrence counts once against each distinct file its two lines come
    from (almost always the same file, since the pair is adjacent in the
    merged, chronologically-sorted line stream).
    """
    counts: dict[Path, int] = {}
    for previous, current in mismatches:
        counts[previous.source] = counts.get(previous.source, 0) + 1
        if current.source != previous.source:
            counts[current.source] = counts.get(current.source, 0) + 1
    return counts


def write_transfer_mislabel_md(
    path: Path,
    mismatches: list[tuple[LogLine, LogLine]],
    max_timer_increase: int = DEFAULT_MAX_TIMER_INCREASE,
) -> None:
    """Write a Markdown report listing every adjacent-contact-line ID mismatch.

    One section per occurrence, showing the two offending lines verbatim so
    they can be cross-checked against the raw log files.
    """
    with path.open("w", encoding="utf-8") as md_file:
        md_file.write("# Potential Contact Mislabeling\n\n")
        md_file.write(
            "Every occurrence where two back-to-back contact log lines "
            "(`ID2: ..., Timer: ..., RSSI: ...`, no line of any kind between "
            "them) report a different `ID:` than the line immediately before "
            "them, with the Timer not decreasing and increasing by less than "
            f"{max_timer_increase}. Within one connection dump all contact "
            "lines share the same `ID:` and have Timer values close "
            "together, so a change here is a potential mislabeling.\n\n"
        )
        md_file.write(f"Found {len(mismatches)} occurrence(s).\n\n")
        for index, (previous, current) in enumerate(mismatches, start=1):
            md_file.write(f"## {index}. ID {previous.beacon_id} -> ID {current.beacon_id}\n\n")
            md_file.write(f"{previous.source.name}:{previous.line_number}\n")
            md_file.write("```\n")
            md_file.write(f"{format_log_line(previous)}\n")
            md_file.write("```\n\n")
            md_file.write(f"{current.source.name}:{current.line_number}\n")
            md_file.write("```\n")
            md_file.write(f"{format_log_line(current)}\n")
            md_file.write("```\n\n")


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


def is_within_valid_span(timestamp: datetime, valid_span: ValidTimeSpan) -> bool:
    """Whether timestamp falls within [valid_span.start, valid_span.end]."""
    return valid_span.start <= timestamp <= valid_span.end


def is_plausible_event_timestamp(
    timestamp: datetime,
    message_timestamp: datetime,
    valid_span: ValidTimeSpan,
    tolerance_seconds: int = DEFAULT_TIMESTAMP_TOLERANCE_SECONDS,
) -> bool:
    """Whether a timer-resolved past event's timestamp is plausible.

    A contact/self-report/eco-session is resolved relative to a "Current
    Timer" reference, but that reference may now be a later one than the
    event itself (see aggregate_into's fallback to a following reference
    when no preceding one exists) - so the event's resolved timestamp is no
    longer bounded by the reference's own timestamp. It's still bounded by
    something more fundamental: it can never be later than message_timestamp,
    the wall-clock moment this log line itself was captured, since an event
    can't be reported before it happens - except for a small tolerance, since
    a beacon reporting a backlog of contacts recorded just before its own
    clock reset routinely overshoots the fresh "Current Timer" reference by a
    few seconds (see aggregate_into's reboot-backlog comment) without that
    being a sign of corruption. Corrupted timer values that undershoot only
    slightly can still land inside the valid date span by chance, so both
    checks are needed.
    """
    return timestamp <= message_timestamp + timedelta(seconds=tolerance_seconds) and is_within_valid_span(
        timestamp, valid_span
    )


def resolve_event_timestamp(
    timer: int,
    ref: tuple[int, datetime],
    message_timestamp: datetime,
    valid_span: ValidTimeSpan,
    previous_ref: Optional[tuple[int, datetime]] = None,
    previous_high_water: Optional[int] = None,
    tolerance_seconds: int = DEFAULT_TIMESTAMP_TOLERANCE_SECONDS,
) -> tuple[datetime, bool, bool]:
    """Resolve timer to a local timestamp against ref, retrying against
    previous_ref if that lands implausibly in the future.

    A beacon's clock resets on reboot, but it can still transmit contacts
    stored in flash from before the reset - for those, the *previous*
    Current Timer reference applies instead (see aggregate_into's
    reboot-backlog comment). Recognizing that case from timer alone isn't
    reliable (nothing rules out timer being genuinely, implausibly large),
    so the retry only kicks in when resolving against ref overshoots into
    the future *and* timer exceeds every timer value already seen under
    previous_ref's own epoch (previous_high_water) - i.e. this is a new high
    for that old epoch, consistent with being one more late entry from it
    rather than a corrupted value.

    Returns (timestamp, plausible, used_previous_ref). When plausible is
    False, timestamp is still the (implausible) value resolved against ref,
    for the caller to discard.
    """
    reference_timer, reference_timestamp = ref
    timestamp = _timer_to_timestamp(reference_timer, reference_timestamp, timer)
    if is_plausible_event_timestamp(
        timestamp, message_timestamp, valid_span, tolerance_seconds
    ):
        return timestamp, True, False

    in_future = timestamp > message_timestamp + timedelta(seconds=tolerance_seconds)
    if (
        in_future
        and previous_ref is not None
        and previous_high_water is not None
        and previous_high_water < timer
    ):
        old_reference_timer, old_reference_timestamp = previous_ref
        old_timestamp = _timer_to_timestamp(old_reference_timer, old_reference_timestamp, timer)
        if is_plausible_event_timestamp(
            old_timestamp, message_timestamp, valid_span, tolerance_seconds
        ):
            return old_timestamp, True, True

    return timestamp, False, False


def aggregate_into(
    lines: list[LogLine],
    summaries: dict[str, BeaconSummary],
    current_timer_ref: dict[str, tuple[int, datetime]],
    contacts: list[tuple[str, str, int, datetime]],
    self_reports: list[tuple[str, datetime]],
    eco_sessions: list[tuple[str, datetime, datetime]],
    valid_span: ValidTimeSpan = DEFAULT_VALID_SPAN,
    skipped_by_source: Optional[dict[Path, dict[str, int]]] = None,
    previous_timer_ref: Optional[dict[str, tuple[int, datetime]]] = None,
    entry_high_water: Optional[dict[str, int]] = None,
    previous_entry_high_water: Optional[dict[str, int]] = None,
) -> tuple[int, int, int, int]:
    """Fold lines into the given (possibly already populated) aggregation state.

    Lets callers process a batch of new lines on top of state carried over
    from earlier batches, instead of rebuilding everything from scratch.
    Returns the number of contact, self-report, and eco session entries
    skipped for lack of any Current Timer reference (neither a preceding nor
    a following one, within this batch), plus the number of entries dropped
    as corrupted: an out-of-roster beacon ID, an out-of-range RSSI, or a
    timer-resolved timestamp outside [valid_span.start, valid_span.end]. If
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

    previous_timer_ref, entry_high_water, and previous_entry_high_water are
    optional caller-supplied state (as for current_timer_ref) tracking, per
    beacon, the Current Timer reference that was replaced by the current one
    and the highest contact/self-report/eco-session timer value seen under
    each - used by resolve_event_timestamp to recover entries that overshoot
    into the future against the current reference but are consistent with
    being late arrivals from the previous one. Callers that don't care about
    that recovery (e.g. one-shot scripts wanting default behavior) can leave
    them unset.
    """
    if previous_timer_ref is None:
        previous_timer_ref = {}
    if entry_high_water is None:
        entry_high_water = {}
    if previous_entry_high_water is None:
        previous_entry_high_water = {}

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
            if line.beacon_id in current_timer_ref:
                previous_timer_ref[line.beacon_id] = current_timer_ref[line.beacon_id]
                if line.beacon_id in entry_high_water:
                    previous_entry_high_water[line.beacon_id] = entry_high_water[line.beacon_id]
                else:
                    previous_entry_high_water.pop(line.beacon_id, None)
            current_timer_ref[line.beacon_id] = (int(match.group(1)), line.timestamp)
            entry_high_water.pop(line.beacon_id, None)
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
            timer = int(timer_str)
            rssi = int(rssi_str)
            entry_high_water[line.beacon_id] = max(entry_high_water.get(line.beacon_id, timer), timer)
            if not is_valid_beacon_id(other_id):
                skipped_invalid += 1
                note_skip(line.source, CHECK_ID2_RANGE)
                continue
            if not is_valid_rssi(rssi):
                skipped_invalid += 1
                note_skip(line.source, CHECK_RSSI_RANGE)
                continue

            preceding_ref = current_timer_ref.get(line.beacon_id)
            ref = preceding_ref or first_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_contacts += 1
                note_skip(line.source, CHECK_CONTACT_NO_REF)
                continue
            if preceding_ref is None:
                note_skip(line.source, CHECK_CONTACT_FALLBACK_REF)

            contact_timestamp, plausible, used_previous_ref = resolve_event_timestamp(
                timer, ref, line.timestamp, valid_span,
                previous_timer_ref.get(line.beacon_id), previous_entry_high_water.get(line.beacon_id),
            )
            if not plausible:
                skipped_invalid += 1
                note_skip(line.source, CHECK_CONTACT_TIMESTAMP)
                continue
            if used_previous_ref:
                note_skip(line.source, CHECK_CONTACT_TIMESTAMP_RECOVERED)

            contacts.append((line.beacon_id, other_id, rssi, contact_timestamp))
            continue

        match = SELF_REPORT_RE.match(line.rest)
        if match:
            timer = int(match.group(1))
            entry_high_water[line.beacon_id] = max(entry_high_water.get(line.beacon_id, timer), timer)

            preceding_ref = current_timer_ref.get(line.beacon_id)
            ref = preceding_ref or first_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_self_reports += 1
                note_skip(line.source, CHECK_SELF_REPORT_NO_REF)
                continue
            if preceding_ref is None:
                note_skip(line.source, CHECK_SELF_REPORT_FALLBACK_REF)

            report_timestamp, plausible, used_previous_ref = resolve_event_timestamp(
                timer, ref, line.timestamp, valid_span,
                previous_timer_ref.get(line.beacon_id), previous_entry_high_water.get(line.beacon_id),
            )
            if not plausible:
                skipped_invalid += 1
                note_skip(line.source, CHECK_SELF_REPORT_TIMESTAMP)
                continue
            if used_previous_ref:
                note_skip(line.source, CHECK_SELF_REPORT_TIMESTAMP_RECOVERED)

            self_reports.append((line.beacon_id, report_timestamp))
            continue

        match = ECO_SESSION_RE.match(line.rest)
        if match:
            enter_timer, exit_timer = (int(group) for group in match.groups())
            max_timer = max(enter_timer, exit_timer)
            entry_high_water[line.beacon_id] = max(entry_high_water.get(line.beacon_id, max_timer), max_timer)

            preceding_ref = current_timer_ref.get(line.beacon_id)
            ref = preceding_ref or first_timer_ref.get(line.beacon_id)
            if ref is None:
                skipped_eco_sessions += 1
                note_skip(line.source, CHECK_ECO_SESSION_NO_REF)
                continue
            if preceding_ref is None:
                note_skip(line.source, CHECK_ECO_SESSION_FALLBACK_REF)

            prev_ref = previous_timer_ref.get(line.beacon_id)
            prev_high_water = previous_entry_high_water.get(line.beacon_id)
            enter_timestamp, enter_plausible, enter_used_previous = resolve_event_timestamp(
                enter_timer, ref, line.timestamp, valid_span, prev_ref, prev_high_water,
            )
            exit_timestamp, exit_plausible, exit_used_previous = resolve_event_timestamp(
                exit_timer, ref, line.timestamp, valid_span, prev_ref, prev_high_water,
            )
            if not (enter_plausible and exit_plausible):
                skipped_invalid += 1
                note_skip(line.source, CHECK_ECO_SESSION_TIMESTAMP)
                continue
            if enter_used_previous or exit_used_previous:
                note_skip(line.source, CHECK_ECO_SESSION_TIMESTAMP_RECOVERED)

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
        valid_span: ValidTimeSpan = DEFAULT_VALID_SPAN,
        stale_after: timedelta = timedelta(hours=DEFAULT_STALE_HOURS),
        low_battery_mv: int = DEFAULT_LOW_BATTERY_MV,
        measurements_csv: Optional[Path] = None,
    ) -> None:
        self.log_dir = log_dir
        self.summary_csv = summary_csv
        self.contacts_csv = contacts_csv
        self.self_reports_csv = self_reports_csv
        self.eco_sessions_csv = eco_sessions_csv
        self.current_issues_csv = current_issues_csv
        self.measurements_csv = measurements_csv or summary_csv.parent / DEFAULT_MEASUREMENTS_CSV
        self.valid_span = valid_span
        self.stale_after = stale_after
        self.low_battery_mv = low_battery_mv
        self._offsets: dict[Path, int] = {}
        self._line_counts: dict[Path, int] = {}
        self.summaries: dict[str, BeaconSummary] = {}
        self.current_timer_ref: dict[str, tuple[int, datetime]] = {}
        self.previous_timer_ref: dict[str, tuple[int, datetime]] = {}
        self.entry_high_water: dict[str, int] = {}
        self.previous_entry_high_water: dict[str, int] = {}
        self.contacts: list[tuple[str, str, int, datetime]] = []
        self.self_reports: list[tuple[str, datetime]] = []
        self.eco_sessions: list[tuple[str, datetime, datetime]] = []
        self.measurements: list[Measurement] = []
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
            line_count = self._line_counts.get(path, 0)
            self._line_counts[path] = line_count + len(raw_lines)
            for line_number, raw_line in enumerate(raw_lines, start=line_count + 1):
                parsed = parse_line(raw_line, path, line_number)
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
            self.valid_span,
            previous_timer_ref=self.previous_timer_ref,
            entry_high_water=self.entry_high_water,
            previous_entry_high_water=self.previous_entry_high_water,
        )
        self.skipped_contacts += skipped_contacts
        self.skipped_self_reports += skipped_self_reports
        self.skipped_eco_sessions += skipped_eco_sessions
        self.skipped_invalid += skipped_invalid

        self.measurements.extend(collect_measurements(new_lines))
        self.measurements.sort(key=lambda m: (id_sort_key(m.beacon_id), m.timestamp))

        write_summary_csv(self.summary_csv, self.summaries)
        write_contacts_csv(self.contacts_csv, self.contacts)
        write_self_reports_csv(self.self_reports_csv, self.self_reports)
        write_eco_sessions_csv(self.eco_sessions_csv, self.eco_sessions)
        write_measurements_csv(self.measurements_csv, self.measurements)
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


def read_current_issues_csv(path: Path) -> list[tuple[str, str]]:
    """Read a previously-written current-issues CSV back in as (ID, Issue) rows."""
    if not path.exists():
        return []

    with path.open(encoding="utf-8", newline="") as csv_file:
        return [(row["ID"], row["Issue"]) for row in csv.DictReader(csv_file)]


def update_current_issues_for_beacon(
    path: Path,
    beacon_id: str,
    summary: BeaconSummary,
    now: datetime,
    stale_after: timedelta = timedelta(hours=DEFAULT_STALE_HOURS),
    low_battery_mv: int = DEFAULT_LOW_BATTERY_MV,
) -> None:
    """Rewrite only ``beacon_id``'s rows in current_issues.csv.

    Every other beacon's rows are carried over verbatim; ``beacon_id``'s rows are
    replaced with ``describe_beacon_issues(summary, ...)`` - which may be empty, in
    which case the beacon drops out of the file entirely. This lets a live capture
    keep the findings file current for the beacon it just talked to without
    re-reading the whole log history the way IncrementalPostProcessor does.
    """
    rows = [row for row in read_current_issues_csv(path) if row[0] != beacon_id]
    rows.extend(
        (beacon_id, issue)
        for issue in describe_beacon_issues(summary, now, stale_after, low_battery_mv)
    )
    rows.sort(key=lambda row: id_sort_key(row[0]))  # stable: issue order within an ID is kept
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["ID", "Issue"])
        writer.writerows(rows)


def write_sanity_findings_csv(
    path: Path,
    log_paths: list[Path],
    skipped_by_source: dict[Path, dict[str, int]],
) -> None:
    """Write one row per log file: how many entries each sanity check flagged.

    Every log file gets a row, including ones with nothing flagged, so the
    table is a complete record of what was read, not just where problems
    were found. Most checks count entries skipped during aggregation. Three
    are purely informational and never cause a skip - the CHECK_*_FALLBACK_REF
    checks count entries that resolved against a following (rather than
    preceding) Current Timer reference, per aggregate_into's reboot-backlog
    fallback - and CHECK_TRANSFER_MISLABEL counts occurrences flagged for
    review without anything being skipped - see find_adjacent_id_mismatches.
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


def collect_measurements(lines: list[LogLine]) -> list[Measurement]:
    """One row per "Current Timer" readout: the reporting beacon, the moment
    it was read, its battery voltage and stored contact count from the same
    connection burst, and the local time its own Timer would have read 0
    (the readout timestamp minus Current Timer seconds - its last clock reset).

    A connection burst always leads with "Current Timer", followed by
    "Contact Count" and "Voltage" for that same beacon before any next burst
    (see the log format), so each new "Current Timer" line for a beacon
    flushes the row accumulated for the previous one. Out-of-roster beacon
    IDs are dropped, as they are for the other outputs.
    """
    measurements: list[Measurement] = []
    pending: dict[str, dict] = {}

    def flush(beacon_id: str) -> None:
        row = pending.pop(beacon_id, None)
        if row is not None:
            measurements.append(
                Measurement(
                    beacon_id,
                    row["timestamp"],
                    row["voltage_mv"],
                    row["contact_count"],
                    row["timestamp"] - timedelta(seconds=row["timer"]),
                )
            )

    for line in lines:
        if not is_valid_beacon_id(line.beacon_id):
            continue

        match = CURRENT_TIMER_RE.match(line.rest)
        if match:
            flush(line.beacon_id)
            pending[line.beacon_id] = {
                "timestamp": line.timestamp,
                "timer": int(match.group(1)),
                "voltage_mv": None,
                "contact_count": None,
            }
            continue

        row = pending.get(line.beacon_id)
        if row is None:
            continue

        match = VOLTAGE_RE.match(line.rest)
        if match:
            row["voltage_mv"] = int(match.group(1))
            continue

        match = CONTACT_COUNT_RE.match(line.rest)
        if match:
            row["contact_count"] = int(match.group(1))

    for beacon_id in list(pending):
        flush(beacon_id)

    measurements.sort(key=lambda m: (id_sort_key(m.beacon_id), m.timestamp))
    return measurements


def write_measurements_csv(path: Path, measurements: list[Measurement]) -> None:
    """Write one row per Current Timer readout, grouped by beacon ID and ordered chronologically."""
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(
            ["ID", "Timestamp", "Battery (mV)", "Collected Contacts", "Calculated time for 0 timer"]
        )
        for measurement in measurements:
            writer.writerow(
                [
                    measurement.beacon_id,
                    measurement.timestamp.strftime(TIMESTAMP_FORMAT),
                    "" if measurement.voltage_mv is None else measurement.voltage_mv,
                    "" if measurement.contact_count is None else measurement.contact_count,
                    measurement.timer_zero.strftime(TIMESTAMP_FORMAT),
                ]
            )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Post-process dsa_logger.py *.log files into per-ID and per-contact CSVs."
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=Path(__file__).resolve().parent / DEFAULT_LOG_DIR_NAME,
        help=f"Directory containing *.log files. Default: this script's directory/{DEFAULT_LOG_DIR_NAME}.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(__file__).resolve().parent / DEFAULT_OUTPUT_DIR_NAME,
        help=(
            "Directory the output CSVs/Markdown are written into (created if "
            f"missing). Default: this script's directory/{DEFAULT_OUTPUT_DIR_NAME}."
        ),
    )
    parser.add_argument(
        "--summary-csv",
        type=Path,
        default=None,
        help=f"Output path for the per-ID summary CSV. Default: <output-dir>/{DEFAULT_SUMMARY_CSV}",
    )
    parser.add_argument(
        "--contacts-csv",
        type=Path,
        default=None,
        help=(
            "Base path for the per-day contacts CSVs; one file is written per "
            "calendar day as <stem>_YYYYMMDD<suffix> next to it, e.g. "
            f"contacts_20260101.csv. Default base: <output-dir>/{DEFAULT_CONTACTS_CSV}"
        ),
    )
    parser.add_argument(
        "--self-reports-csv",
        type=Path,
        default=None,
        help=f"Output path for the self-report CSV. Default: <output-dir>/{DEFAULT_SELF_REPORTS_CSV}",
    )
    parser.add_argument(
        "--eco-sessions-csv",
        type=Path,
        default=None,
        help=f"Output path for the eco session CSV. Default: <output-dir>/{DEFAULT_ECO_SESSIONS_CSV}",
    )
    parser.add_argument(
        "--measurements-csv",
        type=Path,
        default=None,
        help=(
            "Output path for the measurements CSV: one row per Current Timer "
            "readout with the beacon's battery voltage, stored contact count, "
            "and calculated Timer-zero time. "
            f"Default: <output-dir>/{DEFAULT_MEASUREMENTS_CSV}"
        ),
    )
    parser.add_argument(
        "--current-issues-csv",
        type=Path,
        default=None,
        help=(
            "Output path for the current-issues CSV (one row per beacon/issue: "
            "fault bits, not seen recently, low battery). "
            f"Default: <output-dir>/{DEFAULT_CURRENT_ISSUES_CSV}"
        ),
    )
    parser.add_argument(
        "--sanity-findings-csv",
        type=Path,
        default=None,
        help=(
            "Output path for the sanity-findings CSV: one row per log file "
            "read, with a column per sanity check showing how many entries it "
            f"skipped. Default: <output-dir>/{DEFAULT_SANITY_FINDINGS_CSV}"
        ),
    )
    parser.add_argument(
        "--transfer-mislabel-md",
        type=Path,
        default=None,
        help=(
            "Output path for the contact-mislabeling report: every pair of "
            "chronologically adjacent contact log lines that report a "
            f"different ID. Default: <output-dir>/{DEFAULT_TRANSFER_MISLABEL_MD}"
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
        type=datetime.fromisoformat,
        default=DEFAULT_VALID_SPAN.start,
        help=(
            "Earliest timestamp (YYYY-MM-DD or YYYY-MM-DD HH:MM:SS) accepted for "
            "a timer-resolved contact/self-report/eco-session event; anything "
            "before this is treated as corrupted flash data and dropped. "
            f"Default: {DEFAULT_VALID_SPAN.start.isoformat(sep=' ')}"
        ),
    )
    parser.add_argument(
        "--valid-date-end",
        type=datetime.fromisoformat,
        default=DEFAULT_VALID_SPAN.end,
        help=(
            "Latest timestamp (YYYY-MM-DD or YYYY-MM-DD HH:MM:SS) accepted for "
            "a timer-resolved contact/self-report/eco-session event; anything "
            "after this is treated as corrupted flash data and dropped. "
            f"Default: {DEFAULT_VALID_SPAN.end.isoformat(sep=' ')}"
        ),
    )
    args = parser.parse_args(argv)
    args.valid_span = ValidTimeSpan(args.valid_date_start, args.valid_date_end)
    if args.summary_csv is None:
        args.summary_csv = args.output_dir / DEFAULT_SUMMARY_CSV
    if args.contacts_csv is None:
        args.contacts_csv = args.output_dir / DEFAULT_CONTACTS_CSV
    if args.self_reports_csv is None:
        args.self_reports_csv = args.output_dir / DEFAULT_SELF_REPORTS_CSV
    if args.eco_sessions_csv is None:
        args.eco_sessions_csv = args.output_dir / DEFAULT_ECO_SESSIONS_CSV
    if args.measurements_csv is None:
        args.measurements_csv = args.output_dir / DEFAULT_MEASUREMENTS_CSV
    if args.current_issues_csv is None:
        args.current_issues_csv = args.output_dir / DEFAULT_CURRENT_ISSUES_CSV
    if args.sanity_findings_csv is None:
        args.sanity_findings_csv = args.output_dir / DEFAULT_SANITY_FINDINGS_CSV
    if args.transfer_mislabel_md is None:
        args.transfer_mislabel_md = args.output_dir / DEFAULT_TRANSFER_MISLABEL_MD
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output_dir.mkdir(parents=True, exist_ok=True)

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
        args.valid_span, skipped_by_source,
    )

    # Guard against duplicate rows if a log file gets reprocessed before it's
    # archived away.
    self_reports = list(dict.fromkeys(self_reports))
    eco_sessions = list(dict.fromkeys(eco_sessions))

    write_summary_csv(args.summary_csv, summaries)
    contacts_paths = write_contacts_csv(args.contacts_csv, contacts)
    write_self_reports_csv(args.self_reports_csv, self_reports)
    write_eco_sessions_csv(args.eco_sessions_csv, eco_sessions)
    measurements = collect_measurements(lines)
    write_measurements_csv(args.measurements_csv, measurements)
    issue_count = write_current_issues_csv(
        args.current_issues_csv,
        summaries,
        datetime.now(),
        timedelta(hours=args.stale_hours),
        args.low_battery_mv,
    )
    mislabel_mismatches = find_adjacent_id_mismatches(lines)
    sanity_by_source = {source: dict(counts) for source, counts in skipped_by_source.items()}
    for source, count in count_mismatches_by_source(mislabel_mismatches).items():
        sanity_by_source.setdefault(source, {})[CHECK_TRANSFER_MISLABEL] = count
    write_sanity_findings_csv(args.sanity_findings_csv, log_paths, sanity_by_source)
    write_transfer_mislabel_md(args.transfer_mislabel_md, mislabel_mismatches)

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
    print(f"Wrote {len(measurements)} measurement rows to {args.measurements_csv}")
    print(f"Wrote {issue_count} beacon(s) with current issues to {args.current_issues_csv}")
    print(f"Wrote sanity findings for {len(log_paths)} log file(s) to {args.sanity_findings_csv}")
    print(
        f"Wrote {len(mislabel_mismatches)} potential mislabeling occurrence(s) "
        f"to {args.transfer_mislabel_md}"
    )
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
            f"or a resolved timestamp outside {args.valid_span.start} to {args.valid_span.end}."
        )
    total_skipped = skipped_contacts + skipped_self_reports + skipped_eco_sessions + skipped_invalid
    if total_skipped:
        print("Skipped entries by log file:")
        for path in log_paths:
            print(f"  {path.name}: {sum(skipped_by_source.get(path, {}).values())}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
