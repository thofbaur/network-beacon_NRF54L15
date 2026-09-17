#!/usr/bin/env python3
"""Plot contact heatmaps from contacts_*.csv: person-person, person-room, and time-of-day.

A cell's color encodes the total time entities spent in a qualifying contact:
RSSI above --rssi-threshold, sustained for at least --min-duration-sec. The
loading/filtering/session logic lives in evaluation_preparation.py (reusable
by other scripts); this file is just the reordering-for-legibility and
plotting on top of it.

The person-person heatmap's rows/columns are reordered (hierarchical
clustering with optimal leaf ordering, see reorder_by_similarity) so that
people who share a lot of contact time end up near each other - this is what
makes the matrix read as close to diagonal/block-structured as the data
allows, rather than a scatter of cells in ID order. The person-room heatmap's
columns (rooms) follow a fixed priority order instead - Plenum, Mensa, every
"Kurs*" room, then the rest (see order_rooms_by_priority); its rows (persons)
are clustered specifically on that Kurs block (see
order_persons_by_kurs_usage), the same seriation idea applied to just the
persons x Kurs-rooms submatrix, so that block - the one with real
person-to-person structure (who's in which course) - reads as close to
diagonal as the data allows; Plenum/Mensa/the rest are just sorted by each
room's single highest person-contact value, not part of that clustering. The
time-of-day heatmap (day x time-of-day, on a variable raster - coarser 30-min
buckets across the quiet 02:00-07:30 window, finer 15-min buckets the rest of
the day, see variable_bucket_edges in evaluation_preparation.py) keeps its
natural chronological order on both axes instead - reordering a real time axis
would misrepresent it.

If list_persons.csv is available (see read_persons in
evaluation_preparation.py), one additional person-person sub-heatmap is
written per Kurs Grob value - the same contact data and clustering approach
as the main heatmap, but restricted to and independently reclustered on just
that course's persons (see plot_course_sub_heatmaps).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib
import numpy as np
import pandas as pd
from scipy.cluster.hierarchy import leaves_list, linkage, optimal_leaf_ordering
from scipy.spatial.distance import pdist, squareform

matplotlib.use("Agg")
from matplotlib.colors import LinearSegmentedColormap  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402

from evaluation_preparation import (  # noqa: E402
    DEFAULT_CONTACTS_DIR,
    DEFAULT_IGNORE_TIMES_CSV,
    DEFAULT_JOIN_ID_CSV,
    DEFAULT_MAX_GAP_SEC,
    DEFAULT_MIN_DURATION_SEC,
    DEFAULT_PERSONS_CSV,
    DEFAULT_RESULT_DIR,
    DEFAULT_ROOMS_CSV,
    DEFAULT_RSSI_THRESHOLD,
    DEFAULT_START_TIME,
    TIME_RASTER_COARSE_MINUTES,
    TIME_RASTER_COARSE_WINDOW_MIN,
    TIME_RASTER_FINE_MINUTES,
    build_bipartite_matrix,
    build_matrix,
    compute_person_person_sessions,
    compute_person_room_seconds,
    compute_time_of_day_matrix,
    variable_bucket_edges,
    load_contacts,
    load_id_joins,
    load_ignore_times,
    read_persons,
    read_rooms,
)

DEFAULT_OUTPUT_PNG = DEFAULT_RESULT_DIR / "Heatmap_Personen.png"
DEFAULT_ROOM_OUTPUT_PNG = DEFAULT_RESULT_DIR / "Heatmap_Raeume.png"
DEFAULT_TIME_OUTPUT_PNG = DEFAULT_RESULT_DIR / "Heatmap_Zeit.png"

# Sequential blue ramp, light -> dark (dataviz skill reference palette, palette.md).
SEQUENTIAL_BLUE = [
    "#cde2fb", "#b7d3f6", "#9ec5f4", "#86b6ef", "#6da7ec", "#5598e7",
    "#3987e5", "#2a78d6", "#256abf", "#1c5cab", "#184f95", "#104281", "#0d366b",
]
SURFACE = "#fcfcfb"
INK_PRIMARY = "#0b0b0b"
INK_SECONDARY = "#52514e"
GRIDLINE = "#e1e0d9"


def reorder_by_similarity(matrix: np.ndarray) -> np.ndarray:
    """Row/column order that groups people who share a lot of contact time.

    Hierarchical clustering (average linkage) on a contact-time-derived
    distance, with optimal leaf ordering to additionally minimize the
    distance between neighboring leaves - the standard "clustermap" seriation
    approach, which is what makes similar rows/columns land next to each
    other and the matrix read as close to diagonal as the data allows.
    """
    n = matrix.shape[0]
    if n < 3:
        return np.arange(n)

    max_seconds = matrix.max()
    if max_seconds <= 0:
        return np.arange(n)  # no contact at all recorded; ID order is as good as any

    distance = max_seconds - matrix
    np.fill_diagonal(distance, 0.0)
    condensed = squareform(distance, checks=False)

    z = linkage(condensed, method="average")
    z = optimal_leaf_ordering(z, condensed)
    return leaves_list(z)


def reorder_axis(vectors: np.ndarray) -> np.ndarray:
    """Order for one axis of a non-square (bipartite) matrix.

    Same seriation idea as reorder_by_similarity, but there's no single
    "shared contact time" between two rows (or two columns) the way there is
    between two people in the symmetric matrix - two persons are similar here
    if they visit the same rooms for similar durations. So this clusters on
    Euclidean distance between the rows' (or, for column order, the
    transposed matrix's rows') own value vectors instead - the standard
    approach for reordering a two-way value matrix (e.g. seaborn's
    clustermap).
    """
    n = vectors.shape[0]
    if n < 3 or not np.any(vectors):
        return np.arange(n)

    condensed = pdist(vectors, metric="euclidean")
    if not np.any(condensed):
        return np.arange(n)

    z = linkage(condensed, method="average")
    z = optimal_leaf_ordering(z, condensed)
    return leaves_list(z)


# Fixed lead-in for the person-room heatmap's room (column) order: these two
# always come first, in this order, if present at all.
ROOM_ORDER_FIXED_PREFIX = ["Plenum", "Mensa"]
ROOM_ORDER_GROUPED_PREFIX = "Kurs"


def kurs_column_indices(rooms: list[str]) -> list[int]:
    return [j for j, room in enumerate(rooms) if room.startswith(ROOM_ORDER_GROUPED_PREFIX)]


def order_rooms_by_priority(rooms: list[str], room_matrix: np.ndarray) -> np.ndarray:
    """Person-room heatmap column order: Plenum, Mensa, every room whose label starts
    with "Kurs", then everything else.

    The Kurs block is ordered by clustering it against the matching person
    row order (see order_persons_by_kurs_usage - both come from the same
    persons x Kurs-rooms submatrix), so that block reads as close to
    diagonal as the data allows, the same seriation idea as
    reorder_by_similarity/reorder_axis elsewhere in this file. The remaining
    rooms aren't part of that shared clustering, so they're just sorted by
    their single highest person-contact value (column max), descending.
    """
    room_index = {room: j for j, room in enumerate(rooms)}

    order: list[int] = []
    used: set[int] = set()
    for name in ROOM_ORDER_FIXED_PREFIX:
        if name in room_index:
            j = room_index[name]
            order.append(j)
            used.add(j)

    kurs_idx = kurs_column_indices(rooms)
    if kurs_idx:
        kurs_local_order = reorder_axis(room_matrix[:, kurs_idx].T)
        kurs_order = [kurs_idx[i] for i in kurs_local_order]
        order.extend(kurs_order)
        used.update(kurs_order)

    max_per_room = room_matrix.max(axis=0)
    rest = sorted((j for j in range(len(rooms)) if j not in used), key=lambda j: max_per_room[j], reverse=True)
    order.extend(rest)

    return np.array(order)


def order_persons_by_kurs_usage(rooms: list[str], room_matrix: np.ndarray) -> np.ndarray:
    """Person-room heatmap row order: persons clustered by their Kurs-room usage alone.

    Plenum, Mensa, and the "rest" rooms don't carry the kind of
    person-to-person distinction Kurs rooms do (which course someone is
    actually in), so clustering the full matrix (every room) dilutes exactly
    the pattern that's worth surfacing - restricting to the Kurs columns (see
    kurs_column_indices) is what makes that block, specifically, read as
    diagonal. Falls back to clustering on every room if there are no Kurs
    rooms at all.
    """
    kurs_idx = kurs_column_indices(rooms)
    if not kurs_idx:
        return reorder_axis(room_matrix)
    return reorder_axis(room_matrix[:, kurs_idx])


def _style_axes(ax: plt.Axes) -> None:
    ax.tick_params(length=0)
    for spine in ax.spines.values():
        spine.set_color(GRIDLINE)


def _colorbar(fig: plt.Figure, im, ax: plt.Axes) -> None:
    cbar = fig.colorbar(im, ax=ax, fraction=0.035, pad=0.02)
    cbar.set_label("Gesamtdauer der Kontakte (Minuten)", color=INK_SECONDARY, fontsize=9)
    cbar.ax.tick_params(labelsize=7, color=GRIDLINE, labelcolor=INK_SECONDARY)
    cbar.outline.set_edgecolor(GRIDLINE)


def plot_heatmap(
    matrix: np.ndarray,
    persons: list[int],
    order: np.ndarray,
    rssi_threshold: int,
    min_duration_sec: float,
    start_time: pd.Timestamp,
    output_path: Path,
    title_suffix: str = "",
) -> None:
    ordered = matrix[np.ix_(order, order)]
    ordered_ids = [persons[i] for i in order]
    minutes = ordered / 60.0

    n = len(persons)
    # A course sub-heatmap's title is longer (adds " - Kurs X"), so it needs a wider
    # floor than the main heatmap's to avoid the title clipping against the figure edge.
    width_floor = 11.5 if title_suffix else 8.0
    fig_size = max(width_floor, n * 0.14)
    fig, ax = plt.subplots(figsize=(fig_size, fig_size), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    cmap = LinearSegmentedColormap.from_list("contact_blue", SEQUENTIAL_BLUE)
    vmax = minutes.max() if minutes.max() > 0 else 1.0
    im = ax.imshow(minutes, cmap=cmap, vmin=0, vmax=vmax, aspect="equal")

    tick_step = max(1, n // 60)
    ticks = np.arange(0, n, tick_step)
    labels = [str(ordered_ids[i]) for i in ticks]
    ax.set_xticks(ticks)
    ax.set_xticklabels(labels, rotation=90, fontsize=5.5, color=INK_SECONDARY)
    ax.set_yticks(ticks)
    ax.set_yticklabels(labels, fontsize=5.5, color=INK_SECONDARY)
    _style_axes(ax)

    ax.set_xlabel("Beacon-ID", color=INK_SECONDARY, fontsize=9)
    ax.set_ylabel("Beacon-ID", color=INK_SECONDARY, fontsize=9)
    suffix = f" — {title_suffix}" if title_suffix else ""
    ax.set_title(
        f"Personenkontakte über die Gesamtzeit ab {start_time:%d.%m.%Y %H:%M}{suffix} — "
        f"RSSI > {rssi_threshold} dBm, Kontakt ≥ {min_duration_sec:g}s am Stück\n"
        "Sortierung: ähnlich vernetzte Personen gruppiert (hierarchisches Clustering)",
        color=INK_PRIMARY,
        fontsize=10.5,
        pad=12,
    )

    _colorbar(fig, im, ax)
    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def plot_course_sub_heatmaps(
    matrix: np.ndarray,
    persons: list[int],
    id_to_kurs: dict[int, str],
    rssi_threshold: int,
    min_duration_sec: float,
    start_time: pd.Timestamp,
    output_dir: Path,
) -> None:
    """One person-person sub-heatmap per Kurs Grob value, next to the main heatmap.

    Each sub-heatmap is its own independently reclustered (reorder_by_similarity)
    view of just that course's persons - clustering the full matrix and then
    slicing out a course's rows/columns would keep the full-matrix order,
    which needn't be the order that makes that specific submatrix read as
    diagonal. Courses with fewer than two persons with qualifying contact
    can't show a meaningful pairwise heatmap and are skipped.
    """
    kurs_to_indices: dict[str, list[int]] = {}
    for i, person_id in enumerate(persons):
        kurs = id_to_kurs.get(person_id)
        if kurs is not None:
            kurs_to_indices.setdefault(kurs, []).append(i)

    unassigned = len(persons) - sum(len(indices) for indices in kurs_to_indices.values())
    if unassigned:
        print(f"{unassigned} person(s) with qualifying contact have no Kurs Grob assignment - excluded from course sub-heatmaps.")

    for kurs in sorted(kurs_to_indices):
        indices = kurs_to_indices[kurs]
        if len(indices) < 2:
            print(f"Skipping Kurs {kurs} sub-heatmap - only {len(indices)} person(s) with qualifying contact.")
            continue
        sub_matrix = matrix[np.ix_(indices, indices)]
        sub_persons = [persons[i] for i in indices]
        sub_order = reorder_by_similarity(sub_matrix)
        safe_kurs = kurs.replace(" ", "_")
        course_output = output_dir / f"Heatmap_Personen_Kurs_{safe_kurs}.png"
        plot_heatmap(
            sub_matrix, sub_persons, sub_order, rssi_threshold, min_duration_sec, start_time, course_output,
            title_suffix=f"Kurs {kurs}",
        )
        print(f"Wrote person-person sub-heatmap for Kurs {kurs} ({len(sub_persons)} people) to {course_output}")


def plot_person_room_heatmap(
    matrix: np.ndarray,
    persons: list[int],
    rooms: list[str],
    row_order: np.ndarray,
    col_order: np.ndarray,
    rssi_threshold: int,
    min_duration_sec: float,
    start_time: pd.Timestamp,
    output_path: Path,
) -> None:
    ordered = matrix[np.ix_(row_order, col_order)]
    ordered_persons = [persons[i] for i in row_order]
    ordered_rooms = [rooms[j] for j in col_order]
    minutes = ordered / 60.0

    n_rows, n_cols = len(persons), len(rooms)
    fig_w = max(9.0, n_cols * 0.42)
    fig_h = max(10.0, n_rows * 0.13)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    cmap = LinearSegmentedColormap.from_list("contact_blue", SEQUENTIAL_BLUE)
    vmax = minutes.max() if minutes.max() > 0 else 1.0
    im = ax.imshow(minutes, cmap=cmap, vmin=0, vmax=vmax, aspect="auto")

    ax.set_xticks(np.arange(n_cols))
    ax.set_xticklabels(ordered_rooms, rotation=60, ha="right", fontsize=7, color=INK_SECONDARY)
    row_tick_step = max(1, n_rows // 70)
    row_ticks = np.arange(0, n_rows, row_tick_step)
    ax.set_yticks(row_ticks)
    ax.set_yticklabels([str(ordered_persons[i]) for i in row_ticks], fontsize=5.5, color=INK_SECONDARY)
    _style_axes(ax)

    ax.set_xlabel("Raum", color=INK_SECONDARY, fontsize=9)
    ax.set_ylabel("Beacon-ID (Person)", color=INK_SECONDARY, fontsize=9)
    ax.set_title(
        f"Personen-Raum-Kontakte über die Gesamtzeit ab {start_time:%d.%m.%Y %H:%M} — "
        f"RSSI > {rssi_threshold} dBm, Kontakt ≥ {min_duration_sec:g}s am Stück\n"
        "Räume: Plenum, Mensa, Kurs*, Rest (nach stärkstem Einzelkontakt); "
        "Personen nach Kurs-Nutzung geclustert, damit der Kurs-Block diagonal liest",
        color=INK_PRIMARY,
        fontsize=10.5,
        pad=12,
    )

    _colorbar(fig, im, ax)
    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def _raster_description() -> str:
    lo, hi = TIME_RASTER_COARSE_WINDOW_MIN
    return (
        f"{TIME_RASTER_COARSE_MINUTES}-Minuten-Fenster {lo // 60:02d}:{lo % 60:02d}-{hi // 60:02d}:{hi % 60:02d}, "
        f"sonst {TIME_RASTER_FINE_MINUTES}-Minuten-Fenster"
    )


def plot_time_of_day_heatmap(
    matrix: np.ndarray,
    days: list[pd.Timestamp],
    bucket_edges: list[int],
    rssi_threshold: int,
    min_duration_sec: float,
    output_path: Path,
) -> None:
    minutes = matrix / 60.0
    slots_per_day, n_days = matrix.shape

    fig_w = max(11.0, n_days * 0.8)
    fig_h = max(9.0, slots_per_day * 0.16)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h), dpi=150, facecolor=SURFACE)
    ax.set_facecolor(SURFACE)

    cmap = LinearSegmentedColormap.from_list("contact_blue", SEQUENTIAL_BLUE)
    vmax = minutes.max() if minutes.max() > 0 else 1.0
    im = ax.imshow(minutes, cmap=cmap, vmin=0, vmax=vmax, aspect="auto")

    weekdays = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]
    ax.set_xticks(np.arange(n_days))
    ax.set_xticklabels([f"{d:%d.%m.} ({weekdays[d.weekday()]})" for d in days], rotation=60, ha="right", fontsize=8)

    hour_ticks = [i for i in range(slots_per_day) if bucket_edges[i] % 60 == 0]
    ax.set_yticks(hour_ticks)
    ax.set_yticklabels([f"{bucket_edges[i] // 60:02d}:00" for i in hour_ticks], fontsize=7)
    ax.tick_params(colors=INK_SECONDARY)
    _style_axes(ax)

    ax.set_xlabel("Tag", color=INK_SECONDARY, fontsize=9)
    ax.set_ylabel("Uhrzeit", color=INK_SECONDARY, fontsize=9)
    ax.set_title(
        f"Personen-Personen-Kontakte je Zeitfenster ({_raster_description()}) — "
        f"RSSI > {rssi_threshold} dBm, Kontakt ≥ {min_duration_sec:g}s am Stück\n"
        "Gewichtung: Gesamtdauer aller Personen-Personen-Kontakte im jeweiligen Zeitfenster",
        color=INK_PRIMARY,
        fontsize=10.5,
        pad=12,
    )

    _colorbar(fig, im, ax)
    fig.tight_layout()
    fig.savefig(output_path, facecolor=SURFACE)
    plt.close(fig)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Plot person-person, person-room, and day x half-hour-of-day contact heatmaps "
            "(total qualifying contact time) from contacts_*.csv."
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
        "--persons-csv",
        type=Path,
        default=DEFAULT_PERSONS_CSV,
        help=(
            "Path to list_persons.csv (semicolon-separated; columns ID, Kurs Detail, Kurs "
            "Grob) - for each Kurs Grob value, an additional person-person sub-heatmap is "
            f"written next to the main one. Missing/empty file means no sub-heatmaps. Default: {DEFAULT_PERSONS_CSV}"
        ),
    )
    parser.add_argument(
        "--no-course-heatmaps",
        action="store_true",
        help="Skip the per-Kurs-Grob person-person sub-heatmaps even if --persons-csv is available.",
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
        help=f"Person-person heatmap output PNG path. Default: {DEFAULT_OUTPUT_PNG}",
    )
    parser.add_argument(
        "--room-output",
        type=Path,
        default=DEFAULT_ROOM_OUTPUT_PNG,
        help=f"Person-room heatmap output PNG path. Default: {DEFAULT_ROOM_OUTPUT_PNG}",
    )
    parser.add_argument(
        "--matrix-csv",
        type=Path,
        default=None,
        help="Optional: also write the ordered person x person matrix (minutes) to this CSV path.",
    )
    parser.add_argument(
        "--room-matrix-csv",
        type=Path,
        default=None,
        help="Optional: also write the ordered person x room matrix (minutes) to this CSV path.",
    )
    parser.add_argument(
        "--time-output",
        type=Path,
        default=DEFAULT_TIME_OUTPUT_PNG,
        help=f"Day x half-hour-of-day heatmap output PNG path. Default: {DEFAULT_TIME_OUTPUT_PNG}",
    )
    parser.add_argument(
        "--time-matrix-csv",
        type=Path,
        default=None,
        help="Optional: also write the day x half-hour-of-day matrix (minutes) to this CSV path.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    for output_path in (args.output, args.room_output, args.time_output):
        output_path.parent.mkdir(parents=True, exist_ok=True)

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

    # --- person-person heatmap + time-of-day heatmap (share the same sessions) ---
    pp_sessions = compute_person_person_sessions(
        contacts, room_ids, args.rssi_threshold, args.min_duration_sec, args.max_gap_sec
    )
    persons: list[int] = []
    if pp_sessions.empty:
        print("No qualifying person-person contacts found for the given thresholds.", file=sys.stderr)
    else:
        pp_totals = pp_sessions.groupby(["pmin", "pmax"])["duration"].sum().reset_index()
        pp_totals = pp_totals.rename(columns={"duration": "seconds"})
        persons = sorted(set(pp_totals["pmin"]).union(pp_totals["pmax"]))

        matrix = build_matrix(pp_totals, persons)
        order = reorder_by_similarity(matrix)
        plot_heatmap(matrix, persons, order, args.rssi_threshold, args.min_duration_sec, args.start_time, args.output)
        print(
            f"Wrote person-person heatmap for {len(persons)} people, {len(pp_totals)} pair(s) "
            f"with qualifying contact, to {args.output}"
        )
        if args.matrix_csv is not None:
            ordered_ids = [persons[i] for i in order]
            ordered_matrix = matrix[np.ix_(order, order)] / 60.0
            pd.DataFrame(ordered_matrix, index=ordered_ids, columns=ordered_ids).to_csv(args.matrix_csv)
            print(f"Wrote ordered person-person contact-minutes matrix to {args.matrix_csv}")

        if not args.no_course_heatmaps:
            if args.persons_csv.exists():
                id_to_kurs = read_persons(args.persons_csv)
                plot_course_sub_heatmaps(
                    matrix, persons, id_to_kurs, args.rssi_threshold, args.min_duration_sec, args.start_time,
                    args.output.parent,
                )
            else:
                print(f"Persons CSV not found at {args.persons_csv} - skipping course sub-heatmaps.")

        bucket_edges = variable_bucket_edges()
        time_matrix, days = compute_time_of_day_matrix(pp_sessions, bucket_edges=bucket_edges)
        plot_time_of_day_heatmap(
            time_matrix, days, bucket_edges, args.rssi_threshold, args.min_duration_sec, args.time_output
        )
        print(f"Wrote time-of-day heatmap for {len(days)} day(s) to {args.time_output}")
        if args.time_matrix_csv is not None:
            row_labels = [
                f"{bucket_edges[i] // 60:02d}:{bucket_edges[i] % 60:02d}-"
                f"{bucket_edges[i + 1] // 60:02d}:{bucket_edges[i + 1] % 60:02d}"
                for i in range(time_matrix.shape[0])
            ]
            col_labels = [f"{d:%Y-%m-%d}" for d in days]
            pd.DataFrame(time_matrix / 60.0, index=row_labels, columns=col_labels).to_csv(args.time_matrix_csv)
            print(f"Wrote day x time-of-day contact-minutes matrix to {args.time_matrix_csv}")

    # --- person-room heatmap ---
    pr_totals = compute_person_room_seconds(
        contacts, id_to_room_name, args.rssi_threshold, args.min_duration_sec, args.max_gap_sec
    )
    room_persons = sorted(pr_totals["person"].unique())
    rooms = sorted(pr_totals["room"].unique())
    if not room_persons or not rooms:
        print("No qualifying person-room contacts found for the given thresholds.", file=sys.stderr)
        return 0 if persons else 1

    room_matrix = build_bipartite_matrix(pr_totals, room_persons, rooms)
    row_order = order_persons_by_kurs_usage(rooms, room_matrix)
    col_order = order_rooms_by_priority(rooms, room_matrix)
    plot_person_room_heatmap(
        room_matrix,
        room_persons,
        rooms,
        row_order,
        col_order,
        args.rssi_threshold,
        args.min_duration_sec,
        args.start_time,
        args.room_output,
    )
    print(
        f"Wrote person-room heatmap for {len(room_persons)} people x {len(rooms)} rooms, "
        f"{len(pr_totals)} pair(s) with qualifying contact, to {args.room_output}"
    )
    if args.room_matrix_csv is not None:
        ordered_persons = [room_persons[i] for i in row_order]
        ordered_rooms = [rooms[j] for j in col_order]
        ordered_room_matrix = room_matrix[np.ix_(row_order, col_order)] / 60.0
        pd.DataFrame(ordered_room_matrix, index=ordered_persons, columns=ordered_rooms).to_csv(args.room_matrix_csv)
        print(f"Wrote ordered person-room contact-minutes matrix to {args.room_matrix_csv}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
