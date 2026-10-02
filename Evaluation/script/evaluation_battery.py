#!/usr/bin/env python3
"""Plot every beacon's battery voltage over time, on a single figure.

Reads measurements.csv (written by Network_Python_RRT+UART/postprocessing.py -
one row per "Current Timer" readout, carrying that beacon's battery voltage
at that moment) and draws one voltage-over-time line per beacon ID, so the
whole fleet's battery drain is visible at a glance and outliers (a beacon
draining much faster than the rest) stand out.

Every individual readout is plotted as-is (no smoothing or per-day
aggregation) - the series is spiky because a readout's voltage sags several
hundred mV while the radio is transmitting, but these are the real measured
values.

IDs are deliberately not folded via join_ID.csv here: a reissued beacon is a
physically different device with its own battery, so each raw ID keeps its
own curve. Only the reserved special IDs (252-254) are dropped.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib
import pandas as pd

matplotlib.use("Agg")
import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

from evaluation_preparation import (  # noqa: E402
    DEFAULT_CONTACTS_DIR,
    DEFAULT_RESULT_DIR,
    SPECIAL_IDS,
)

DEFAULT_MEASUREMENTS_CSV = DEFAULT_CONTACTS_DIR / "measurements.csv"
DEFAULT_OUTPUT_PNG = DEFAULT_RESULT_DIR / "Batteriespannung_Verlauf.png"
DEFAULT_HISTOGRAM_PNG = DEFAULT_RESULT_DIR / "Beacon_Resets_Histogramm.png"

LOW_BATTERY_MV = 2650  # kept in sync with postprocessing.py's DEFAULT_LOW_BATTERY_MV
DEFAULT_RESET_TOLERANCE_MIN = 10  # kept in sync with evaluation_resets.py's DEFAULT_TOLERANCE_MIN

SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRIDLINE = "#e1e0d9"
LOW_BATTERY_COLOR = "#e34948"
BAR_COLOR = "#3987e5"


def load_measurements(path: Path) -> pd.DataFrame:
    """Load ID / Timestamp / Battery (mV) from measurements.csv, cleaned and sorted.

    Rows without a voltage reading and the reserved special IDs are dropped;
    the result is sorted by (ID, Timestamp) so each ID's row order is its
    timeline.
    """
    df = pd.read_csv(path, usecols=["ID", "Timestamp", "Battery (mV)"])
    df["ID"] = df["ID"].astype("int64")
    df["Timestamp"] = pd.to_datetime(df["Timestamp"])
    df["Battery (mV)"] = pd.to_numeric(df["Battery (mV)"], errors="coerce")
    df = df.dropna(subset=["Battery (mV)"])
    df = df[~df["ID"].isin(SPECIAL_IDS)]
    return df.sort_values(["ID", "Timestamp"]).reset_index(drop=True)


def load_zero_times(path: Path) -> pd.DataFrame:
    """Load ID / Timestamp / Calculated time for 0 timer from measurements.csv, cleaned and sorted.

    Kept separate from load_measurements: a "Current Timer" readout can be
    missing its Voltage line (see postprocessing.py's collect_measurements)
    while still carrying a valid Timer, so reset detection must not skip rows
    that load_measurements drops for lacking a voltage reading - that would
    create gaps in a beacon's timeline and produce false or missed resets.
    """
    df = pd.read_csv(path, usecols=["ID", "Timestamp", "Calculated time for 0 timer"])
    df["ID"] = df["ID"].astype("int64")
    df["Timestamp"] = pd.to_datetime(df["Timestamp"])
    df["Zero Time"] = pd.to_datetime(df["Calculated time for 0 timer"], errors="coerce")
    df = df.dropna(subset=["Zero Time"])
    df = df[~df["ID"].isin(SPECIAL_IDS)]
    return df.sort_values(["ID", "Timestamp"]).reset_index(drop=True)


def count_resets_per_beacon(df: pd.DataFrame, tolerance_min: float) -> pd.DataFrame:
    """Number of clock resets per beacon: how often its Timer counter restarted.

    A beacon's calculated Timer-zero time is stable between resets (see the
    project's ID-mapping notes) - only small readout-to-readout jitter. A
    reboot restarts the Timer counter, so the zero time no longer matches the
    previous stretch; this shows up as it dropping by more than
    tolerance_min versus that beacon's immediately preceding readout (see
    evaluation_resets.py, which this mirrors). df must already be sorted by
    (ID, Timestamp) (see load_zero_times). Returns ID, Resets - every beacon
    in df, including those with 0.
    """
    tolerance = pd.Timedelta(minutes=tolerance_min)
    previous_zero = df.groupby("ID")["Zero Time"].shift(1)
    is_reset = previous_zero.notna() & (df["Zero Time"] < previous_zero - tolerance)

    all_ids = sorted(df["ID"].unique())
    counts = df.loc[is_reset, "ID"].value_counts().reindex(all_ids, fill_value=0)
    counts.index.name = "ID"
    return counts.reset_index(name="Resets")


def plot_battery_over_time(df: pd.DataFrame, output_path: Path, annotate_ids: bool) -> None:
    ids = sorted(df["ID"].unique())
    cmap = matplotlib.colormaps["turbo"]
    span = max(len(ids) - 1, 1)

    fig, ax = plt.subplots(figsize=(14, 8), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    for index, beacon_id in enumerate(ids):
        group = df[df["ID"] == beacon_id]
        color = cmap(index / span)
        ax.plot(
            group["Timestamp"],
            group["Battery (mV)"],
            color=color,
            linewidth=0.9,
            alpha=0.8,
            marker="o",
            markersize=1.5,
        )
        if annotate_ids:
            last = group.iloc[-1]
            ax.annotate(
                str(beacon_id),
                (last["Timestamp"], last["Battery (mV)"]),
                xytext=(3, 0),
                textcoords="offset points",
                fontsize=4.5,
                color=color,
                va="center",
            )

    ax.axhline(LOW_BATTERY_MV, color=LOW_BATTERY_COLOR, linewidth=1.3, linestyle="--")
    ax.text(
        ax.get_xlim()[0],
        LOW_BATTERY_MV,
        f" Low-Battery-Schwelle ({LOW_BATTERY_MV} mV)",
        color=LOW_BATTERY_COLOR,
        fontsize=8.5,
        va="bottom",
    )

    ax.xaxis.set_major_locator(mdates.AutoDateLocator())
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%d.%m."))
    ax.tick_params(length=0, colors=INK_SECONDARY, labelsize=8.5)
    for spine in ax.spines.values():
        spine.set_color(GRIDLINE)
    ax.grid(color=GRIDLINE, linewidth=0.7)
    ax.set_axisbelow(True)

    ax.set_xlabel("Datum", color=INK_SECONDARY, fontsize=9.5)
    ax.set_ylabel("Batteriespannung (mV)", color=INK_SECONDARY, fontsize=9.5)
    ax.set_title(
        f"Batteriespannung über die Zeit ({len(ids)} Beacons)",
        color=INK_PRIMARY,
        fontsize=11,
        pad=12,
    )

    fig.autofmt_xdate()
    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def plot_reset_histogram(counts: pd.DataFrame, output_path: Path) -> None:
    """Bar chart of the number of clock resets (Timer restarts) per beacon ID.

    counts is the output of count_resets_per_beacon: one bar per beacon,
    including beacons that never reset (bar height 0).
    """
    counts = counts.sort_values("ID")

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
        description="Plot every beacon's battery voltage over time from measurements.csv."
    )
    parser.add_argument(
        "--measurements-csv",
        type=Path,
        default=DEFAULT_MEASUREMENTS_CSV,
        help=f"Path to measurements.csv (written by postprocessing.py). Default: {DEFAULT_MEASUREMENTS_CSV}",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_OUTPUT_PNG,
        help=f"Output PNG path. Default: {DEFAULT_OUTPUT_PNG}",
    )
    parser.add_argument(
        "--start-time",
        type=pd.Timestamp,
        default=None,
        help="Optional: ignore readouts before this local timestamp (default: plot the full history).",
    )
    parser.add_argument(
        "--annotate-ids",
        action="store_true",
        help="Label each line with its beacon ID at its last point (dense with the full fleet, off by default).",
    )
    parser.add_argument(
        "--histogram-output",
        type=Path,
        default=DEFAULT_HISTOGRAM_PNG,
        help=f"Output PNG path for the per-ID reset-count histogram. Default: {DEFAULT_HISTOGRAM_PNG}",
    )
    parser.add_argument(
        "--reset-tolerance-min",
        type=float,
        default=DEFAULT_RESET_TOLERANCE_MIN,
        help=(
            "A zero-time drop of at most this many minutes vs. the beacon's previous readout "
            f"is treated as clock jitter, not a reset. Default: {DEFAULT_RESET_TOLERANCE_MIN}"
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.histogram_output.parent.mkdir(parents=True, exist_ok=True)

    if not args.measurements_csv.exists():
        print(f"Measurements CSV not found: {args.measurements_csv}", file=sys.stderr)
        return 1

    df = load_measurements(args.measurements_csv)
    if args.start_time is not None:
        df = df[df["Timestamp"] >= args.start_time]
    if df.empty:
        print("No battery readings to plot.", file=sys.stderr)
        return 1

    print(
        f"Loaded {len(df)} battery reading(s) from {df['ID'].nunique()} beacon(s), "
        f"{df['Timestamp'].min()} to {df['Timestamp'].max()}."
    )
    below = df[df["Battery (mV)"] < LOW_BATTERY_MV]["ID"].nunique()
    if below:
        print(f"{below} beacon(s) recorded a reading below the {LOW_BATTERY_MV} mV low-battery threshold.")

    plot_battery_over_time(df, args.output, args.annotate_ids)
    print(f"Wrote battery voltage plot to {args.output}")

    zero_times = load_zero_times(args.measurements_csv)
    if args.start_time is not None:
        zero_times = zero_times[zero_times["Timestamp"] >= args.start_time]
    if not zero_times.empty:
        reset_counts = count_resets_per_beacon(zero_times, args.reset_tolerance_min)
        print(
            f"{int(reset_counts['Resets'].sum())} reset(s) detected across "
            f"{int((reset_counts['Resets'] > 0).sum())} of {len(reset_counts)} beacon(s)."
        )
        plot_reset_histogram(reset_counts, args.histogram_output)
        print(f"Wrote per-ID reset-count histogram to {args.histogram_output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
