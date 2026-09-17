#!/usr/bin/env python3
"""Report every Current Timer occurrence for two mapped beacon-ID ranges.

For each occurrence, calculates the local time at which that beacon's own
Timer would have read 0 (its last reset/boot point), and groups the output
by mapped ID pair (e.g. 48 next to 0, 57 next to 9) so occurrences of a
possibly-mislabeled high ID sit next to the low ID it may have been
misattributed to.
"""

from __future__ import annotations

import argparse
import csv
import sys
from datetime import datetime, timedelta
from pathlib import Path

import postprocessing as pp

DEFAULT_LOW_RANGE = "0-9"
DEFAULT_HIGH_RANGE = "48-57"
DEFAULT_OUTPUT_CSV = "current_timer_zero_report.csv"

CsvRow = tuple[str, int, datetime, int, datetime]


def parse_id_range(spec: str) -> list[str]:
    """Parse an inclusive "start-end" range spec, e.g. "48-57", into ID strings."""
    start_str, _, end_str = spec.partition("-")
    start, end = int(start_str), int(end_str)
    return [str(beacon_id) for beacon_id in range(start, end + 1)]


def find_current_timer_occurrences(
    log_paths: list[Path], target_ids: set[str]
) -> dict[str, list[CsvRow]]:
    """Find every "Current Timer" line for any of target_ids across log_paths.

    Returns {beacon_id: [(file_name, line_number, timestamp, timer, zero_timestamp), ...]},
    each list sorted chronologically. zero_timestamp is the local time at
    which that beacon's Timer would have read 0, derived the same way
    contacts/self-reports/eco-sessions are resolved elsewhere in
    postprocessing.py: timestamp - timer seconds.
    """
    occurrences: dict[str, list[CsvRow]] = {beacon_id: [] for beacon_id in target_ids}

    for path in log_paths:
        with path.open(encoding="utf-8", errors="replace") as log_file:
            for line_number, raw_line in enumerate(log_file, start=1):
                parsed = pp.parse_line(raw_line, path, line_number)
                if parsed is None or parsed.beacon_id not in target_ids:
                    continue

                match = pp.CURRENT_TIMER_RE.match(parsed.rest)
                if not match:
                    continue

                timer = int(match.group(1))
                zero_timestamp = parsed.timestamp - timedelta(seconds=timer)
                occurrences[parsed.beacon_id].append(
                    (path.name, line_number, parsed.timestamp, timer, zero_timestamp)
                )

    for id_occurrences in occurrences.values():
        id_occurrences.sort(key=lambda occurrence: occurrence[2])
    return occurrences


def write_report_csv(
    path: Path,
    low_ids: list[str],
    high_ids: list[str],
    occurrences: dict[str, list[CsvRow]],
) -> int:
    """Write one row per Current Timer occurrence, grouped by mapped (low, high) ID pair.

    Returns the total number of rows written.
    """
    row_count = 0
    with path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(
            [
                "ID",
                "File",
                "Line of current timer",
                "Timestamp",
                "Value of current timer",
                "Calculated time for 0 timer",
            ]
        )
        for low_id, high_id in zip(low_ids, high_ids):
            for id_label, beacon_id in ((low_id, low_id), (f"{high_id} (mapped to {low_id})", high_id)):
                for file_name, line_number, timestamp, timer, zero_timestamp in occurrences[beacon_id]:
                    writer.writerow(
                        [
                            id_label,
                            file_name,
                            line_number,
                            timestamp.strftime(pp.TIMESTAMP_FORMAT),
                            timer,
                            zero_timestamp.strftime(pp.TIMESTAMP_FORMAT),
                        ]
                    )
                    row_count += 1
    return row_count


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Report Current Timer occurrences and their calculated zero-time for two mapped ID ranges."
    )
    parser.add_argument(
        "--log-dir",
        type=Path,
        default=Path(__file__).resolve().parent / pp.DEFAULT_LOG_DIR_NAME,
        help=f"Directory containing *.log files. Default: this script's directory/{pp.DEFAULT_LOG_DIR_NAME}.",
    )
    parser.add_argument(
        "--output-csv",
        type=Path,
        default=None,
        help=(
            "Output CSV path. Default: this script's directory/"
            f"{pp.DEFAULT_OUTPUT_DIR_NAME}/{DEFAULT_OUTPUT_CSV}"
        ),
    )
    parser.add_argument(
        "--low-ids",
        default=DEFAULT_LOW_RANGE,
        help=f'Inclusive low-end ID range, e.g. "0-9". Default: {DEFAULT_LOW_RANGE}',
    )
    parser.add_argument(
        "--high-ids",
        default=DEFAULT_HIGH_RANGE,
        help=(
            "Inclusive high-end ID range, mapped 1:1 onto --low-ids in order "
            f'(e.g. "48-57" pairs 48 with the first --low-ids value). Default: {DEFAULT_HIGH_RANGE}'
        ),
    )
    args = parser.parse_args(argv)
    if args.output_csv is None:
        args.output_csv = Path(__file__).resolve().parent / pp.DEFAULT_OUTPUT_DIR_NAME / DEFAULT_OUTPUT_CSV
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)

    low_ids = parse_id_range(args.low_ids)
    high_ids = parse_id_range(args.high_ids)
    if len(low_ids) != len(high_ids):
        print(
            f"--low-ids ({args.low_ids}, {len(low_ids)} id(s)) and --high-ids "
            f"({args.high_ids}, {len(high_ids)} id(s)) must cover the same number of IDs.",
            file=sys.stderr,
        )
        return 1

    log_paths = sorted(args.log_dir.glob("*.log"))
    if not log_paths:
        print(f"No .log files found in {args.log_dir}", file=sys.stderr)
        return 1

    occurrences = find_current_timer_occurrences(log_paths, set(low_ids) | set(high_ids))
    row_count = write_report_csv(args.output_csv, low_ids, high_ids, occurrences)

    print(f"Processed {len(log_paths)} log file(s).")
    print(f"Wrote {row_count} Current Timer occurrence(s) to {args.output_csv}")
    for low_id, high_id in zip(low_ids, high_ids):
        print(f"  ID {low_id}: {len(occurrences[low_id])}, ID {high_id}: {len(occurrences[high_id])}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
