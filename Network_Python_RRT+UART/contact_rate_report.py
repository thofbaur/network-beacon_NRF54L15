#!/usr/bin/env python3
"""Report contacts recorded per beacon between successive readouts.

Every beacon connection logs a "Contact Count: N" line - the number of new
contact records the beacon has stored since it was last read out (storage is
cleared on readout, so this is already a delta, not a running total).
Grouped by beacon ID and ordered chronologically, dividing that count by the
elapsed time since the previous readout gives a contacts-per-hour rate for
that interval. For an ID's first readout there is no previous readout to
measure the interval from, so the beacon's own "Current Timer" line (seconds
since its clock last reset, at which point its contact count was 0 too) is
used instead.
"""

from __future__ import annotations

import argparse
import csv
import re
import statistics
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

import postprocessing as pp

DEFAULT_OUTPUT_CSV = "contact_rate_report.csv"

CONTACT_COUNT_RE = re.compile(r"^Contact Count:\s*(\d+)$")

RATE_OUTLIER_THRESHOLD = 50_000.0

NOTE_ZERO_INTERVAL = "Readouts at/within the same second as the previous one"
NOTE_NO_REFERENCE = "No Current Timer reference before first readout"


@dataclass(frozen=True)
class Readout:
    timestamp: datetime
    count: int
    file_name: str
    line_number: int
    current_timer: Optional[int]


def find_contact_count_readouts(log_paths: list[Path]) -> dict[str, list[Readout]]:
    """Find every "Contact Count" line per beacon ID across log_paths.

    Returns {beacon_id: [Readout, ...]}, each list in chronological order.
    Each Readout also carries the value of the "Current Timer" line that
    immediately preceded it for that same beacon ID (seconds since that
    beacon's clock last reset), used to date a beacon's first readout.
    """
    lines = pp.read_log_lines(log_paths)

    last_current_timer: dict[str, int] = {}
    readouts: dict[str, list[Readout]] = {}
    for line in lines:
        timer_match = pp.CURRENT_TIMER_RE.match(line.rest)
        if timer_match:
            last_current_timer[line.beacon_id] = int(timer_match.group(1))
            continue

        count_match = CONTACT_COUNT_RE.match(line.rest)
        if not count_match:
            continue

        readouts.setdefault(line.beacon_id, []).append(
            Readout(
                line.timestamp,
                int(count_match.group(1)),
                line.source.name,
                line.line_number,
                last_current_timer.get(line.beacon_id),
            )
        )
    return readouts


def write_report_csv(path: Path, readouts_by_id: dict[str, list[Readout]]) -> tuple[int, list[float]]:
    """Write one row per Contact Count readout, grouped and ordered by beacon ID.

    Returns (row_count, rates), where rates holds every successfully computed
    contacts-per-hour value across all IDs, unfiltered, for the caller to
    aggregate.
    """
    row_count = 0
    rates: list[float] = []
    with path.open("w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(
            [
                "ID",
                "File",
                "Line",
                "Timestamp",
                "Contact Count",
                "Hours since previous readout",
                "Contacts since previous readout",
                "Contacts per hour",
                "Note",
            ]
        )
        for beacon_id in sorted(readouts_by_id, key=pp.id_sort_key):
            previous: Readout | None = None
            for readout in readouts_by_id[beacon_id]:
                hours_since = ""
                rate = ""
                note = ""

                if previous is not None:
                    hours: Optional[float] = (readout.timestamp - previous.timestamp).total_seconds() / 3600
                elif readout.current_timer is not None:
                    hours = readout.current_timer / 3600
                else:
                    hours = None
                    note = NOTE_NO_REFERENCE

                if hours is not None:
                    hours_since = f"{hours:.4f}"
                    if hours <= 0:
                        note = NOTE_ZERO_INTERVAL
                    else:
                        rate_value = readout.count / hours
                        rate = f"{rate_value:.2f}"
                        rates.append(rate_value)

                writer.writerow(
                    [
                        beacon_id,
                        readout.file_name,
                        readout.line_number,
                        readout.timestamp.strftime(pp.TIMESTAMP_FORMAT),
                        readout.count,
                        hours_since,
                        readout.count,
                        rate,
                        note,
                    ]
                )
                row_count += 1
                previous = readout
    return row_count, rates


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Report contacts-per-hour between successive Contact Count readouts, per beacon ID."
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
    args = parser.parse_args(argv)
    if args.output_csv is None:
        args.output_csv = Path(__file__).resolve().parent / pp.DEFAULT_OUTPUT_DIR_NAME / DEFAULT_OUTPUT_CSV
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)

    log_paths = sorted(args.log_dir.glob("*.log"))
    if not log_paths:
        print(f"No .log files found in {args.log_dir}", file=sys.stderr)
        return 1

    readouts_by_id = find_contact_count_readouts(log_paths)
    row_count, rates = write_report_csv(args.output_csv, readouts_by_id)

    print(f"Processed {len(log_paths)} log file(s).")
    print(f"Wrote {row_count} Contact Count readout(s) across {len(readouts_by_id)} beacon(s) to {args.output_csv}")

    filtered_rates = [rate for rate in rates if rate <= RATE_OUTLIER_THRESHOLD]
    discarded = len(rates) - len(filtered_rates)
    if filtered_rates:
        average_rate = sum(filtered_rates) / len(filtered_rates)
        variance = statistics.variance(filtered_rates) if len(filtered_rates) > 1 else 0.0
        print(
            f"Average contacts/hour across {len(filtered_rates)} readout(s), all IDs combined "
            f"(discarded {discarded} outlier(s) > {RATE_OUTLIER_THRESHOLD:,.0f}): "
            f"{average_rate:.2f} (variance: {variance:.2f}, stdev: {variance ** 0.5:.2f})"
        )
    else:
        print("No readouts available to compute an average rate.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
