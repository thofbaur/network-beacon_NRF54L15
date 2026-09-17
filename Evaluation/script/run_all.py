#!/usr/bin/env python3
"""Run every script in this folder, in sequence, with each script's own default arguments.

Every other script here exposes main(argv: list[str]) -> int (0 = success);
this just imports each one and calls its main([]) in turn (in-process, no
subprocess/CLI-quoting overhead) and prints a pass/fail summary at the end. A
failing step is reported and does not stop the run - one broken script
shouldn't hide the results of every other one.

Order matters for the graph steps: build-graph(-no-rooms) writes the
edges/nodes CSV that graph-video(-no-rooms) reads, so each pair always runs
build before video.

build_contact_graph.py/visualize_contact_graph.py are a separate, older
pipeline that doesn't go through evaluation_preparation.py for its
contacts_*.csv reading - they don't apply join_ID.csv or ignore_times.csv the
way the evaluation_*.py scripts do. build-graph-no-rooms does use
evaluation_preparation.read_rooms (via build_contact_graph.py's --rooms-csv)
to exclude every room ID entirely, for a person-only graph with no room
nodes - unlike build-graph's default graph, where rooms show up as regular
nodes.

occupancy and graph-video render a full video over the whole dataset and can
take a long time (tens of minutes) once contacts_*.csv covers the full
deployment - see --skip-slow / --only / --skip to leave those out of a quick
run.
"""

from __future__ import annotations

import argparse
import sys
import time

import build_contact_graph
import evaluation_battery
import evaluation_heatmap_zeit_tag
import evaluation_heatmaps
import evaluation_occupancy
import evaluation_selfreport
import evaluation_selfreport_gaps
import visualize_contact_graph

NO_ROOMS_EDGES_CSV = str(build_contact_graph.DEFAULT_RESULT_DIR / "graph_ohne_raeume_edges.csv")
NO_ROOMS_NODES_CSV = str(build_contact_graph.DEFAULT_RESULT_DIR / "graph_ohne_raeume_nodes.csv")
NO_ROOMS_VIDEO = str(build_contact_graph.DEFAULT_RESULT_DIR / "graph_ohne_raeume.mp4")

# (name, module, human label, is a slow video render, argv to call module.main() with)
STEPS = [
    ("battery", evaluation_battery, "Batteriespannung über die Zeit", False, []),
    ("heatmaps", evaluation_heatmaps, "Personen-/Raum-/Zeit-Heatmaps", False, []),
    ("heatmap-zeit-tag", evaluation_heatmap_zeit_tag, "Kontaktzeit je Tag", False, []),
    ("selfreport", evaluation_selfreport, "Self-Report-Heatmap", False, []),
    ("selfreport-gaps", evaluation_selfreport_gaps, "Self-Report-Abstandsverteilung", False, []),
    ("occupancy", evaluation_occupancy, "Room-Occupancy-Video", True, []),
    #("build-graph", build_contact_graph, "Kontaktgraph-CSV (edges/nodes)", False, []),
    #("graph-video", visualize_contact_graph, "Kontaktgraph-Video", True, []),
    (
        "build-graph-no-rooms",
        build_contact_graph,
        "Kontaktgraph-CSV ohne Raum-Knoten (edges/nodes)",
        False,
        [
            "--rooms-csv", str(build_contact_graph.DEFAULT_ROOMS_CSV),
            "--edges-csv", NO_ROOMS_EDGES_CSV,
            "--nodes-csv", NO_ROOMS_NODES_CSV,
        ],
    ),
    (
        "graph-video-no-rooms",
        visualize_contact_graph,
        "Kontaktgraph-Video ohne Raum-Knoten",
        True,
        ["--edges-csv", NO_ROOMS_EDGES_CSV, "--nodes-csv", NO_ROOMS_NODES_CSV, "--output", NO_ROOMS_VIDEO],
    ),
]
STEP_NAMES = [name for name, *_ in STEPS]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run every evaluation script with its own defaults, in sequence.")
    parser.add_argument(
        "--only",
        nargs="+",
        choices=STEP_NAMES,
        default=None,
        metavar="STEP",
        help=f"Run only these steps, instead of all of them. Choices: {', '.join(STEP_NAMES)}",
    )
    parser.add_argument(
        "--skip",
        nargs="+",
        choices=STEP_NAMES,
        default=[],
        metavar="STEP",
        help=f"Skip these steps. Choices: {', '.join(STEP_NAMES)}",
    )
    parser.add_argument(
        "--skip-slow",
        action="store_true",
        help="Skip the slow video-rendering steps (occupancy, graph-video) - for a quick refresh of just the plots.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    selected = set(args.only) if args.only is not None else set(STEP_NAMES)
    selected -= set(args.skip)
    if args.skip_slow:
        selected -= {name for name, _module, _label, slow, _argv in STEPS if slow}

    results: list[tuple[str, str, bool, float]] = []
    for name, module, label, _slow, step_argv in STEPS:
        if name not in selected:
            continue
        print(f"\n{'=' * 70}\n{label} ({name})\n{'=' * 70}")
        start = time.monotonic()
        try:
            ok = module.main(step_argv) == 0
        except Exception as exc:  # a broken step shouldn't take the rest of the run down with it
            print(f"  -> {name} raised {exc!r}", file=sys.stderr)
            ok = False
        elapsed = time.monotonic() - start
        results.append((name, label, ok, elapsed))
        print(f"  -> {'OK' if ok else 'FAILED'} ({elapsed:.0f}s)")

    print(f"\n{'=' * 70}\nSummary\n{'=' * 70}")
    if not results:
        print("  Nothing selected to run (check --only/--skip/--skip-slow).")
        return 1
    for name, label, ok, elapsed in results:
        print(f"  {'OK    ' if ok else 'FAILED'}  {elapsed:7.0f}s  {label} ({name})")

    return 0 if all(ok for _name, _label, ok, _elapsed in results) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
