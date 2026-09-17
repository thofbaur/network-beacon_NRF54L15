#!/usr/bin/env python3
"""Plot the distribution of time gaps between consecutive self-reports of the same ID.

For every ID, self-reports are sorted by time and the gap to the previous
report from that same ID is computed (the first report of an ID has no
gap - it contributes nothing). All IDs' gaps are pooled into one
distribution: this is diagnostic for --dedupe-window-min in
evaluation_selfreport.py/evaluation_occupancy.py (see
evaluation_preparation.dedupe_self_reports) - a cluster of very short gaps
here is repeat button presses, not distinct reports, and this plot is where
that cluster (and where it ends) is actually visible, which is what the
dedupe threshold should be set from.

Gaps span seconds to days, so the histogram bins log-spaced and the x-axis
is log-scaled with human-readable tick labels (1min, 1h, 1d, ...) rather than
raw seconds.
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
    DEFAULT_IGNORE_TIMES_CSV,
    DEFAULT_JOIN_ID_CSV,
    DEFAULT_ROOMS_CSV,
    DEFAULT_RESULT_DIR,
    DEFAULT_SELF_REPORTS_CSV,
    DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN,
    DEFAULT_START_TIME,
    load_id_joins,
    load_ignore_times,
    load_self_reports,
    read_rooms,
)

DEFAULT_OUTPUT_PNG = DEFAULT_RESULT_DIR / "verteilung_selfreport_gaps.png"
DEFAULT_BINS = 60

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRIDLINE = "#e1e0d9"
BAR_COLOR = "#3987e5"
THRESHOLD_COLOR = "#e34948"

# (seconds, label) ticks to show on the log-scaled x-axis, filtered to the data's own range.
TICK_CANDIDATES = [
    (1, "1s"), (5, "5s"), (10, "10s"), (30, "30s"),
    (60, "1min"), (300, "5min"), (600, "10min"), (1800, "30min"),
    (3600, "1h"), (3 * 3600, "3h"), (6 * 3600, "6h"), (12 * 3600, "12h"),
    (86400, "1d"), (2 * 86400, "2d"), (7 * 86400, "7d"), (14 * 86400, "14d"),
]


def compute_gaps(self_reports: pd.DataFrame) -> pd.Series:
    """Seconds between each report and the same ID's immediately preceding report."""
    reports = self_reports.sort_values(["ID", "Local Time"])
    gap = reports.groupby("ID")["Local Time"].diff().dropna()
    return gap.dt.total_seconds()


def format_seconds(seconds: float) -> str:
    if seconds < 60:
        return f"{seconds:.0f}s"
    if seconds < 3600:
        return f"{seconds / 60:.1f}min"
    if seconds < 86400:
        return f"{seconds / 3600:.1f}h"
    return f"{seconds / 86400:.1f}d"


def plot_gap_distribution(
    gaps_sec: pd.Series,
    bins: int,
    dedupe_window_min: float,
    output_path: Path,
) -> None:
    fig, ax = plt.subplots(figsize=(10, 6), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    log_edges = np.logspace(np.log10(gaps_sec.min()), np.log10(gaps_sec.max()), bins + 1)
    ax.hist(gaps_sec, bins=log_edges, color=BAR_COLOR, edgecolor=SURFACE, linewidth=0.3)
    ax.set_xscale("log")

    threshold_sec = dedupe_window_min * 60
    ax.axvline(threshold_sec, color=THRESHOLD_COLOR, linewidth=1.5, linestyle="--")
    ax.text(
        threshold_sec,
        ax.get_ylim()[1] * 0.97,
        f" --dedupe-window-min = {dedupe_window_min:g} min",
        color=THRESHOLD_COLOR,
        fontsize=8.5,
        va="top",
    )

    ticks = [sec for sec, _ in TICK_CANDIDATES if gaps_sec.min() <= sec <= gaps_sec.max()]
    labels = [label for sec, label in TICK_CANDIDATES if gaps_sec.min() <= sec <= gaps_sec.max()]
    ax.set_xticks(ticks)
    ax.set_xticklabels(labels, fontsize=8.5, color=INK_SECONDARY)
    ax.tick_params(length=0, colors=INK_SECONDARY)
    for spine in ax.spines.values():
        spine.set_color(GRIDLINE)
    ax.grid(axis="y", color=GRIDLINE, linewidth=0.7)
    ax.set_axisbelow(True)

    ax.set_xlabel("Zeitlicher Abstand zum vorherigen Self-Report derselben ID", color=INK_SECONDARY, fontsize=9.5)
    ax.set_ylabel("Anzahl Report-Paare", color=INK_SECONDARY, fontsize=9.5)
    ax.set_title(
        f"Verteilung der Abstände zwischen aufeinanderfolgenden Self-Reports einer ID (n={len(gaps_sec)})",
        color=INK_PRIMARY,
        fontsize=11,
        pad=12,
    )

    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Plot the distribution of time gaps between consecutive self-reports from the same ID."
    )
    parser.add_argument(
        "--self-reports-csv",
        type=Path,
        default=DEFAULT_SELF_REPORTS_CSV,
        help=f"Path to self_reports.csv. Default: {DEFAULT_SELF_REPORTS_CSV}",
    )
    parser.add_argument(
        "--rooms-csv",
        type=Path,
        default=DEFAULT_ROOMS_CSV,
        help=f"Path to list_rooms.csv (only used for the reserved special IDs to exclude). Default: {DEFAULT_ROOMS_CSV}",
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
            "Path to ignore_times.csv (columns ID, start, end - drop that ID's self-reports "
            f"entirely during that window). Missing/empty file means no windows. Default: {DEFAULT_IGNORE_TIMES_CSV}"
        ),
    )
    parser.add_argument(
        "--start-time",
        type=pd.Timestamp,
        default=DEFAULT_START_TIME,
        help=(
            "Ignore self-reports before this local timestamp (deployment go-live; earlier "
            f"rows are setup/test noise). Default: {DEFAULT_START_TIME}"
        ),
    )
    parser.add_argument(
        "--dedupe-window-min",
        type=float,
        default=DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN,
        help=(
            "Reference line: the --dedupe-window-min value evaluation_selfreport.py / "
            f"evaluation_occupancy.py use. Default: {DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN}"
        ),
    )
    parser.add_argument(
        "--bins", type=int, default=DEFAULT_BINS, help=f"Number of log-spaced histogram bins. Default: {DEFAULT_BINS}"
    )
    parser.add_argument(
        "--output", type=Path, default=DEFAULT_OUTPUT_PNG, help=f"Output PNG path. Default: {DEFAULT_OUTPUT_PNG}"
    )
    parser.add_argument(
        "--gaps-csv",
        type=Path,
        default=None,
        help="Optional: also write the raw per-(ID, report pair) gap list (seconds) to this CSV path.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    if not args.self_reports_csv.exists():
        print(f"Self-reports CSV not found: {args.self_reports_csv}", file=sys.stderr)
        return 1
    if not args.rooms_csv.exists():
        print(f"Rooms CSV not found: {args.rooms_csv}", file=sys.stderr)
        return 1

    _, always_exclude = read_rooms(args.rooms_csv)
    id_joins = load_id_joins(args.join_id_csv)
    if id_joins:
        print(f"Joining {len(id_joins)} ID(s) per {args.join_id_csv}.")
    ignore_times = load_ignore_times(args.ignore_times_csv)
    if not ignore_times.empty:
        print(f"Ignoring {len(ignore_times)} blackout window(s) per {args.ignore_times_csv}.")

    self_reports = load_self_reports(args.self_reports_csv, id_joins, ignore_times)
    self_reports = self_reports[
        ~self_reports["ID"].isin(always_exclude) & (self_reports["Local Time"] >= args.start_time)
    ]
    print(f"Loaded {len(self_reports)} self-report(s) from {self_reports['ID'].nunique()} ID(s) (from {args.start_time} on).")

    gaps_sec = compute_gaps(self_reports)
    if gaps_sec.empty:
        print("Fewer than two reports for every ID - no gaps to plot.", file=sys.stderr)
        return 1

    threshold_sec = args.dedupe_window_min * 60
    below = int((gaps_sec <= threshold_sec).sum())
    print(
        f"{len(gaps_sec)} consecutive-report gap(s); "
        f"median {format_seconds(gaps_sec.median())}, "
        f"{below} ({below / len(gaps_sec):.1%}) at or below the {args.dedupe_window_min:g} min dedupe threshold."
    )

    plot_gap_distribution(gaps_sec, args.bins, args.dedupe_window_min, args.output)
    print(f"Wrote gap distribution to {args.output}")

    if args.gaps_csv is not None:
        gaps_sec.rename("gap_seconds").to_csv(args.gaps_csv, index=False)
        print(f"Wrote raw gap list to {args.gaps_csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
