#!/usr/bin/env python3
"""Plot total person-person contact duration per day, from contacts_*.csv.

Same underlying data and weighting as evaluation_heatmaps.py's time-of-day
heatmap (Heatmap_Zeit.png) - every qualifying session's duration, split
proportionally across the wall-clock buckets it overlaps (see
evaluation_preparation.compute_time_of_day_matrix) - just summed across each
day's buckets instead of shown per half-hour, so this is a bar chart (one
value per day) rather than a heatmap.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib
import numpy as np
import pandas as pd

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from evaluation_preparation import (  # noqa: E402
    DEFAULT_CONTACTS_DIR,
    DEFAULT_IGNORE_TIMES_CSV,
    DEFAULT_JOIN_ID_CSV,
    DEFAULT_MAX_GAP_SEC,
    DEFAULT_MIN_DURATION_SEC,
    DEFAULT_RESULT_DIR,
    DEFAULT_ROOMS_CSV,
    DEFAULT_RSSI_THRESHOLD,
    DEFAULT_START_TIME,
    compute_person_person_sessions,
    compute_time_of_day_matrix,
    load_contacts,
    load_id_joins,
    load_ignore_times,
    read_rooms,
)

DEFAULT_OUTPUT_PNG = DEFAULT_RESULT_DIR / "Heatmap_Zeit_Tag.png"

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRIDLINE = "#e1e0d9"
BAR_COLOR = "#3987e5"


def plot_daily_totals(
    days: list[pd.Timestamp],
    hours: np.ndarray,
    rssi_threshold: int,
    min_duration_sec: float,
    output_path: Path,
) -> None:
    fig_w = max(9.0, len(days) * 0.6)
    fig, ax = plt.subplots(figsize=(fig_w, 6.0), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    weekdays = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]
    labels = [f"{d:%d.%m.} ({weekdays[d.weekday()]})" for d in days]
    x = np.arange(len(days))
    ax.bar(x, hours, color=BAR_COLOR, width=0.65, edgecolor=SURFACE, linewidth=0.5)

    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=60, ha="right", fontsize=8.5, color=INK_SECONDARY)
    ax.tick_params(length=0, colors=INK_SECONDARY)
    for spine in ax.spines.values():
        spine.set_color(GRIDLINE)
    ax.grid(axis="y", color=GRIDLINE, linewidth=0.7)
    ax.set_axisbelow(True)

    ax.set_xlabel("Tag", color=INK_SECONDARY, fontsize=9.5)
    ax.set_ylabel("Summe Personen-Personen-Kontaktzeit (Stunden)", color=INK_SECONDARY, fontsize=9.5)
    ax.set_title(
        f"Personen-Personen-Kontaktzeit je Tag — RSSI > {rssi_threshold} dBm, "
        f"Kontakt ≥ {min_duration_sec:g}s am Stück\n"
        "Gewichtung wie Heatmap_Zeit (Gesamtdauer aller Kontakte), aber ohne Halbstunden-Aufteilung",
        color=INK_PRIMARY,
        fontsize=11,
        pad=12,
    )

    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Plot total person-person contact duration per day (same weighting as "
            "evaluation_heatmaps.py's time-of-day heatmap, summed across each day)."
        )
    )
    parser.add_argument(
        "--contacts-dir",
        type=Path,
        default=DEFAULT_CONTACTS_DIR,
        help=f"Directory containing contacts_*.csv files. Default: {DEFAULT_CONTACTS_DIR}",
    )
    parser.add_argument(
        "--rooms-csv",
        type=Path,
        default=DEFAULT_ROOMS_CSV,
        help=f"Path to list_rooms.csv (room/anchor IDs to exclude). Default: {DEFAULT_ROOMS_CSV}",
    )
    parser.add_argument(
        "--join-id-csv",
        type=Path,
        default=DEFAULT_JOIN_ID_CSV,
        help=(
            "Path to join_ID.csv (columns ID1, ID2, date - from date on, ID2 is folded into "
            "ID1, e.g. a reissued beacon's old and new ID). Missing/empty file means no joins. "
            f"Default: {DEFAULT_JOIN_ID_CSV}"
        ),
    )
    parser.add_argument(
        "--ignore-times-csv",
        type=Path,
        default=DEFAULT_IGNORE_TIMES_CSV,
        help=(
            "Path to ignore_times.csv (columns ID, start, end - drop that ID's contacts entirely "
            f"during that window). Missing/empty file means no windows. Default: {DEFAULT_IGNORE_TIMES_CSV}"
        ),
    )
    parser.add_argument(
        "--rssi-threshold",
        type=int,
        default=DEFAULT_RSSI_THRESHOLD,
        help=f"Only count contact events with RSSI strictly above this value (dBm). Default: {DEFAULT_RSSI_THRESHOLD}",
    )
    parser.add_argument(
        "--min-duration-sec",
        type=float,
        default=DEFAULT_MIN_DURATION_SEC,
        help=(
            "A contact session only counts if it lasts at least this many seconds. "
            f"Default: {DEFAULT_MIN_DURATION_SEC}"
        ),
    )
    parser.add_argument(
        "--max-gap-sec",
        type=float,
        default=DEFAULT_MAX_GAP_SEC,
        help=(
            "Gap between qualifying events, in seconds, beyond which a contact session is "
            f"considered ended. Default: {DEFAULT_MAX_GAP_SEC}"
        ),
    )
    parser.add_argument(
        "--start-time",
        type=pd.Timestamp,
        default=DEFAULT_START_TIME,
        help=(
            "Ignore contact events before this local timestamp (deployment go-live; earlier "
            f"rows are setup/test noise). Default: {DEFAULT_START_TIME}"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT_PNG,
        help=f"Output PNG path. Default: {DEFAULT_OUTPUT_PNG}",
    )
    parser.add_argument(
        "--csv",
        type=Path,
        default=None,
        help="Optional: also write the daily totals (hours) to this CSV path.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    contacts_paths = sorted(args.contacts_dir.glob("contacts_*.csv"))
    if not contacts_paths:
        print(f"No contacts_*.csv files found in {args.contacts_dir}", file=sys.stderr)
        return 1
    if not args.rooms_csv.exists():
        print(f"Rooms CSV not found: {args.rooms_csv}", file=sys.stderr)
        return 1

    id_to_room_name, always_exclude = read_rooms(args.rooms_csv)
    room_ids = set(id_to_room_name)

    id_joins = load_id_joins(args.join_id_csv)
    if id_joins:
        print(f"Joining {len(id_joins)} ID(s) per {args.join_id_csv}.")
    ignore_times = load_ignore_times(args.ignore_times_csv)
    if not ignore_times.empty:
        print(f"Ignoring {len(ignore_times)} blackout window(s) per {args.ignore_times_csv}.")

    contacts = load_contacts(contacts_paths, always_exclude, id_joins, ignore_times)
    print(f"Loaded {len(contacts)} contact rows from {len(contacts_paths)} file(s).")

    before_cutoff = len(contacts)
    contacts = contacts[contacts["Contact Local Time"] >= args.start_time]
    print(f"Dropped {before_cutoff - len(contacts)} row(s) before start time {args.start_time}.")

    pp_sessions = compute_person_person_sessions(
        contacts, room_ids, args.rssi_threshold, args.min_duration_sec, args.max_gap_sec
    )
    if pp_sessions.empty:
        print("No qualifying person-person contacts found for the given thresholds.", file=sys.stderr)
        return 1

    time_matrix, days = compute_time_of_day_matrix(pp_sessions)
    daily_hours = time_matrix.sum(axis=0) / 3600.0

    plot_daily_totals(days, daily_hours, args.rssi_threshold, args.min_duration_sec, args.output)
    print(f"Wrote daily contact-time totals for {len(days)} day(s) to {args.output}")

    if args.csv is not None:
        pd.DataFrame({"date": [f"{d:%Y-%m-%d}" for d in days], "hours": daily_hours}).to_csv(args.csv, index=False)
        print(f"Wrote daily contact-time totals (hours) to {args.csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
