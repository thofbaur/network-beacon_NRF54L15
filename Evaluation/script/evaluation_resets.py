#!/usr/bin/env python3
"""Detect and count beacon clock resets from measurements.csv.

Every "Current Timer" readout in measurements.csv carries a "Calculated time
for 0 timer" (see Network_Log+Postprocessing/postprocessing.py): the local
time at which that beacon's own Timer counter would have read 0, i.e. its
last reboot/clock-reset moment. Between resets this value is stable for a
given beacon (see the project's ID-mapping notes) - it only drifts by small
amounts readout to readout. A reboot restarts the Timer counter, so the next
readout's zero time no longer matches the previous stretch; this shows up as
the zero time jumping backwards by more than a small tolerance (10 minutes by
default) compared to that beacon's immediately preceding readout.

IDs are deliberately not folded via join_ID.csv here, matching
evaluation_battery.py's reasoning: a reset is a property of the physical
device's own clock, not of whoever is currently carrying it. Only the
reserved special IDs (252-254) are dropped.

Writes two CSVs and one bar chart to the result directory:
- beacon_reset_counts.csv: ID, Resets - one row per beacon (0 if it never
  reset), feeding the bar chart.
- beacon_resets.csv: ID, Detected At, Previous Zero Time, New Zero Time - one
  row per detected reset event, listing exactly when (by new zero time) each
  beacon reset.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib
import pandas as pd

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from evaluation_preparation import (  # noqa: E402
    DEFAULT_CONTACTS_DIR,
    DEFAULT_RESULT_DIR,
    SPECIAL_IDS,
)

DEFAULT_MEASUREMENTS_CSV = DEFAULT_CONTACTS_DIR / "measurements.csv"
DEFAULT_OUTPUT_PNG = DEFAULT_RESULT_DIR / "Beacon_Resets.png"
DEFAULT_COUNTS_CSV = DEFAULT_RESULT_DIR / "beacon_reset_counts.csv"
DEFAULT_RESETS_CSV = DEFAULT_RESULT_DIR / "beacon_resets.csv"

DEFAULT_TOLERANCE_MIN = 10  # zero-time jitter within this is drift, not a reset

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRIDLINE = "#e1e0d9"
BAR_COLOR = "#3987e5"


def load_measurements(path: Path) -> pd.DataFrame:
    """Load ID / Timestamp / Zero Time from measurements.csv, cleaned and sorted.

    Rows without a calculated zero time and the reserved special IDs are
    dropped; the result is sorted by (ID, Timestamp) so each ID's row order
    is its timeline.
    """
    df = pd.read_csv(path, usecols=["ID", "Timestamp", "Calculated time for 0 timer"])
    df["ID"] = df["ID"].astype("int64")
    df["Timestamp"] = pd.to_datetime(df["Timestamp"])
    df["Zero Time"] = pd.to_datetime(df["Calculated time for 0 timer"], errors="coerce")
    df = df.dropna(subset=["Zero Time"])
    df = df[~df["ID"].isin(SPECIAL_IDS)]
    return df.sort_values(["ID", "Timestamp"]).reset_index(drop=True)


def find_resets(df: pd.DataFrame, tolerance_min: float) -> pd.DataFrame:
    """One row per detected reset: a beacon's zero time dropping by more than tolerance_min vs. its own previous readout.

    df must already be sorted by (ID, Timestamp) (see load_measurements).
    Returns ID, Detected At (the readout timestamp the reset shows up at),
    Previous Zero Time, New Zero Time - sorted the same way.
    """
    tolerance = pd.Timedelta(minutes=tolerance_min)
    previous_zero = df.groupby("ID")["Zero Time"].shift(1)
    is_reset = previous_zero.notna() & (df["Zero Time"] < previous_zero - tolerance)

    resets = df.loc[is_reset, ["ID", "Timestamp", "Zero Time"]].copy()
    resets["Previous Zero Time"] = previous_zero[is_reset]
    resets = resets.rename(columns={"Timestamp": "Detected At", "Zero Time": "New Zero Time"})
    return resets[["ID", "Detected At", "Previous Zero Time", "New Zero Time"]].reset_index(drop=True)


def count_resets_per_beacon(all_ids: list[int], resets: pd.DataFrame) -> pd.DataFrame:
    """Resets per beacon, including every beacon with 0. Returns ID, Resets, sorted by ID."""
    counts = resets.groupby("ID").size().reindex(all_ids, fill_value=0)
    counts.index.name = "ID"
    return counts.reset_index(name="Resets")


def plot_reset_counts(counts: pd.DataFrame, output_path: Path) -> None:
    fig, ax = plt.subplots(figsize=(max(10, len(counts) * 0.22), 6), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    positions = range(len(counts))
    ax.bar(positions, counts["Resets"], color=BAR_COLOR, width=0.7)

    ax.set_xticks(list(positions))
    ax.set_xticklabels(counts["ID"].astype(str), fontsize=7, color=INK_SECONDARY, rotation=90)
    ax.yaxis.get_major_locator().set_params(integer=True)
    ax.tick_params(length=0, colors=INK_SECONDARY, labelsize=8.5)
    for spine in ax.spines.values():
        spine.set_color(GRIDLINE)
    ax.grid(axis="y", color=GRIDLINE, linewidth=0.7)
    ax.set_axisbelow(True)

    ax.set_xlabel("Beacon-ID", color=INK_SECONDARY, fontsize=9.5)
    ax.set_ylabel("Anzahl Resets", color=INK_SECONDARY, fontsize=9.5)
    total_resets = int(counts["Resets"].sum())
    reset_beacons = int((counts["Resets"] > 0).sum())
    ax.set_title(
        f"Beacon-Resets ({total_resets} Reset(s) auf {reset_beacons} von {len(counts)} Beacons)",
        color=INK_PRIMARY,
        fontsize=11,
        pad=12,
    )

    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Detect beacon clock resets (Timer restarts) from measurements.csv, count them per beacon and list when they happened."
    )
    parser.add_argument(
        "--measurements-csv",
        type=Path,
        default=DEFAULT_MEASUREMENTS_CSV,
        help=f"Path to measurements.csv (written by postprocessing.py). Default: {DEFAULT_MEASUREMENTS_CSV}",
    )
    parser.add_argument(
        "--tolerance-min",
        type=float,
        default=DEFAULT_TOLERANCE_MIN,
        help=(
            "A zero-time drop of at most this many minutes vs. the beacon's previous readout "
            f"is treated as clock jitter, not a reset. Default: {DEFAULT_TOLERANCE_MIN}"
        ),
    )
    parser.add_argument(
        "--output", type=Path, default=DEFAULT_OUTPUT_PNG, help=f"Output PNG path. Default: {DEFAULT_OUTPUT_PNG}"
    )
    parser.add_argument(
        "--counts-csv",
        type=Path,
        default=DEFAULT_COUNTS_CSV,
        help=f"Output CSV path for per-beacon reset counts. Default: {DEFAULT_COUNTS_CSV}",
    )
    parser.add_argument(
        "--resets-csv",
        type=Path,
        default=DEFAULT_RESETS_CSV,
        help=f"Output CSV path for the per-event reset list (new zero time per reset). Default: {DEFAULT_RESETS_CSV}",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.counts_csv.parent.mkdir(parents=True, exist_ok=True)
    args.resets_csv.parent.mkdir(parents=True, exist_ok=True)

    if not args.measurements_csv.exists():
        print(f"Measurements CSV not found: {args.measurements_csv}", file=sys.stderr)
        return 1

    df = load_measurements(args.measurements_csv)
    if df.empty:
        print("No measurements with a calculated zero time to check.", file=sys.stderr)
        return 1

    all_ids = sorted(df["ID"].unique())
    print(f"Loaded {len(df)} readout(s) from {len(all_ids)} beacon(s).")

    resets = find_resets(df, args.tolerance_min)
    counts = count_resets_per_beacon(all_ids, resets)

    counts.to_csv(args.counts_csv, index=False)
    resets.to_csv(args.resets_csv, index=False)
    print(f"Wrote per-beacon reset counts to {args.counts_csv}")
    print(f"Wrote {len(resets)} reset event(s) to {args.resets_csv}")

    reset_beacons = int((counts["Resets"] > 0).sum())
    print(f"{len(resets)} reset(s) detected across {reset_beacons} of {len(all_ids)} beacon(s).")

    plot_reset_counts(counts, args.output)
    print(f"Wrote reset-count chart to {args.output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
