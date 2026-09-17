#!/usr/bin/env python3
"""Build a time-varying contact graph from contacts_*.csv files and write it to CSV.

Reads every contacts_*.csv in a directory (see postprocessing.py, which writes
one such file per calendar day with columns ID1,ID2,RSSI,Contact Local Time).
ID1/ID2 are symmetric - a row "A,B,..." and a row "B,A,..." describe the same
link - so events are first regrouped onto an unordered pair. Within a pair,
consecutive events with RSSI stronger than --rssi-threshold and no more than
--max-gap-seconds apart are merged into one continuous contact session; each
session becomes one edge of the graph, valid for [Start Local Time, End Local
Time]. A gap of --max-gap-seconds or more (or a contact falling at/below the
RSSI threshold) breaks the session.

Two CSVs are written:
- the edges CSV: one row per contact session (the time-varying edges)
- the nodes CSV: one row per ID seen in the input (first/last seen), useful
  for a visualization to also place IDs that never had a qualifying contact

--rooms-csv (optional) excludes every ID listed in that rooms CSV (see
evaluation_preparation.read_rooms - same Label/ID-prefix handling as the
evaluation_*.py scripts) entirely, from both nodes and edges - for a
person-only contact graph with no room nodes at all, rather than the default
graph where rooms show up as nodes like anyone else.

--start-time drops contact rows before that local timestamp, same default
(evaluation_preparation.DEFAULT_START_TIME - deployment go-live) as the
evaluation_*.py scripts, so the graph covers the same window as the rest of
the evaluation instead of also including setup/test noise.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd

from evaluation_preparation import DEFAULT_ROOMS_CSV, DEFAULT_START_TIME, read_rooms

TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S"
DEFAULT_DATA_DIR = Path(__file__).resolve().parent.parent.parent / "Network_Python_RRT+UART" / "Output"
DEFAULT_RESULT_DIR = Path(__file__).resolve().parent.parent / "result"
DEFAULT_PATTERN = "contacts_*.csv"
DEFAULT_RSSI_THRESHOLD = -70
DEFAULT_MAX_GAP_SECONDS = 30
DEFAULT_EDGES_CSV = "graph_mit_raeumen_edges.csv"
DEFAULT_NODES_CSV = "graph_mit_raeumen_nodes.csv"

EDGE_COLUMNS = [
    "ID1",
    "ID2",
    "Start Local Time",
    "End Local Time",
    "Duration (s)",
    "Contact Count",
    "Min RSSI",
    "Max RSSI",
    "Mean RSSI",
]


def read_contacts(data_dir: Path, pattern: str) -> pd.DataFrame:
    paths = sorted(data_dir.glob(pattern))
    if not paths:
        raise FileNotFoundError(f"No files matching {pattern!r} found in {data_dir}")

    frames = [
        pd.read_csv(
            path,
            dtype={"ID1": "int32", "ID2": "int32", "RSSI": "int16"},
            parse_dates=["Contact Local Time"],
            date_format=TIMESTAMP_FORMAT,
        )
        for path in paths
    ]
    df = pd.concat(frames, ignore_index=True)
    return df.sort_values("Contact Local Time", kind="mergesort").reset_index(drop=True)


def build_node_table(df: pd.DataFrame) -> pd.DataFrame:
    """One row per ID seen as either endpoint, first/last seen across all rows.

    Unlike build_edge_sessions, this ignores the RSSI threshold: a node is a
    device that appeared in the log at all, not just one with a qualifying
    (strong-enough, frequent-enough) contact.
    """
    long = pd.concat(
        [
            df[["ID1", "Contact Local Time"]].rename(columns={"ID1": "ID"}),
            df[["ID2", "Contact Local Time"]].rename(columns={"ID2": "ID"}),
        ],
        ignore_index=True,
    )
    nodes = long.groupby("ID")["Contact Local Time"].agg(["min", "max"]).reset_index()
    nodes.columns = ["ID", "First Seen Local Time", "Last Seen Local Time"]
    return nodes.sort_values("ID").reset_index(drop=True)


def build_edge_sessions(df: pd.DataFrame, rssi_threshold: int, max_gap_seconds: int) -> pd.DataFrame:
    """Collapse qualifying contact events into continuous per-pair contact sessions (edges)."""
    strong = df.loc[df["RSSI"] > rssi_threshold, ["ID1", "ID2", "RSSI", "Contact Local Time"]].copy()
    if strong.empty:
        return pd.DataFrame(columns=EDGE_COLUMNS)

    # Normalize onto an unordered pair since ID1/ID2 are symmetric.
    strong["Pair ID1"] = np.minimum(strong["ID1"], strong["ID2"])
    strong["Pair ID2"] = np.maximum(strong["ID1"], strong["ID2"])
    strong = strong.sort_values(["Pair ID1", "Pair ID2", "Contact Local Time"], kind="mergesort")

    # diff() is NaN at the first row of every (Pair ID1, Pair ID2) group, so a
    # plain global cumsum (no need to regroup) already bumps the session
    # counter at every pair boundary as well as every too-large gap; the
    # counter's absolute value doesn't matter, only that it's constant within
    # a session and distinct between sessions of the *same* pair, which the
    # (Pair ID1, Pair ID2, Session) groupby key below guarantees.
    gap = strong.groupby(["Pair ID1", "Pair ID2"])["Contact Local Time"].diff()
    new_session = gap.isna() | (gap > pd.Timedelta(seconds=max_gap_seconds))
    strong["Session"] = new_session.cumsum()

    sessions = (
        strong.groupby(["Pair ID1", "Pair ID2", "Session"])
        .agg(
            **{
                "Start Local Time": ("Contact Local Time", "min"),
                "End Local Time": ("Contact Local Time", "max"),
                "Contact Count": ("RSSI", "size"),
                "Min RSSI": ("RSSI", "min"),
                "Max RSSI": ("RSSI", "max"),
                "Mean RSSI": ("RSSI", "mean"),
            }
        )
        .reset_index()
        .rename(columns={"Pair ID1": "ID1", "Pair ID2": "ID2"})
    )
    sessions["Duration (s)"] = (sessions["End Local Time"] - sessions["Start Local Time"]).dt.total_seconds()
    sessions["Mean RSSI"] = sessions["Mean RSSI"].round(1)
    sessions = sessions.sort_values(["Start Local Time", "ID1", "ID2"], kind="mergesort").reset_index(drop=True)
    return sessions[EDGE_COLUMNS]


def write_edges_csv(path: Path, edges: pd.DataFrame) -> None:
    formatted = edges.copy()
    formatted["Start Local Time"] = formatted["Start Local Time"].dt.strftime(TIMESTAMP_FORMAT)
    formatted["End Local Time"] = formatted["End Local Time"].dt.strftime(TIMESTAMP_FORMAT)
    formatted.to_csv(path, index=False)


def write_nodes_csv(path: Path, nodes: pd.DataFrame) -> None:
    formatted = nodes.copy()
    formatted["First Seen Local Time"] = formatted["First Seen Local Time"].dt.strftime(TIMESTAMP_FORMAT)
    formatted["Last Seen Local Time"] = formatted["Last Seen Local Time"].dt.strftime(TIMESTAMP_FORMAT)
    formatted.to_csv(path, index=False)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a time-varying contact graph (nodes + time-bounded edges) from contacts_*.csv files."
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=DEFAULT_DATA_DIR,
        help=f"Directory containing the contact CSV files. Default: {DEFAULT_DATA_DIR}",
    )
    parser.add_argument(
        "--pattern",
        default=DEFAULT_PATTERN,
        help=f"Glob pattern (within --data-dir) matching the contact CSV files. Default: {DEFAULT_PATTERN}",
    )
    parser.add_argument(
        "--rssi-threshold",
        type=int,
        default=DEFAULT_RSSI_THRESHOLD,
        help=(
            "A contact only counts towards an edge if its RSSI is strictly "
            f"stronger than (greater than) this value. Default: {DEFAULT_RSSI_THRESHOLD}"
        ),
    )
    parser.add_argument(
        "--max-gap-seconds",
        type=int,
        default=DEFAULT_MAX_GAP_SECONDS,
        help=(
            "Two consecutive qualifying contacts between the same pair belong to "
            "the same edge/session if no more than this many seconds apart; a "
            f"larger gap ends the session. Default: {DEFAULT_MAX_GAP_SECONDS}"
        ),
    )
    parser.add_argument(
        "--start-time",
        type=pd.Timestamp,
        default=DEFAULT_START_TIME,
        help=(
            "Ignore contact events before this local timestamp (deployment go-live; earlier "
            f"rows are setup/test noise) - same default as the evaluation_*.py scripts. Default: {DEFAULT_START_TIME}"
        ),
    )
    parser.add_argument(
        "--rooms-csv",
        type=Path,
        default=None,
        help=(
            "If given, exclude every ID listed in this rooms CSV (see list_rooms.csv) entirely - "
            "no room nodes, no room-involving edges. Default: not set (rooms are kept as regular "
            f"nodes). Pass '{DEFAULT_ROOMS_CSV}' for a person-only graph."
        ),
    )
    parser.add_argument(
        "--edges-csv",
        type=Path,
        default=None,
        help=f"Output path for the edges CSV. Default: {DEFAULT_RESULT_DIR}/{DEFAULT_EDGES_CSV}",
    )
    parser.add_argument(
        "--nodes-csv",
        type=Path,
        default=None,
        help=f"Output path for the nodes CSV. Default: {DEFAULT_RESULT_DIR}/{DEFAULT_NODES_CSV}",
    )
    args = parser.parse_args(argv)
    if args.edges_csv is None:
        args.edges_csv = DEFAULT_RESULT_DIR / DEFAULT_EDGES_CSV
    if args.nodes_csv is None:
        args.nodes_csv = DEFAULT_RESULT_DIR / DEFAULT_NODES_CSV
    args.edges_csv.parent.mkdir(parents=True, exist_ok=True)
    args.nodes_csv.parent.mkdir(parents=True, exist_ok=True)
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    print(f"Reading contact files matching {args.pattern!r} from {args.data_dir} ...")
    df = read_contacts(args.data_dir, args.pattern)
    print(
        f"Loaded {len(df)} contact rows spanning "
        f"{df['Contact Local Time'].min()} to {df['Contact Local Time'].max()}"
    )

    before_cutoff = len(df)
    df = df[df["Contact Local Time"] >= args.start_time]
    print(f"Dropped {before_cutoff - len(df)} row(s) before start time {args.start_time}.")

    if args.rooms_csv is not None:
        id_to_room_name, _always_exclude = read_rooms(args.rooms_csv)
        room_ids = set(id_to_room_name)
        before = len(df)
        df = df[~df["ID1"].isin(room_ids) & ~df["ID2"].isin(room_ids)]
        print(f"Excluding {len(room_ids)} room ID(s) per {args.rooms_csv}: dropped {before - len(df)} row(s).")

    nodes = build_node_table(df)
    write_nodes_csv(args.nodes_csv, nodes)
    print(f"Wrote {len(nodes)} node(s) to {args.nodes_csv}")

    edges = build_edge_sessions(df, args.rssi_threshold, args.max_gap_seconds)
    write_edges_csv(args.edges_csv, edges)
    print(
        f"Wrote {len(edges)} edge/contact-session(s) "
        f"(RSSI > {args.rssi_threshold}, max gap {args.max_gap_seconds}s) to {args.edges_csv}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
