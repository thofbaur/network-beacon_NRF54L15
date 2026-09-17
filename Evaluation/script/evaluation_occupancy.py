#!/usr/bin/env python3
"""Animate person-room occupancy: rooms fixed on an outer circle, IDs drifting between them.

Every room (list_rooms.csv) gets one fixed position, evenly spaced around a
circle - see visualize_contact_graph.compute_room_positions, reused as-is so
both animations lay rooms out the same way. At each frame time t, a person
with a qualifying room session (RSSI/min-duration/gap rules from
evaluation_preparation.py, same as the other evaluation_*.py scripts)
covering t moves towards that room's position; a person with no such session
drifts towards the circle's center instead, joining the others with nothing
going on into a loose pool rather than a single overlapping dot (see
--pool-radius). Movement is a simple per-frame lerp towards the current
target (--move-speed), not a physics simulation - unlike
visualize_contact_graph.py's force-directed layout, positions here are fully
determined by "which room, if any" and don't need one.

IDs are colored by their current room (same room -> same color everywhere in
the frame); pooled IDs are a fixed neutral color. With dozens of rooms this
can't be a CVD-safe categorical palette (that caps out around 8 hues) - color
here is a secondary read alongside position and the room labels, not the only
way to tell rooms apart. Each room's own circle marker grows with how many
IDs currently have a qualifying session there (--room-marker-base-size /
--room-marker-size-per-id) and is filled in that room's color, so a busy room
reads as a visibly bigger, same-colored halo behind its occupants' dots.

Self-reports (self_reports.csv, deduplicated the same way as
evaluation_selfreport.py - see evaluation_preparation.dedupe_self_reports)
are matched to a room the same way too (a qualifying room session overlapping
+/- --report-window-min around the report - see
evaluation_preparation.match_self_reports_to_rooms) and drawn as a lightning
bolt in that room's color on whichever frame the report falls into, with a
one-frame fading afterglow so a report isn't just a single-frame blink.

A second ring further out shows the same matched self-reports as a per-room
daily histogram, built up live: each room gets one small radial bar per day
(days running --day-start-hour to --day-start-hour, default 04:00-04:00, so a
late night doesn't get cut in half), bars for the same room laid out side by
side in chronological order. A bar doesn't appear until that room's first
report of the day, then grows by one step on the frame each further report
lands in; once --day-start-hour is crossed the bar stops growing (it stays at
whatever it reached) and the next day gets a fresh bar starting at zero. Bar
length is relative to the busiest room-day of the whole run (computed once up
front), so the scale doesn't shift under a bar while it's still growing.
Filled in the room's color, same as the flashes.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.collections import LineCollection

from evaluation_preparation import (
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
    bucket_into_days,
    compute_person_room_sessions,
    dedupe_self_reports,
    load_contacts,
    load_id_joins,
    load_ignore_times,
    load_self_reports,
    match_self_reports_to_rooms,
    read_rooms,
)
from visualize_contact_graph import compute_room_positions, resolve_writer

DEFAULT_OUTPUT = DEFAULT_RESULT_DIR / "video_room_occupancy.mp4"
DEFAULT_STEP_SECONDS = 120
DEFAULT_MIN_DISPLAY_SECONDS = 60
DEFAULT_MOVE_SPEED = 0.25  # fraction of the remaining distance to the target closed per frame
DEFAULT_POOL_RADIUS_FRACTION = 0.18  # fraction of the room-circle radius the center pool spreads over
DEFAULT_FPS = 10
DEFAULT_NODE_SIZE = 60
DEFAULT_ROOM_MARKER_BASE_SIZE = 20  # empty-room marker area (points^2), so empty rooms still show up
DEFAULT_ROOM_MARKER_SIZE_PER_ID = 12  # extra marker area (points^2) added per current occupant
DEFAULT_FLASH_SIZE = 260  # self-report lightning-bolt marker area (points^2)
DEFAULT_DAY_START_HOUR = 4  # a "day", for the outer daily-self-report ring, runs this hour to the same hour next day
DEFAULT_RING_GAP_FRACTION = 0.1  # gap between the room-label ring and the daily-histogram ring, as a fraction of room_radius
DEFAULT_RING_LENGTH_FRACTION = 0.65  # max daily-histogram bar length, as a fraction of room_radius
DEFAULT_RING_SLICE_FRACTION = 0.7  # fraction of each room's angular slot the day-bars are allowed to fill
DEFAULT_RING_LINEWIDTH = 3.0  # daily-histogram bar thickness (points)
DEFAULT_DPI = 100
DEFAULT_SEED = 42

POOL_COLOR = "#B0B0B0"
ROOM_MARKER_COLOR = "#52514E"
FLASH_EDGE_COLOR = "#0B0B0B"
RING_TRACK_COLOR = "#E1E0D9"

# A simple lightning-bolt outline, in marker-unit coordinates (roughly -1..1
# on each axis, as matplotlib's custom-marker vertices expect).
BOLT_VERTICES = np.array(
    [
        [0.15, 1.0],
        [-0.55, 0.05],
        [-0.05, 0.05],
        [-0.35, -1.0],
        [0.55, -0.05],
        [0.05, -0.05],
        [0.15, 1.0],
    ]
)


def compute_pool_offsets(person_ids: list[int], radius: float, seed: int) -> dict[int, np.ndarray]:
    """A small, fixed, deterministic offset per person within the center pool.

    Without this every unassigned person would sit exactly on (0, 0) and
    render as a single dot; a stable per-ID jitter spreads them into a
    visible cluster instead, and staying fixed (not re-rolled per frame)
    means a pooled person doesn't jitter in place across frames.
    """
    rng = np.random.default_rng(seed)
    offsets = {}
    for person_id in person_ids:
        angle = rng.uniform(0, 2 * np.pi)
        r = radius * np.sqrt(rng.uniform(0, 1))  # sqrt so points fill the disc evenly, not bunch at the center
        offsets[person_id] = r * np.array([np.cos(angle), np.sin(angle)])
    return offsets


def room_colors(room_order: list[str]) -> dict[str, tuple]:
    """One color per room, cycled from a large qualitative colormap.

    Not validated for colorblind-safe adjacency (see module docstring) -
    with dozens of rooms that's not achievable by hue alone, and position on
    the circle plus the room label are the primary way to tell rooms apart.
    """
    cmap = plt.get_cmap("gist_rainbow", max(len(room_order), 1))
    return {room: cmap(i) for i, room in enumerate(room_order)}


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Animate person IDs drifting between fixed room positions on a circle, based on person-room sessions."
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
        "--self-reports-csv",
        type=Path,
        default=DEFAULT_SELF_REPORTS_CSV,
        help=f"Path to self_reports.csv. Default: {DEFAULT_SELF_REPORTS_CSV}",
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
        "--start-time",
        type=pd.Timestamp,
        default=DEFAULT_START_TIME,
        help=(
            "Ignore contact events before this local timestamp (deployment go-live; earlier "
            f"rows are setup/test noise), and default animation start. Default: {DEFAULT_START_TIME}"
        ),
    )
    parser.add_argument(
        "--end-time",
        type=pd.Timestamp,
        default=None,
        help="Animation end timestamp. Default: the latest contact timestamp in the data.",
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
            "Minimum time a session stays visible, padded on from its start if it's shorter - "
            "without this, a session much shorter than --step-seconds could fall between two "
            f"sampled frames and never appear at all. Default: {DEFAULT_MIN_DISPLAY_SECONDS}"
        ),
    )
    parser.add_argument(
        "--move-speed",
        type=float,
        default=DEFAULT_MOVE_SPEED,
        help=(
            "Fraction of the remaining distance to an ID's current target (room or pool) "
            f"closed per frame. Lower = slower/smoother drift, 1.0 = snap instantly. Default: {DEFAULT_MOVE_SPEED}"
        ),
    )
    parser.add_argument(
        "--pool-radius-fraction",
        type=float,
        default=DEFAULT_POOL_RADIUS_FRACTION,
        help=(
            "How far IDs with no current room session spread out around the center, as a "
            f"fraction of the room circle's radius. Default: {DEFAULT_POOL_RADIUS_FRACTION}"
        ),
    )
    parser.add_argument(
        "--report-window-min",
        type=float,
        default=DEFAULT_SELFREPORT_WINDOW_MIN,
        help=(
            "A self-report is attributed to a room if that person had a qualifying room "
            "session overlapping this many minutes before or after the report. "
            f"Default: {DEFAULT_SELFREPORT_WINDOW_MIN}"
        ),
    )
    parser.add_argument(
        "--report-dedupe-window-min",
        type=float,
        default=DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN,
        help=(
            "Ignore a self-report if it falls within this many minutes of an earlier report "
            f"from the same ID (a burst of repeat presses counts once). Default: {DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN}"
        ),
    )
    parser.add_argument("--no-flashes", action="store_true", help="Don't draw self-report lightning-bolt flashes.")
    parser.add_argument(
        "--flash-size",
        type=float,
        default=DEFAULT_FLASH_SIZE,
        help=f"Self-report lightning-bolt marker area (points^2). Default: {DEFAULT_FLASH_SIZE}",
    )
    parser.add_argument(
        "--no-day-ring", action="store_true", help="Don't draw the outer per-room daily self-report histogram ring."
    )
    parser.add_argument(
        "--day-start-hour",
        type=float,
        default=DEFAULT_DAY_START_HOUR,
        help=(
            "A 'day' in the outer histogram ring runs from this hour to the same hour the next "
            f"day (e.g. 4 -> 04:00-04:00), so late-night activity isn't split across two bars. Default: {DEFAULT_DAY_START_HOUR}"
        ),
    )
    parser.add_argument(
        "--ring-gap-fraction",
        type=float,
        default=DEFAULT_RING_GAP_FRACTION,
        help=(
            "Gap between the room circle and the daily histogram ring, as a fraction of the room "
            f"circle's radius. Default: {DEFAULT_RING_GAP_FRACTION}"
        ),
    )
    parser.add_argument(
        "--ring-length-fraction",
        type=float,
        default=DEFAULT_RING_LENGTH_FRACTION,
        help=(
            "Length of the tallest daily-histogram bar, as a fraction of the room circle's "
            f"radius. Default: {DEFAULT_RING_LENGTH_FRACTION}"
        ),
    )
    parser.add_argument(
        "--ring-slice-fraction",
        type=float,
        default=DEFAULT_RING_SLICE_FRACTION,
        help=(
            "Fraction of each room's angular slot the day-bars are allowed to spread across "
            f"(the rest is empty gap to the next room's bars). Default: {DEFAULT_RING_SLICE_FRACTION}"
        ),
    )
    parser.add_argument(
        "--ring-linewidth",
        type=float,
        default=DEFAULT_RING_LINEWIDTH,
        help=f"Daily-histogram bar thickness (points). Default: {DEFAULT_RING_LINEWIDTH}",
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Output video path (.mp4 or .gif).")
    parser.add_argument("--fps", type=int, default=DEFAULT_FPS, help=f"Video frame rate. Default: {DEFAULT_FPS}")
    parser.add_argument(
        "--node-size", type=int, default=DEFAULT_NODE_SIZE, help=f"Person marker size. Default: {DEFAULT_NODE_SIZE}"
    )
    parser.add_argument(
        "--room-marker-base-size",
        type=float,
        default=DEFAULT_ROOM_MARKER_BASE_SIZE,
        help=f"Room marker area (points^2) with nobody in it. Default: {DEFAULT_ROOM_MARKER_BASE_SIZE}",
    )
    parser.add_argument(
        "--room-marker-size-per-id",
        type=float,
        default=DEFAULT_ROOM_MARKER_SIZE_PER_ID,
        help=(
            "Extra room marker area (points^2) added per person currently in that room, so "
            f"fuller rooms draw a visibly bigger circle. Default: {DEFAULT_ROOM_MARKER_SIZE_PER_ID}"
        ),
    )
    parser.add_argument("--dpi", type=int, default=DEFAULT_DPI, help=f"Output resolution (DPI). Default: {DEFAULT_DPI}")
    parser.add_argument(
        "--figsize", type=float, nargs=2, default=(11.0, 11.0), metavar=("WIDTH", "HEIGHT"), help="Figure size in inches."
    )
    parser.add_argument("--no-labels", action="store_true", help="Don't draw room name labels.")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="Pool-jitter random seed, for reproducibility.")
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
    contacts = contacts[contacts["Contact Local Time"] >= args.start_time]
    print(f"Loaded {len(contacts)} contact rows (from {args.start_time} on).")

    person_ids = sorted((set(contacts["ID1"]) | set(contacts["ID2"])) - room_ids)
    if not person_ids:
        print("No person IDs found in the data.", file=sys.stderr)
        return 1

    sessions = compute_person_room_sessions(
        contacts, id_to_room_name, args.rssi_threshold, args.min_duration_sec, args.max_gap_sec
    )
    print(f"Found {len(sessions)} qualifying person-room session(s) across {len(person_ids)} person(s).")
    if sessions.empty:
        print("No qualifying person-room sessions - every ID would just sit pooled the whole video.", file=sys.stderr)

    min_display = pd.Timedelta(seconds=args.min_display_seconds)
    effective_end = sessions["end"].where(
        sessions["end"] >= sessions["start"] + min_display, sessions["start"] + min_display
    )
    session_persons = sessions["person"].to_numpy()
    session_rooms = sessions["room"].to_numpy()
    session_starts = sessions["start"].to_numpy()
    session_ends = effective_end.to_numpy()

    start = args.start_time
    end = args.end_time if args.end_time is not None else contacts["Contact Local Time"].max()
    if end <= start:
        print(f"--end-time ({end}) must be after --start-time ({start})", file=sys.stderr)
        return 1
    frame_times = pd.date_range(start, end, freq=f"{args.step_seconds}s")
    print(f"Animating {start} to {end} in {len(frame_times)} frame(s) of {args.step_seconds}s each.")

    want_reports = not args.no_flashes or not args.no_day_ring
    report_matches = pd.DataFrame(columns=["room", "report_time"])
    if want_reports:
        if not args.self_reports_csv.exists():
            print(f"Self-reports CSV not found at {args.self_reports_csv}; no flashes/day-ring will be drawn.")
        else:
            self_reports = load_self_reports(args.self_reports_csv, id_joins, ignore_times)
            self_reports = self_reports[
                ~self_reports["ID"].isin(always_exclude)
                & (self_reports["Local Time"] >= start)
                & (self_reports["Local Time"] <= end)
            ]
            self_reports = dedupe_self_reports(self_reports, args.report_dedupe_window_min)
            report_matches, matched_report_count = match_self_reports_to_rooms(
                self_reports, sessions, args.report_window_min
            )
            print(
                f"Matched {matched_report_count} of {len(self_reports)} self-report(s) to a room "
                f"({len(report_matches)} (report, room) pair(s))."
            )

    flashes_by_frame: dict[int, set[str]] = {}
    if not args.no_flashes and not report_matches.empty:
        # side="right" - 1 puts a report at/after frame i's timestamp into
        # frame i (the frame whose sampled instant it's closest after),
        # so every kept report gets exactly one frame to flash on.
        frame_indices = np.searchsorted(frame_times.values, report_matches["report_time"].values, side="right") - 1
        for frame_index, room in zip(frame_indices, report_matches["room"]):
            if 0 <= frame_index < len(frame_times):
                flashes_by_frame.setdefault(int(frame_index), set()).add(room)

    # dict preserves insertion order, and id_to_room_name was built from the
    # rooms CSV's own row order, so this dedupes room names while keeping
    # that order - rooms listed near each other in the CSV (e.g. same floor)
    # land next to each other on the circle too.
    room_order = list(dict.fromkeys(id_to_room_name.values()))
    room_pos = compute_room_positions(room_order)
    room_angle = dict(zip(room_order, np.linspace(0.0, 2 * np.pi, len(room_order), endpoint=False)))
    room_radius = max((float(np.linalg.norm(p)) for p in room_pos.values()), default=1.0)
    pool_radius = room_radius * args.pool_radius_fraction
    pool_offsets = compute_pool_offsets(person_ids, pool_radius, args.seed)
    colors_by_room = room_colors(room_order)

    # Outer ring: a small per-day bar per room, built up live instead of shown
    # complete from frame 1 - each matched self-report grows its (room, day)
    # bar by one step the frame it happens in, and crossing --day-start-hour
    # starts a fresh bar rather than continuing the previous one. The bar's
    # eventual full length (once its day is over) is fixed by max_count (the
    # busiest room-day of the whole run), computed once up front so the scale
    # doesn't shift under a bar while it's still growing.
    ring_bar_geometry: dict[tuple, tuple[np.ndarray, np.ndarray, tuple]] = {}  # (room, day) -> (start_xy, direction, color)
    ring_increments_by_frame: dict[int, list[tuple]] = {}
    ring_counts: dict[tuple, int] = {}  # mutated frame by frame in update() - this frame's cumulative count per (room, day)
    ring_start_radius = room_radius * (1 + args.ring_gap_fraction)
    ring_length = room_radius * args.ring_length_fraction
    ring_outer_limit = room_radius
    max_count = 1
    day_labels: list[str] = []
    if not args.no_day_ring and not report_matches.empty:
        report_matches = report_matches.copy()
        report_matches["day"] = bucket_into_days(report_matches["report_time"], args.day_start_hour)
        day_order = sorted(report_matches["day"].unique())
        day_labels = [f"{d:%d.%m.}" for d in day_order]
        day_index = {d: i for i, d in enumerate(day_order)}
        counts = report_matches.groupby(["room", "day"]).size()
        max_count = int(counts.max())

        slice_width = (2 * np.pi / len(room_order)) * args.ring_slice_fraction
        n_days = len(day_order)
        for (room, day), _count in counts.items():
            slot = day_index[day]
            offset = -slice_width / 2 + (slot + 0.5) / n_days * slice_width
            bar_angle = room_angle[room] + offset
            direction = np.array([np.cos(bar_angle), np.sin(bar_angle)])
            ring_bar_geometry[(room, day)] = (ring_start_radius * direction, direction, colors_by_room[room])

        # Every matched report grows its (room, day) bar on the frame it
        # itself falls into - reusing the same "which frame does this
        # timestamp land in" logic as the flashes above.
        report_frame_indices = np.searchsorted(frame_times.values, report_matches["report_time"].values, side="right") - 1
        for frame_index, room, day in zip(report_frame_indices, report_matches["room"], report_matches["day"]):
            if 0 <= frame_index < len(frame_times):
                ring_increments_by_frame.setdefault(int(frame_index), []).append((room, day))

        ring_outer_limit = ring_start_radius + ring_length
        print(f"Day ring: {len(day_order)} day(s) ({', '.join(day_labels)}) x {len(room_order)} room(s), max {max_count}/day.")

    # Everyone starts in the pool; the first frame already carries whatever
    # session is active then, so this only matters as the pre-frame-1 state
    # the first lerp step moves away from.
    pos = {person_id: pool_offsets[person_id].copy() for person_id in person_ids}
    flash_trail: list[set[str]] = []  # this frame's flashes plus one frame of fading afterglow

    fig, ax = plt.subplots(figsize=args.figsize)
    # Tighter than matplotlib's default ~10-12%-per-side margin, so the
    # circle/ring get more of the frame; kept symmetric (same margin on
    # opposite sides) to leave the axes box itself square, matching the
    # square figsize.
    fig.subplots_adjust(left=0.05, right=0.95, top=0.94, bottom=0.04)
    if day_labels:
        fig.text(
            0.01,
            0.01,
            f"Äußerer Ring: Self-Reports/Tag ({args.day_start_hour:g}:00–{args.day_start_hour:g}:00), "
            f"je Raum links→rechts: {', '.join(day_labels)}",
            fontsize=7,
            color=ROOM_MARKER_COLOR,
        )
    progress_every = max(1, len(frame_times) // 20)

    def update(frame_index: int):
        ax.clear()
        t = np.datetime64(frame_times[frame_index])
        mask = (session_starts <= t) & (t <= session_ends)
        # A person with two simultaneous qualifying room sessions (ambiguous,
        # rare) just gets the first match - one dot can only be in one place.
        current_room: dict[int, str] = {}
        for person, room in zip(session_persons[mask], session_rooms[mask]):
            current_room.setdefault(person, room)

        for person_id in person_ids:
            target = room_pos[current_room[person_id]] if person_id in current_room else pool_offsets[person_id]
            pos[person_id] += (target - pos[person_id]) * args.move_speed

        room_counts: dict[str, int] = {}
        for room in current_room.values():
            room_counts[room] = room_counts.get(room, 0) + 1

        # Room markers/labels are drawn every frame regardless of occupancy,
        # so the circle stays legible even for empty rooms (--room-marker-base-size
        # keeps them visible at zero) - size grows with current occupant count,
        # filled in the room's own color so it reads as a "how full" halo
        # behind that room's person dots.
        room_xy = np.array([room_pos[room] for room in room_order])
        room_sizes = np.array(
            [args.room_marker_base_size + room_counts.get(room, 0) * args.room_marker_size_per_id for room in room_order]
        )
        room_fill_colors = [colors_by_room[room] for room in room_order]
        ax.scatter(
            room_xy[:, 0],
            room_xy[:, 1],
            s=room_sizes,
            c=room_fill_colors,
            marker="o",
            alpha=0.35,
            edgecolors=ROOM_MARKER_COLOR,
            linewidths=0.6,
            zorder=2,
        )
        if not args.no_labels:
            for room in room_order:
                x, y = room_pos[room]
                ax.annotate(
                    room, (x, y), xytext=(x * 1.12, y * 1.12), fontsize=6, color=ROOM_MARKER_COLOR, ha="center", va="center"
                )

        if not args.no_day_ring and ring_bar_geometry:
            for room, day in ring_increments_by_frame.get(frame_index, []):
                key = (room, day)
                ring_counts[key] = ring_counts.get(key, 0) + 1

            ax.add_patch(
                plt.Circle((0, 0), ring_start_radius, fill=False, edgecolor=RING_TRACK_COLOR, linewidth=0.8, zorder=1)
            )
            if ring_counts:
                segments = []
                colors = []
                for key, count in ring_counts.items():
                    start_xy, direction, color = ring_bar_geometry[key]
                    end_xy = start_xy + direction * (count / max_count) * ring_length
                    segments.append([tuple(start_xy), tuple(end_xy)])
                    colors.append(color)
                ax.add_collection(LineCollection(segments, colors=colors, linewidths=args.ring_linewidth, zorder=1))

        person_xy = np.array([pos[person_id] for person_id in person_ids])
        person_colors = [
            colors_by_room[current_room[person_id]] if person_id in current_room else POOL_COLOR
            for person_id in person_ids
        ]
        ax.scatter(person_xy[:, 0], person_xy[:, 1], s=args.node_size, c=person_colors, zorder=3, edgecolors="none")

        if not args.no_flashes:
            flash_trail.append(flashes_by_frame.get(frame_index, set()))
            del flash_trail[:-2]  # this frame plus one frame of afterglow
            for age, rooms in enumerate(flash_trail):
                if not rooms:
                    continue
                is_current = age == len(flash_trail) - 1
                xy = np.array([room_pos[room] for room in rooms])
                colors = [colors_by_room[room] for room in rooms]
                ax.scatter(
                    xy[:, 0],
                    xy[:, 1],
                    s=args.flash_size if is_current else args.flash_size * 0.6,
                    c=colors,
                    marker=BOLT_VERTICES,
                    alpha=1.0 if is_current else 0.35,
                    edgecolors=FLASH_EDGE_COLOR,
                    linewidths=0.7,
                    zorder=4,
                )

        limit = max(room_radius * 1.1, ring_outer_limit * 1.02)
        ax.set_xlim(-limit, limit)
        ax.set_ylim(-limit, limit)
        ax.set_aspect("equal")
        ax.set_title(f"{pd.Timestamp(t):%Y-%m-%d %H:%M} — {len(current_room)}/{len(person_ids)} in a room")
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
