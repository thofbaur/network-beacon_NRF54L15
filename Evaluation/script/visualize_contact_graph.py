#!/usr/bin/env python3
"""Render the time-varying contact graph (from build_contact_graph.py) as a video.

Node positions are driven by a live force-directed simulation: every frame
runs a few more spring-layout iterations, warm-started from the previous
frame's positions, using only that frame's active edges as attraction and all
nodes repelling each other. That makes nodes drift slowly towards whoever
they're currently in contact with and drift apart again once contact ends,
instead of either sitting at fixed coordinates all video or jumping to a
freshly recomputed layout each frame. --layout-iterations controls how much
each frame nudges positions towards the current forces - fewer iterations
means slower, smoother drift; more means positions catch up faster and can
look jumpy.

At each frame time t, an edge is drawn if t falls within
[Start Local Time, effective End Local Time], where the effective end is
padded up to at least --min-display-seconds after the start. Without this
padding, a session made of a single contact event (Start == End) would only
ever be active at the one frame that happens to land exactly on it - usually
none - and would never appear in the video at all.

An optional --rooms-csv (ID -> Raumnummer) marks some IDs as fixed room
anchors instead of free-drifting nodes: every ID mapped to the same room name
is pinned to that room's single position on a large circle (so they overlap -
they *are* the same physical room), and only one representative ID per room
is labelled, with the room name rather than the ID. Rows whose Raumnummer is
"DYNAMIC" describe a mobile tag misusing that column for bookkeeping, not a
room, so those IDs are left free to drift - confined to the interior of the
room circle - like any ID absent from the rooms CSV. Contacts between two
room anchors are never drawn: stationary room beacons picking each other up
is just background noise, not a meaningful "contact" to animate.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from datetime import datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import networkx as nx
import numpy as np
import pandas as pd

TIMESTAMP_FORMAT = "%Y-%m-%d %H:%M:%S"
SCRIPT_DIR = Path(__file__).resolve().parent
EVALUATION_DIR = SCRIPT_DIR.parent
RESULT_DIR = EVALUATION_DIR / "result"
DEFAULT_EDGES_CSV = RESULT_DIR / "graph_mit_raeumen_edges.csv"
DEFAULT_NODES_CSV = RESULT_DIR / "graph_mit_raeumen_nodes.csv"
DEFAULT_ROOMS_CSV = EVALUATION_DIR / "list_rooms.csv"
DEFAULT_OUTPUT = RESULT_DIR / "graph_mit_raeumen.mp4"
DYNAMIC_ROOM_LABEL = "DYNAMIC"
DEFAULT_STEP_SECONDS = 120
DEFAULT_MIN_DISPLAY_SECONDS = 60
DEFAULT_FPS = 10
DEFAULT_NODE_SIZE = 150
DEFAULT_DPI = 100
DEFAULT_SEED = 42
DEFAULT_LAYOUT_ITERATIONS = 3
DEFAULT_INITIAL_LAYOUT_ITERATIONS = 50
ROOM_CIRCLE_RADIUS = 1.0
FREE_NODE_RADIUS_MARGIN = 0.85

INACTIVE_NODE_COLOR = "#B0B0B0"
ACTIVE_NODE_COLOR = "#E4572E"
EDGE_COLOR = "#4C6EF5"


def load_edges(path: Path) -> pd.DataFrame:
    edges = pd.read_csv(
        path,
        parse_dates=["Start Local Time", "End Local Time"],
        date_format=TIMESTAMP_FORMAT,
    )
    if edges.empty:
        raise ValueError(f"{path} contains no edges - nothing to animate")
    return edges


def load_node_ids(nodes_csv: Path | None, edges: pd.DataFrame) -> list[int]:
    if nodes_csv is not None and nodes_csv.exists():
        nodes = pd.read_csv(nodes_csv, usecols=["ID"])
        ids = set(nodes["ID"].tolist())
    else:
        ids = set()
    ids |= set(edges["ID1"].tolist()) | set(edges["ID2"].tolist())
    return sorted(ids)


def compute_initial_layout(node_ids: list[int], edges: pd.DataFrame, k: float, seed: int) -> dict[int, np.ndarray]:
    """Settle an initial layout on the whole run's edges (weighted by total contact count).

    This only sets a reasonable, non-overlapping starting point for the live
    per-frame simulation in main()'s update() - it does not stay fixed once
    the animation starts.
    """
    graph = nx.Graph()
    graph.add_nodes_from(node_ids)
    weights = edges.groupby(["ID1", "ID2"])["Contact Count"].sum()
    for (id1, id2), weight in weights.items():
        graph.add_edge(id1, id2, weight=float(weight))
    return nx.spring_layout(graph, k=k, weight="weight", iterations=DEFAULT_INITIAL_LAYOUT_ITERATIONS, seed=seed)


def load_room_assignments(path: Path) -> dict[int, str] | None:
    """Read an optional ID -> Raumnummer mapping, or None if path doesn't exist.

    Values are whitespace-stripped: the source file mixes tabs and runs of
    spaces as the separator after the comma (e.g. one row uses "125,    2.4"
    instead of a tab), so a plain split on "," followed by a strip is more
    robust here than relying on a single fixed separator. Some IDs carry a
    non-numeric prefix (e.g. "BTN30", "XTN2" for the dynamic/mobile ones) -
    only the digits are kept, so "BTN30" reads as ID 30.
    """
    if not path.exists():
        return None
    raw = pd.read_csv(path, dtype=str)
    raw.columns = [column.strip() for column in raw.columns]
    ids = raw["ID"].str.strip().str.replace(r"\D", "", regex=True).astype(int)
    rooms = raw["Raumnummer"].str.strip()
    return dict(zip(ids, rooms))


def compute_room_positions(room_names: list[str], radius: float = ROOM_CIRCLE_RADIUS) -> dict[str, np.ndarray]:
    """Evenly spaced positions around a large circle, one point per unique room name.

    room_names is expected in first-seen order from the rooms CSV, so rooms
    that are listed near each other (e.g. the same floor) land next to each
    other on the circle too - there's no real floor-plan coordinate to place
    them at instead. Free (non-room) nodes are kept to the circle's interior
    (see free_node_radius_limit in main()), so the rooms read as a ring
    around whatever's currently moving between them.
    """
    angles = np.linspace(0.0, 2 * np.pi, len(room_names), endpoint=False)
    return {name: radius * np.array([np.cos(angle), np.sin(angle)]) for name, angle in zip(room_names, angles)}


def resolve_writer(output: Path, fps: int) -> tuple[animation.AbstractMovieWriter, Path]:
    """Pick a movie writer for output, falling back to GIF if no ffmpeg is available."""
    if output.suffix.lower() == ".gif":
        return animation.PillowWriter(fps=fps), output

    try:
        import imageio_ffmpeg

        plt.rcParams["animation.ffmpeg_path"] = imageio_ffmpeg.get_ffmpeg_exe()
        return animation.FFMpegWriter(fps=fps), output
    except ImportError:
        pass

    if shutil.which("ffmpeg"):
        return animation.FFMpegWriter(fps=fps), output

    print(
        "Neither the imageio-ffmpeg package nor a system ffmpeg were found; "
        "falling back to a GIF instead of an MP4.",
        file=sys.stderr,
    )
    return animation.PillowWriter(fps=fps), output.with_suffix(".gif")


def parse_datetime(value: str) -> datetime:
    return datetime.strptime(value, TIMESTAMP_FORMAT)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render a graph_*_edges.csv (from build_contact_graph.py) as a time-lapse video."
    )
    parser.add_argument("--edges-csv", type=Path, default=DEFAULT_EDGES_CSV, help="Edges CSV to animate.")
    parser.add_argument(
        "--nodes-csv",
        type=Path,
        default=DEFAULT_NODES_CSV,
        help="Optional nodes CSV, so IDs with no qualifying contact still appear. Missing file is ignored.",
    )
    parser.add_argument(
        "--rooms-csv",
        type=Path,
        default=DEFAULT_ROOMS_CSV,
        help=(
            "Optional ID -> Raumnummer CSV. IDs mapped to a real room name are "
            "pinned to a fixed per-room position (same room -> same position) "
            "and labelled with the room name; IDs mapped to "
            f"'{DYNAMIC_ROOM_LABEL}', or missing from this file, keep drifting "
            "freely and stay labelled with their ID. Missing file is ignored."
        ),
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Output video path (.mp4 or .gif).")
    parser.add_argument(
        "--start",
        type=parse_datetime,
        default=None,
        help=f"Animation start time ('{TIMESTAMP_FORMAT.replace('%', '%%')}'). Default: earliest edge start.",
    )
    parser.add_argument(
        "--end",
        type=parse_datetime,
        default=None,
        help=f"Animation end time ('{TIMESTAMP_FORMAT.replace('%', '%%')}'). Default: latest (padded) edge end.",
    )
    parser.add_argument(
        "--step-seconds",
        type=int,
        default=DEFAULT_STEP_SECONDS,
        help=f"Simulated time advanced per frame. Default: {DEFAULT_STEP_SECONDS}",
    )
    parser.add_argument(
        "--min-display-seconds",
        type=int,
        default=DEFAULT_MIN_DISPLAY_SECONDS,
        help=(
            "Minimum time an edge stays visible, padded on from its start if its "
            f"session is shorter. Default: {DEFAULT_MIN_DISPLAY_SECONDS}"
        ),
    )
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS, help=f"Video frame rate. Default: {DEFAULT_FPS}")
    parser.add_argument(
        "--node-size", type=int, default=DEFAULT_NODE_SIZE, help=f"Node marker size. Default: {DEFAULT_NODE_SIZE}"
    )
    parser.add_argument("--dpi", type=int, default=DEFAULT_DPI, help=f"Output resolution (DPI). Default: {DEFAULT_DPI}")
    parser.add_argument(
        "--figsize", type=float, nargs=2, default=(10.0, 8.0), metavar=("WIDTH", "HEIGHT"), help="Figure size in inches."
    )
    parser.add_argument("--no-labels", action="store_true", help="Don't draw ID labels on nodes.")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="Layout random seed, for reproducibility.")
    parser.add_argument(
        "--layout-iterations",
        type=int,
        default=DEFAULT_LAYOUT_ITERATIONS,
        help=(
            "Spring-layout iterations run per frame, warm-started from the previous "
            "frame's positions. Lower = slower/smoother drift, higher = faster, "
            f"jumpier movement. Default: {DEFAULT_LAYOUT_ITERATIONS}"
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)

    edges = load_edges(args.edges_csv)
    node_ids = load_node_ids(args.nodes_csv, edges)
    print(f"Loaded {len(edges)} edge(s) across {len(node_ids)} node(s) from {args.edges_csv}")

    min_display = pd.Timedelta(seconds=args.min_display_seconds)
    edges["Effective End"] = edges["End Local Time"].where(
        edges["End Local Time"] >= edges["Start Local Time"] + min_display,
        edges["Start Local Time"] + min_display,
    )

    start = pd.Timestamp(args.start) if args.start else edges["Start Local Time"].min()
    end = pd.Timestamp(args.end) if args.end else edges["Effective End"].max()
    if end <= start:
        print(f"--end ({end}) must be after --start ({start})", file=sys.stderr)
        return 1

    frame_times = pd.date_range(start, end, freq=f"{args.step_seconds}s")
    print(f"Animating {start} to {end} in {len(frame_times)} frame(s) of {args.step_seconds}s each.")

    # Fixed optimal-distance parameter so consecutive per-frame spring_layout
    # calls in update() pull/push with consistent strength - if k were left to
    # networkx's own default (derived from the *current* graph's node count)
    # it wouldn't change here since node count is constant anyway, but pinning
    # it explicitly keeps the initial settle pass and every per-frame update
    # unambiguously using the same value.
    k = 1 / np.sqrt(len(node_ids))
    pos = compute_initial_layout(node_ids, edges, k, args.seed)

    room_map = load_room_assignments(args.rooms_csv)
    fixed_ids: set[int] = set()
    room_labels: dict[int, str] = {}
    free_node_radius_limit: float | None = None
    if room_map is not None:
        fixed_ids = {
            id_ for id_, room in room_map.items() if room.upper() != DYNAMIC_ROOM_LABEL and id_ in set(node_ids)
        }
        # dict preserves insertion order, and room_map was built from the CSV's
        # own row order, so this dedupes room names while keeping that order.
        room_order = list(dict.fromkeys(room_map[id_] for id_ in room_map if room_map[id_].upper() != DYNAMIC_ROOM_LABEL))
        room_pos = compute_room_positions(room_order)
        for id_ in fixed_ids:
            pos[id_] = room_pos[room_map[id_]]
        room_radius = max((float(np.linalg.norm(p)) for p in room_pos.values()), default=ROOM_CIRCLE_RADIUS)
        free_node_radius_limit = room_radius * FREE_NODE_RADIUS_MARGIN
        # Several IDs share one room's position, so only its lowest ID gets a
        # label - drawing the room name once per ID at the same point would
        # just stack illegible, overlapping text.
        room_label_id: dict[str, int] = {}
        for id_ in sorted(fixed_ids, reverse=True):
            room_label_id[room_map[id_]] = id_
        room_labels = {id_: room for room, id_ in room_label_id.items()}
        print(f"Pinned {len(fixed_ids)} ID(s) to {len(room_order)} room(s) from {args.rooms_csv}")
    elif args.rooms_csv is not None:
        print(f"No rooms CSV found at {args.rooms_csv}; all nodes keep drifting freely.")

    labels = {}
    if not args.no_labels:
        labels = {id_: str(id_) for id_ in node_ids if id_ not in fixed_ids}
        labels.update(room_labels)

    starts = edges["Start Local Time"].to_numpy()
    ends = edges["Effective End"].to_numpy()
    id1s = edges["ID1"].to_numpy()
    id2s = edges["ID2"].to_numpy()
    # Two stationary room beacons picking each other up is background noise,
    # not a contact worth animating, so those edges are dropped up front.
    room_to_room = np.isin(id1s, list(fixed_ids)) & np.isin(id2s, list(fixed_ids))

    node_graph = nx.Graph()
    node_graph.add_nodes_from(node_ids)

    fig, ax = plt.subplots(figsize=args.figsize)
    progress_every = max(1, len(frame_times) // 20)

    def update(frame_index: int):
        ax.clear()
        t = np.datetime64(frame_times[frame_index])
        mask = (starts <= t) & (t <= ends) & ~room_to_room
        active_edges = list(zip(id1s[mask].tolist(), id2s[mask].tolist()))
        active_nodes = {node for edge in active_edges for node in edge}

        # Advance the simulation a little: warm-start from where nodes already
        # are and only let this frame's active edges pull on them, so contact
        # pairs drift together while they're linked and drift apart again
        # (via the graph's baseline repulsion) once the edge disappears.
        frame_graph = nx.Graph()
        frame_graph.add_nodes_from(node_ids)
        frame_graph.add_edges_from(active_edges)
        new_pos = nx.spring_layout(
            frame_graph,
            pos=pos,
            k=k,
            fixed=list(fixed_ids) or None,
            iterations=args.layout_iterations,
            seed=args.seed,
        )
        if free_node_radius_limit is not None:
            # networkx's spring_layout only rescales/recenters its result when
            # nothing is fixed (see its docstring: "Fixing some nodes ... turns
            # off the rescaling feature"), since a uniform rescale would move
            # the fixed nodes too. With rooms pinned, that safety net is gone:
            # free nodes with no attracting edge only feel mutual repulsion,
            # which - compounded frame after frame with nothing pulling back -
            # pushes them outward without bound over a long video, straight
            # through the room circle. Clamping each free node's distance from
            # the circle's center to just inside its radius keeps them
            # contained without perturbing the fixed room positions, and
            # without erasing which direction (e.g. towards a particular room)
            # a node was pulled in - only distance is capped.
            for id_ in node_ids:
                if id_ in fixed_ids:
                    continue
                point = new_pos[id_]
                radius = np.linalg.norm(point)
                if radius > free_node_radius_limit:
                    new_pos[id_] = point * (free_node_radius_limit / radius)
        pos.update(new_pos)

        node_colors = [ACTIVE_NODE_COLOR if node in active_nodes else INACTIVE_NODE_COLOR for node in node_ids]
        nx.draw_networkx_nodes(
            node_graph, pos, nodelist=node_ids, node_color=node_colors, node_size=args.node_size, ax=ax
        )
        if active_edges:
            edge_graph = nx.Graph()
            edge_graph.add_edges_from(active_edges)
            nx.draw_networkx_edges(edge_graph, pos, ax=ax, edge_color=EDGE_COLOR, width=1.5)
        if labels:
            nx.draw_networkx_labels(node_graph, pos, labels=labels, ax=ax, font_size=7)

        ax.set_title(pd.Timestamp(t).strftime(TIMESTAMP_FORMAT))
        ax.axis("off")

        if frame_index % progress_every == 0:
            print(f"  frame {frame_index + 1}/{len(frame_times)}")
        return []

    anim = animation.FuncAnimation(fig, update, frames=len(frame_times), blit=False)
    writer, output_path = resolve_writer(args.output, args.fps)
    anim.save(str(output_path), writer=writer, dpi=args.dpi)
    plt.close(fig)

    print(f"Wrote {output_path} ({len(frame_times)} frames at {args.fps} fps)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
