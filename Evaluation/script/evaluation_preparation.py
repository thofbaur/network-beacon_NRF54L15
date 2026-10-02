#!/usr/bin/env python3
"""Load contacts_*.csv and derive qualifying contact sessions.

A "session" is a run of contact events for the same key (a person pair, or a
person and a room) with RSSI above a threshold and no gap larger than
max_gap_sec between consecutive events; it only counts if its span reaches
min_duration_sec (see build_sessions). This is the reusable data layer behind
evaluation_heatmaps.py's heatmaps - import from here rather than duplicating
the filtering/session logic in another script.

Room IDs are collapsed to their room name (list_rooms.csv) - several beacons
usually cover one physical room, and treating them as one column avoids
double-counting simultaneous detections from more than one of them. Every ID
listed in list_rooms.csv counts as a room this way, including the 14 tagged
DYNAMIC/FUN_sub; only the reserved special IDs are dropped entirely.

Two more input files, both optional (a missing or empty file just means
"nothing configured", not an error):
- join_ID.csv (columns ID1, ID2, date; date as D.M.YYYY H:MM): from date
  onward, ID2 is folded into ID1 - e.g. a person was issued a new beacon at
  that time, and this makes their two IDs count as one from then on. Before
  date, ID2 keeps its own identity (it may have belonged to someone else
  beforehand). Chains (id2->id1 effective one date, id1->id0 effective a
  later one) are resolved (see apply_id_joins), and the resulting, merged ID
  is always the row's ID1.
- ignore_times.csv (columns ID, start, end; dates as D.M.YYYY H:MM): every
  contact and every self-report involving that (raw, pre-join) ID during
  [start, end] is dropped entirely before any analysis - for a beacon known
  to be malfunctioning, mislaid, or otherwise producing noise during a
  specific window.

A third, also optional file - list_persons.csv (semicolon-separated; columns
ID, Kurs Detail, Kurs Grob) - assigns each person to a coarse course group
(Kurs Grob). It doesn't feed the session/filtering logic; it's read directly
by evaluation_heatmaps.py to additionally plot one person-person sub-heatmap
per course (see read_persons).
"""

from __future__ import annotations

import re
from pathlib import Path

import numpy as np
import pandas as pd

EVALUATION_DIR = Path(__file__).resolve().parent.parent  # this module lives in Evaluation/script/
DEFAULT_CONTACTS_DIR = EVALUATION_DIR.parent / "Network_Log+Postprocessing" / "Output"
DEFAULT_ROOMS_CSV = EVALUATION_DIR / "list_rooms.csv"
DEFAULT_PERSONS_CSV = EVALUATION_DIR / "list_persons.csv"
DEFAULT_SELF_REPORTS_CSV = DEFAULT_CONTACTS_DIR / "self_reports.csv"
DEFAULT_JOIN_ID_CSV = EVALUATION_DIR / "join_ID.csv"
DEFAULT_IGNORE_TIMES_CSV = EVALUATION_DIR / "ignore_times.csv"
DEFAULT_RESULT_DIR = EVALUATION_DIR / "result"

DEFAULT_RSSI_THRESHOLD = -70
DEFAULT_MIN_DURATION_SEC = 30
DEFAULT_MAX_GAP_SEC = 60  # gap between qualifying events before a contact session is considered ended
DEFAULT_START_TIME = pd.Timestamp("2026-08-13 18:00:00")  # deployment went live; earlier rows are setup/test noise
TIME_BUCKET_MINUTES = 30  # uniform fallback raster; the time-of-day heatmap uses the variable one below

# The day x time-of-day heatmap (Heatmap_Zeit.png) uses a variable raster: coarse
# 30-minute buckets across the quiet overnight window (02:00-07:30), finer
# 15-minute buckets across the rest of the day.
TIME_RASTER_COARSE_WINDOW_MIN = (2 * 60, 7 * 60 + 30)  # [start, end) minutes from midnight
TIME_RASTER_COARSE_MINUTES = 30
TIME_RASTER_FINE_MINUTES = 15
DEFAULT_SELFREPORT_WINDOW_MIN = 5  # match a self-report to a room via a session overlapping this many minutes either side
DEFAULT_SELFREPORT_DEDUPE_WINDOW_MIN = 2  # collapse same-ID repeat reports within this many minutes into one

# Reserved/non-person device IDs - kept in sync with postprocessing.py's
# DEFAULT_VALID_EXTRA_IDS in Network_Python_RRT+UART (not imported from there
# since this module lives in a different folder than postprocessing.py).
SPECIAL_IDS = frozenset({252, 253, 254})


def read_rooms(rooms_csv: Path) -> tuple[dict[int, str], set[int]]:
    """Parse list_rooms.csv (columns ID, Raumnummer, Label).

    Returns (id_to_room_name, always_exclude):
    - id_to_room_name: every listed beacon ID -> its Label (the display/room-
      grouping name - Raumnummer is not used: two rows can share a
      Raumnummer while being different physical rooms, e.g. "2.4" is used for
      both a "Kurs 1.4" and a "großer Kunstraum" row, and Label is what tells
      them apart). Some IDs are written with a non-numeric prefix (e.g.
      "BTN30", "XTN2" for the dynamic/mobile ones) - only the digits are
      kept, so "BTN30" reads as ID 30.
    - always_exclude: the reserved special IDs, dropped entirely.
    """
    rooms = pd.read_csv(rooms_csv, dtype=str)
    rooms.columns = [column.strip() for column in rooms.columns]

    id_to_room_name: dict[int, str] = {}
    for raw_id, label in zip(rooms["ID"], rooms["Label"]):
        raw_id = raw_id.strip()
        digits = re.sub(r"\D", "", raw_id)
        if not digits:
            continue
        id_to_room_name[int(digits)] = label.strip()

    return id_to_room_name, set(SPECIAL_IDS)


def read_persons(persons_csv: Path) -> dict[int, str]:
    """Parse list_persons.csv (semicolon-separated; columns ID, Kurs Detail, Kurs Grob).

    Returns id_to_kurs_grob: every listed person ID -> its "Kurs Grob" value
    (the coarse course grouping - "Kurs Detail" is more specific and not
    used here). A missing or empty file returns an empty mapping - not an
    error. An ID that appears more than once with different Kurs Grob values
    is a genuine data conflict, not a formatting quirk, so it's dropped
    entirely rather than guessed at.
    """
    if not persons_csv.exists() or persons_csv.stat().st_size == 0:
        return {}
    persons = pd.read_csv(persons_csv, sep=";", dtype=str)
    persons.columns = [column.strip() for column in persons.columns]
    persons = persons.dropna(subset=["ID", "Kurs Grob"])

    id_to_kurs: dict[int, str] = {}
    conflicting: set[int] = set()
    for raw_id, kurs in zip(persons["ID"], persons["Kurs Grob"]):
        digits = re.sub(r"\D", "", raw_id.strip())
        kurs = kurs.strip()
        if not digits or not kurs:
            continue
        person_id = int(digits)
        if person_id in id_to_kurs and id_to_kurs[person_id] != kurs:
            conflicting.add(person_id)
            continue
        id_to_kurs[person_id] = kurs

    for person_id in conflicting:
        id_to_kurs.pop(person_id, None)

    return id_to_kurs


def load_id_joins(join_id_csv: Path) -> list[tuple[int, int, pd.Timestamp]]:
    """Read join_ID.csv (columns ID1, ID2, date; date as D.M.YYYY H:MM).

    Returns a list of (id1, id2, effective_date) rules: from effective_date
    onward, every occurrence of id2 becomes id1 (see apply_id_joins) - e.g. a
    person was issued a new beacon at that time, so id2's later contacts
    should count towards id1. Before effective_date, id2 keeps its own
    identity (it may have belonged to someone else, or the reissue simply
    hadn't happened yet). A missing or empty file means no joins are
    configured - not an error.
    """
    if not join_id_csv.exists() or join_id_csv.stat().st_size == 0:
        return []
    try:
        joins = pd.read_csv(join_id_csv)
    except pd.errors.EmptyDataError:
        return []
    if joins.empty:
        return []

    # Column names, not just values, need stripping: the file's header uses
    # ", "-separated names ("ID1, ID2, date"), so pandas would otherwise read
    # the second/third columns as " ID2" / " date" (with a leading space) and
    # joins["ID2"] below would raise a KeyError.
    joins.columns = [column.strip() for column in joins.columns]
    joins["ID1"] = joins["ID1"].astype("int64")
    joins["ID2"] = joins["ID2"].astype("int64")
    joins["date"] = pd.to_datetime(joins["date"].astype(str).str.strip(), format="%d.%m.%Y %H:%M")

    return list(joins[["ID1", "ID2", "date"]].itertuples(index=False, name=None))


def apply_id_joins(
    df: pd.DataFrame,
    id_columns: list[str],
    timestamp_column: str,
    join_rules: list[tuple[int, int, pd.Timestamp]],
) -> pd.DataFrame:
    """Replace IDs in the given columns with their join_ID.csv canonical ID, from each rule's effective date on.

    Rules are applied in chronological (effective_date) order, which
    resolves a chain (e.g. 9->5 effective D1, 5->2 effective D2 >= D1) in a
    single pass; repeating that up to len(join_rules) times (stopping as
    soon as a pass changes nothing) also catches a chain given out of date
    order, without needing to special-case it.
    """
    if not join_rules:
        return df
    df = df.copy()
    ordered_rules = sorted(join_rules, key=lambda rule: rule[2])
    for _ in range(len(join_rules)):
        changed = False
        for id1, id2, effective_date in ordered_rules:
            for column in id_columns:
                mask = (df[column] == id2) & (df[timestamp_column] >= effective_date)
                if mask.any():
                    df.loc[mask, column] = id1
                    changed = True
        if not changed:
            break
    return df


def load_ignore_times(ignore_times_csv: Path) -> pd.DataFrame:
    """Read ignore_times.csv (columns ID, start, end; dates as D.M.YYYY H:MM).

    Returns ID, start, end (start/end parsed to Timestamp) - one row per
    blackout window during which that ID's contacts should be dropped
    entirely (see apply_ignore_times). A missing or empty file means no
    windows are configured - not an error.
    """
    if not ignore_times_csv.exists() or ignore_times_csv.stat().st_size == 0:
        return pd.DataFrame(columns=["ID", "start", "end"])
    try:
        times = pd.read_csv(ignore_times_csv)
    except pd.errors.EmptyDataError:
        return pd.DataFrame(columns=["ID", "start", "end"])
    if times.empty:
        return pd.DataFrame(columns=["ID", "start", "end"])

    times.columns = [column.strip() for column in times.columns]
    times["ID"] = times["ID"].astype("int64")
    times["start"] = pd.to_datetime(times["start"].str.strip(), format="%d.%m.%Y %H:%M")
    times["end"] = pd.to_datetime(times["end"].str.strip(), format="%d.%m.%Y %H:%M")
    return times[["ID", "start", "end"]]


def _drop_ignore_time_rows(
    df: pd.DataFrame,
    id_columns: list[str],
    timestamp_column: str,
    ignore_times: pd.DataFrame,
) -> pd.DataFrame:
    if ignore_times.empty:
        return df
    drop_mask = pd.Series(False, index=df.index)
    for id_, start, end in ignore_times.itertuples(index=False):
        involved = pd.Series(False, index=df.index)
        for column in id_columns:
            involved |= df[column] == id_
        in_window = (df[timestamp_column] >= start) & (df[timestamp_column] <= end)
        drop_mask |= involved & in_window
    return df[~drop_mask]


def apply_ignore_times(contacts: pd.DataFrame, ignore_times: pd.DataFrame) -> pd.DataFrame:
    """Drop every contact row involving an ID during one of its ignore_times.csv blackout windows."""
    return _drop_ignore_time_rows(contacts, ["ID1", "ID2"], "Contact Local Time", ignore_times)


def apply_ignore_times_to_self_reports(self_reports: pd.DataFrame, ignore_times: pd.DataFrame) -> pd.DataFrame:
    """Drop every self-report from an ID during one of its ignore_times.csv blackout windows."""
    return _drop_ignore_time_rows(self_reports, ["ID"], "Local Time", ignore_times)


def load_contacts(
    contacts_paths: list[Path],
    always_exclude: set[int],
    id_joins: list[tuple[int, int, pd.Timestamp]] | None = None,
    ignore_times: pd.DataFrame | None = None,
) -> pd.DataFrame:
    """Load ID1/ID2/RSSI/timestamp rows from contacts_*.csv, dropping only always_exclude IDs.

    Keeps both person-person and person-room rows - callers filter further
    for whichever analysis they're building. If given, id_joins (see
    load_id_joins) is applied to ID1/ID2 and ignore_times (see
    load_ignore_times) drops blacked-out rows, both before returning - every
    caller sees already-joined IDs and already-dropped blackout windows.
    """
    frames = []
    for path in contacts_paths:
        df = pd.read_csv(
            path,
            dtype={"ID1": "int32", "ID2": "int32", "RSSI": "int32"},
            parse_dates=["Contact Local Time"],
        )
        df = df[~df["ID1"].isin(always_exclude) & ~df["ID2"].isin(always_exclude)]
        if not df.empty:
            frames.append(df)
    if not frames:
        return pd.DataFrame(columns=["ID1", "ID2", "RSSI", "Contact Local Time"])

    contacts = pd.concat(frames, ignore_index=True)
    # Ignore-times windows name a raw device ID (a beacon that malfunctioned
    # or was mislaid), so they're applied before joins fold IDs together -
    # a join happening later shouldn't change which raw ID a blackout window
    # refers to.
    if ignore_times is not None and not ignore_times.empty:
        contacts = apply_ignore_times(contacts, ignore_times)
    if id_joins:
        contacts = apply_id_joins(contacts, ["ID1", "ID2"], "Contact Local Time", id_joins)
    return contacts


def bucket_into_days(timestamps: pd.Series, day_start_hour: float = 0) -> pd.Series:
    """Each timestamp's day-bucket start, for days that begin at day_start_hour instead of midnight.

    E.g. day_start_hour=4 makes a "day" run 04:00 to next day's 04:00, so a
    01:00 timestamp belongs to the previous calendar day's bucket - useful
    for school/event data where activity routinely runs past midnight and a
    plain calendar-day split would cut a day in the middle of the night.
    Returns the actual start-of-bucket timestamp (not the shifted one), e.g.
    "2026-08-14 04:00:00".
    """
    shift = pd.Timedelta(hours=day_start_hour)
    return (timestamps - shift).dt.normalize() + shift


def load_self_reports(
    self_reports_csv: Path,
    id_joins: list[tuple[int, int, pd.Timestamp]] | None = None,
    ignore_times: pd.DataFrame | None = None,
) -> pd.DataFrame:
    """Load self_reports.csv (written by postprocessing.py). Returns ID, Local Time.

    If given, ignore_times (see load_ignore_times) drops reports from an ID
    during its blackout window, then id_joins (see load_id_joins) is applied
    to ID - same order and same reasoning as load_contacts: a blackout window
    names a raw device ID, so it's applied before joins fold IDs together.
    """
    reports = pd.read_csv(
        self_reports_csv,
        dtype={"ID": "int32"},
        parse_dates=["Local Time"],
    )
    if ignore_times is not None and not ignore_times.empty:
        reports = apply_ignore_times_to_self_reports(reports, ignore_times)
    if id_joins:
        reports = apply_id_joins(reports, ["ID"], "Local Time", id_joins)
    return reports


def dedupe_self_reports(self_reports: pd.DataFrame, min_gap_min: float) -> pd.DataFrame:
    """Drop a report that falls within min_gap_min minutes of the same ID's preceding report.

    A burst of repeated button presses (e.g. three reports 10-20s apart) is
    one real event, not several, so only the first report of each such burst
    is kept - the same gap-chaining rule as build_sessions: a report only
    starts a new kept event if it's more than min_gap_min past the *previous*
    report (kept or not), so a long burst collapses to a single report
    instead of one every min_gap_min.
    """
    if self_reports.empty:
        return self_reports
    reports = self_reports.sort_values(["ID", "Local Time"])
    gap = reports.groupby("ID")["Local Time"].diff()
    starts_new_burst = gap.isna() | (gap > pd.Timedelta(minutes=min_gap_min))
    return reports[starts_new_burst]


def match_self_reports_to_rooms(
    self_reports: pd.DataFrame,
    room_sessions: pd.DataFrame,
    window_min: float,
) -> tuple[pd.DataFrame, int]:
    """For each self-report, every room it has an overlapping session with inside +/- window_min.

    Returns (matches, matched_report_count):
    - matches: columns room, report_time (the report's own, exact timestamp -
      callers bucket it into whatever they need: a day, a wall-clock segment,
      an animation frame) - one row per (report, room) match, so a report
      matching two rooms contributes two rows.
    - matched_report_count: number of self-reports with at least one match
      (<= len(self_reports); the rest had no qualifying room session in the
      window and are excluded entirely).
    """
    window = pd.Timedelta(minutes=window_min)

    sessions_by_person: dict[int, list[tuple[str, pd.Timestamp, pd.Timestamp]]] = {}
    for person, room, start, end in room_sessions[["person", "room", "start", "end"]].itertuples(index=False):
        sessions_by_person.setdefault(person, []).append((room, start, end))

    matches = []
    matched_report_count = 0
    for person, ts in self_reports[["ID", "Local Time"]].itertuples(index=False):
        window_start = ts - window
        window_end = ts + window
        matched_rooms = {
            room
            for room, start, end in sessions_by_person.get(person, [])
            if start <= window_end and end >= window_start
        }
        if matched_rooms:
            matched_report_count += 1
        for room in matched_rooms:
            matches.append((room, ts))

    return pd.DataFrame(matches, columns=["room", "report_time"]), matched_report_count


def build_sessions(
    events: pd.DataFrame,
    group_cols: list[str],
    min_duration_sec: float,
    max_gap_sec: float,
) -> pd.DataFrame:
    """Qualifying contact sessions per group_cols key.

    A contact "session" is a run of events for the same key with no gap
    larger than max_gap_sec between consecutive events; a session's duration
    is its last-minus-first timestamp, and it only qualifies if that duration
    reaches min_duration_sec (a lone blip, or a run too short overall,
    contributes nothing - the "sustained for at least N seconds" filter used
    throughout this project). events must already be RSSI-filtered.

    Returns group_cols + ["start", "end", "duration"] (duration in seconds).
    """
    if events.empty:
        return pd.DataFrame(columns=group_cols + ["start", "end", "duration"])

    events = events.sort_values(group_cols + ["Contact Local Time"])

    gap = events.groupby(group_cols)["Contact Local Time"].diff()
    new_session = gap.isna() | (gap > pd.Timedelta(seconds=max_gap_sec))
    events = events.copy()
    events["session_id"] = new_session.cumsum()  # monotonic across the whole (sorted) frame is fine

    sessions = events.groupby(group_cols + ["session_id"])["Contact Local Time"].agg(start="min", end="max")
    sessions = sessions.reset_index(drop=False).drop(columns="session_id")
    sessions["duration"] = (sessions["end"] - sessions["start"]).dt.total_seconds()
    return sessions[sessions["duration"] >= min_duration_sec]


def sessions_total_seconds(
    events: pd.DataFrame,
    group_cols: list[str],
    min_duration_sec: float,
    max_gap_sec: float,
) -> pd.DataFrame:
    """Total qualifying contact time per group_cols key, in seconds. Returns group_cols + ["seconds"]."""
    sessions = build_sessions(events, group_cols, min_duration_sec, max_gap_sec)
    if sessions.empty:
        return pd.DataFrame(columns=group_cols + ["seconds"])
    totals = sessions.groupby(group_cols)["duration"].sum().reset_index()
    return totals.rename(columns={"duration": "seconds"})


def person_person_events(contacts: pd.DataFrame, room_ids: set[int], rssi_threshold: int) -> pd.DataFrame:
    """RSSI-filtered person-person rows (pmin/pmax added), for session building."""
    events = contacts[
        (contacts["RSSI"] > rssi_threshold) & ~contacts["ID1"].isin(room_ids) & ~contacts["ID2"].isin(room_ids)
    ].copy()
    if events.empty:
        return events
    events["pmin"] = events[["ID1", "ID2"]].min(axis=1)
    events["pmax"] = events[["ID1", "ID2"]].max(axis=1)
    return events


def compute_person_person_sessions(
    contacts: pd.DataFrame,
    room_ids: set[int],
    rssi_threshold: int,
    min_duration_sec: float,
    max_gap_sec: float,
) -> pd.DataFrame:
    """Qualifying person-person contact sessions. Returns pmin, pmax, start, end, duration."""
    events = person_person_events(contacts, room_ids, rssi_threshold)
    if events.empty:
        return pd.DataFrame(columns=["pmin", "pmax", "start", "end", "duration"])
    return build_sessions(events, ["pmin", "pmax"], min_duration_sec, max_gap_sec)


def person_room_events(contacts: pd.DataFrame, id_to_room_name: dict[int, str], rssi_threshold: int) -> pd.DataFrame:
    """RSSI-filtered person-room rows (person/room added), for session building.

    A room's several beacons are merged into one timeline per person (the
    "room" column, not the individual beacon ID) before session detection, so
    overlapping detections from two beacons in the same room extend one
    session instead of creating two to (double-)sum.
    """
    room_ids = set(id_to_room_name)
    filtered = contacts[contacts["RSSI"] > rssi_threshold]

    id1_is_room = filtered["ID1"].isin(room_ids)
    id2_is_room = filtered["ID2"].isin(room_ids)

    room_is_1 = filtered[id1_is_room & ~id2_is_room].rename(columns={"ID2": "person", "ID1": "room_id"})
    room_is_2 = filtered[id2_is_room & ~id1_is_room].rename(columns={"ID1": "person", "ID2": "room_id"})
    events = pd.concat([room_is_1, room_is_2], ignore_index=True)
    if events.empty:
        return events
    events["room"] = events["room_id"].map(id_to_room_name)
    return events


def compute_person_room_seconds(
    contacts: pd.DataFrame,
    id_to_room_name: dict[int, str],
    rssi_threshold: int,
    min_duration_sec: float,
    max_gap_sec: float,
) -> pd.DataFrame:
    """Total qualifying contact time per (person, room) pair. Returns person, room, seconds."""
    events = person_room_events(contacts, id_to_room_name, rssi_threshold)
    if events.empty:
        return pd.DataFrame(columns=["person", "room", "seconds"])
    return sessions_total_seconds(events, ["person", "room"], min_duration_sec, max_gap_sec)


def compute_person_room_sessions(
    contacts: pd.DataFrame,
    id_to_room_name: dict[int, str],
    rssi_threshold: int,
    min_duration_sec: float,
    max_gap_sec: float,
) -> pd.DataFrame:
    """Qualifying person-room contact sessions. Returns person, room, start, end, duration."""
    events = person_room_events(contacts, id_to_room_name, rssi_threshold)
    if events.empty:
        return pd.DataFrame(columns=["person", "room", "start", "end", "duration"])
    return build_sessions(events, ["person", "room"], min_duration_sec, max_gap_sec)


def build_matrix(totals: pd.DataFrame, persons: list[int]) -> np.ndarray:
    """Symmetric person x person matrix of total contact seconds, in persons' order."""
    index = {pid: i for i, pid in enumerate(persons)}
    n = len(persons)
    matrix = np.zeros((n, n), dtype="float64")
    for pmin, pmax, seconds in totals.itertuples(index=False):
        i, j = index[pmin], index[pmax]
        matrix[i, j] = seconds
        matrix[j, i] = seconds
    return matrix


def build_bipartite_matrix(totals: pd.DataFrame, persons: list[int], rooms: list[str]) -> np.ndarray:
    """person x room matrix of total contact seconds, in persons'/rooms' order."""
    row_index = {pid: i for i, pid in enumerate(persons)}
    col_index = {name: j for j, name in enumerate(rooms)}
    matrix = np.zeros((len(persons), len(rooms)), dtype="float64")
    for person, room, seconds in totals.itertuples(index=False):
        matrix[row_index[person], col_index[room]] = seconds
    return matrix


def uniform_bucket_edges(bucket_minutes: int = TIME_BUCKET_MINUTES) -> list[int]:
    """Minute-of-day bucket boundaries for a uniform raster: [0, step, 2*step, ..., 1440]."""
    return list(range(0, 24 * 60 + 1, bucket_minutes))


def variable_bucket_edges(
    coarse_window_min: tuple[int, int] = TIME_RASTER_COARSE_WINDOW_MIN,
    coarse_minutes: int = TIME_RASTER_COARSE_MINUTES,
    fine_minutes: int = TIME_RASTER_FINE_MINUTES,
) -> list[int]:
    """Minute-of-day bucket boundaries for the variable time-of-day raster.

    coarse_minutes-wide buckets inside coarse_window_min (a [start, end) pair of
    minutes from midnight), fine_minutes-wide buckets everywhere else. The window
    edges are always landed on exactly, so the two rasters meet cleanly. Always
    starts at 0 and ends at 1440.
    """
    lo, hi = coarse_window_min
    edges: list[int] = []
    m = 0
    while m < 24 * 60:
        edges.append(m)
        if lo <= m < hi:
            m = min(m + coarse_minutes, hi)
        else:
            nxt = m + fine_minutes
            if m < lo < nxt:
                nxt = lo
            elif m < hi < nxt:
                nxt = hi
            m = nxt
    edges.append(24 * 60)
    return edges


def compute_time_of_day_matrix(
    sessions: pd.DataFrame,
    bucket_minutes: int = TIME_BUCKET_MINUTES,
    bucket_edges: list[int] | None = None,
) -> tuple[np.ndarray, list[pd.Timestamp]]:
    """day x time-of-day matrix of total contact seconds (any session table with start/end columns).

    Weight is every qualifying session's duration (summed across all keys),
    attributed to wall-clock day/time-of-day buckets. A session that spans a
    bucket boundary (e.g. two people together from 14:20 to 15:10) is split
    proportionally across every bucket it overlaps, rather than dumped entirely
    into the one containing its start - sessions routinely run well past a
    single bucket, so that would distort the picture.

    Buckets are given by bucket_edges (minute-of-day boundaries, strictly
    increasing, starting at 0 and ending at 1440); if omitted, a uniform
    bucket_minutes-wide raster is used. Bucket boundaries never cross midnight.

    Returns (matrix, days) with matrix.shape == (len(bucket_edges) - 1, len(days)).
    """
    edges = [int(e) for e in bucket_edges] if bucket_edges is not None else uniform_bucket_edges(bucket_minutes)
    edges_arr = np.asarray(edges, dtype="float64")
    slots_per_day = len(edges) - 1

    totals: dict[tuple[pd.Timestamp, int], float] = {}
    for start, end in zip(sessions["start"], sessions["end"]):
        cur = start
        while cur < end:
            day = cur.normalize()
            minute_of_day = (cur - day).total_seconds() / 60.0
            slot_idx = int(np.searchsorted(edges_arr, minute_of_day, side="right")) - 1
            slot_idx = min(max(slot_idx, 0), slots_per_day - 1)
            slot_end = day + pd.Timedelta(minutes=float(edges[slot_idx + 1]))
            seg_end = min(end, slot_end)
            overlap_sec = (seg_end - cur).total_seconds()
            key = (day, slot_idx)
            totals[key] = totals.get(key, 0.0) + overlap_sec
            cur = seg_end

    days = sorted({day for day, _ in totals})
    day_index = {day: i for i, day in enumerate(days)}
    matrix = np.zeros((slots_per_day, len(days)), dtype="float64")
    for (day, slot_idx), seconds in totals.items():
        matrix[slot_idx, day_index[day]] = seconds
    return matrix, days
