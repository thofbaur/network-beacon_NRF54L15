#!/usr/bin/env python3
"""Plot a room x time-segment histogram of self-reports from self_reports.csv.

Each day is split into wall-clock segments of --segment-hours (default 6, so
four segments/day: 00:00, 06:00, 12:00, 18:00).

A self-report only carries an ID and a timestamp - no location. To attribute
it to a room, this script checks whether that person had a qualifying
person-room contact session (same RSSI/duration/gap rules as
evaluation_heatmaps.py, via evaluation_preparation.py) overlapping a window
of --window-min minutes on either side of the report. A report can match more
than one room (the person had qualifying sessions with two rooms inside the
window) - it's counted once for each; a report matching no room is dropped
and reported as a count, not silently discarded.

Before matching, repeated reports from the same ID within --dedupe-window-min
minutes of an earlier one are dropped (see
evaluation_preparation.dedupe_self_reports) - a burst of several button
presses in quick succession is one real report, not several.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib
import numpy as np
import pandas as pd

matplotlib.use("Agg")
from matplotlib.colors import LinearSegmentedColormap  # noqa: E402
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
    DEFAULT_SELF_REPORTS_CSV,
    DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN,
    DEFAULT_SELFREPORT_WINDOW_MIN,
    DEFAULT_START_TIME,
    compute_person_room_sessions,
    dedupe_self_reports,
    load_contacts,
    load_id_joins,
    load_ignore_times,
    load_self_reports,
    match_self_reports_to_rooms,
    read_rooms,
)

DEFAULT_SEGMENT_HOURS = 6
DEFAULT_OUTPUT_PNG = DEFAULT_RESULT_DIR / "Heatmap_SelfReports.png"

# Sequential blue ramp, light -> dark (dataviz skill reference palette, palette.md) -
# same as evaluation_heatmaps.py, kept local since it's just a handful of color literals.
SEQUENTIAL_BLUE = [
    "#cde2fb", "#b7d3f6", "#9ec5f4", "#86b6ef", "#6da7ec", "#5598e7",
    "#3987e5", "#2a78d6", "#256abf", "#1c5cab", "#184f95", "#104281", "#0d366b",
]
SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRIDLINE = "#e1e0d9"


def bucket_matches_into_segments(matches: pd.DataFrame, segment_hours: float) -> pd.DataFrame:
    """Add a "segment" column: each match's report_time floored to a segment_hours wall-clock bucket."""
    matches = matches.copy()
    matches["segment"] = matches["report_time"].dt.floor(f"{segment_hours}h")
    return matches


def build_matrix(matches: pd.DataFrame) -> tuple[np.ndarray, list[pd.Timestamp], list[str]]:
    """segment x room count matrix. Rooms sorted alphabetically, segments chronologically."""
    segments = sorted(matches["segment"].unique())
    rooms = sorted(matches["room"].unique())
    segment_index = {s: i for i, s in enumerate(segments)}
    room_index = {r: j for j, r in enumerate(rooms)}

    matrix = np.zeros((len(segments), len(rooms)), dtype="int64")
    counts = matches.groupby(["segment", "room"]).size()
    for (segment, room), count in counts.items():
        matrix[segment_index[segment], room_index[room]] = count
    return matrix, segments, rooms


def plot_selfreport_heatmap(
    matrix: np.ndarray,
    segments: list[pd.Timestamp],
    rooms: list[str],
    window_min: float,
    segment_hours: float,
    rssi_threshold: int,
    min_duration_sec: float,
    output_path: Path,
) -> None:
    n_segments, n_rooms = matrix.shape
    fig_w = max(9.0, n_rooms * 0.42)
    fig_h = max(6.0, n_segments * 0.28)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    cmap = LinearSegmentedColormap.from_list("selfreport_blue", SEQUENTIAL_BLUE)
    vmax = matrix.max() if matrix.max() > 0 else 1
    im = ax.imshow(matrix, cmap=cmap, vmin=0, vmax=vmax, aspect="auto")

    weekdays = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]
    ax.set_xticks(np.arange(n_rooms))
    ax.set_xticklabels(rooms, rotation=60, ha="right", fontsize=7, color=INK_SECONDARY)
    ax.set_yticks(np.arange(n_segments))
    ax.set_yticklabels(
        [f"{s:%d.%m.} ({weekdays[s.weekday()]}) {s:%H:%M}" for s in segments], fontsize=7.5, color=INK_SECONDARY
    )
    ax.tick_params(length=0)
    for spine in ax.spines.values():
        spine.set_color(GRIDLINE)

    # Direct labels - counts are small integers, worth reading exactly rather than off the colorbar alone.
    for i in range(n_segments):
        for j in range(n_rooms):
            count = matrix[i, j]
            if count > 0:
                label_color = SURFACE if count > vmax * 0.6 else INK_PRIMARY
                ax.text(j, i, str(count), ha="center", va="center", fontsize=6.5, color=label_color)

    ax.set_xlabel("Raum", color=INK_SECONDARY, fontsize=9)
    ax.set_ylabel(f"Tag / {segment_hours:g}h-Segment", color=INK_SECONDARY, fontsize=9)
    ax.set_title(
        f"Self-Reports je Raum und {segment_hours:g}h-Segment — Fenster ±{window_min:g} min um den Report, "
        f"RSSI > {rssi_threshold} dBm, Kontakt ≥ {min_duration_sec:g}s am Stück",
        color=INK_PRIMARY,
        fontsize=10.5,
        pad=12,
    )

    cbar = fig.colorbar(im, ax=ax, fraction=0.035, pad=0.02)
    cbar.set_label("Anzahl Self-Reports", color=INK_SECONDARY, fontsize=9)
    cbar.ax.tick_params(labelsize=7, color=GRIDLINE, labelcolor=INK_SECONDARY)
    cbar.outline.set_edgecolor(GRIDLINE)

    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot a room x day histogram of self-reports, attributed to a room via nearby contact sessions."
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
        help=f"Path to list_rooms.csv. Default: {DEFAULT_ROOMS_CSV}",
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
        "--self-reports-csv",
        type=Path,
        default=DEFAULT_SELF_REPORTS_CSV,
        help=f"Path to self_reports.csv. Default: {DEFAULT_SELF_REPORTS_CSV}",
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
            "A person-room contact session only counts if it lasts at least this many "
            f"seconds. Default: {DEFAULT_MIN_DURATION_SEC}"
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
        "--window-min",
        type=float,
        default=DEFAULT_SELFREPORT_WINDOW_MIN,
        help=(
            "A self-report is attributed to a room if that person had a qualifying room "
            "session overlapping this many minutes before or after the report. "
            f"Default: {DEFAULT_SELFREPORT_WINDOW_MIN}"
        ),
    )
    parser.add_argument(
        "--dedupe-window-min",
        type=float,
        default=DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN,
        help=(
            "Ignore a self-report if it falls within this many minutes of an earlier report "
            f"from the same ID (a burst of repeat presses counts once). Default: {DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN}"
        ),
    )
    parser.add_argument(
        "--segment-hours",
        type=float,
        default=DEFAULT_SEGMENT_HOURS,
        help=(
            "Split each day into wall-clock segments this many hours long (00:00-aligned; "
            f"6 gives 4 segments/day). Default: {DEFAULT_SEGMENT_HOURS}"
        ),
    )
    parser.add_argument(
        "--start-time",
        type=pd.Timestamp,
        default=DEFAULT_START_TIME,
        help=(
            "Ignore contact events and self-reports before this local timestamp (deployment "
            f"go-live; earlier rows are setup/test noise). Default: {DEFAULT_START_TIME}"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT_PNG,
        help=f"Output PNG path. Default: {DEFAULT_OUTPUT_PNG}",
    )
    parser.add_argument(
        "--matrix-csv",
        type=Path,
        default=None,
        help="Optional: also write the day x room count matrix to this CSV path.",
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
    if not args.self_reports_csv.exists():
        print(f"Self-reports CSV not found: {args.self_reports_csv}", file=sys.stderr)
        return 1

    id_to_room_name, always_exclude = read_rooms(args.rooms_csv)

    id_joins = load_id_joins(args.join_id_csv)
    if id_joins:
        print(f"Joining {len(id_joins)} ID(s) per {args.join_id_csv}.")
    ignore_times = load_ignore_times(args.ignore_times_csv)
    if not ignore_times.empty:
        print(f"Ignoring {len(ignore_times)} blackout window(s) per {args.ignore_times_csv}.")

    contacts = load_contacts(contacts_paths, always_exclude, id_joins, ignore_times)
    contacts = contacts[contacts["Contact Local Time"] >= args.start_time]
    print(f"Loaded {len(contacts)} contact rows (from {args.start_time} on).")

    room_sessions = compute_person_room_sessions(
        contacts, id_to_room_name, args.rssi_threshold, args.min_duration_sec, args.max_gap_sec
    )
    print(f"Found {len(room_sessions)} qualifying person-room session(s).")

    self_reports = load_self_reports(args.self_reports_csv, id_joins, ignore_times)
    self_reports = self_reports[
        ~self_reports["ID"].isin(always_exclude) & (self_reports["Local Time"] >= args.start_time)
    ]
    print(f"Loaded {len(self_reports)} self-report(s) (from {args.start_time} on).")

    before_dedupe = len(self_reports)
    self_reports = dedupe_self_reports(self_reports, args.dedupe_window_min)
    print(
        f"Dropped {before_dedupe - len(self_reports)} repeat report(s) within "
        f"{args.dedupe_window_min:g} min of an earlier one from the same ID."
    )

    matches, matched_report_count = match_self_reports_to_rooms(self_reports, room_sessions, args.window_min)
    unmatched = len(self_reports) - matched_report_count
    print(
        f"Matched {len(matches)} (report, room) pair(s) from {matched_report_count} of "
        f"{len(self_reports)} report(s); {unmatched} report(s) had no qualifying room session "
        f"in the +/-{args.window_min:g} min window."
    )
    matches = bucket_matches_into_segments(matches, args.segment_hours)

    if matches.empty:
        print("No self-report/room matches found - nothing to plot.", file=sys.stderr)
        return 1

    matrix, segments, rooms = build_matrix(matches)
    plot_selfreport_heatmap(
        matrix, segments, rooms, args.window_min, args.segment_hours, args.rssi_threshold, args.min_duration_sec, args.output
    )
    print(f"Wrote self-report heatmap for {len(segments)} segment(s) x {len(rooms)} room(s) to {args.output}")

    if args.matrix_csv is not None:
        row_labels = [f"{s:%Y-%m-%d %H:%M}" for s in segments]
        pd.DataFrame(matrix, index=row_labels, columns=rooms).to_csv(args.matrix_csv)
        print(f"Wrote segment x room count matrix to {args.matrix_csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
