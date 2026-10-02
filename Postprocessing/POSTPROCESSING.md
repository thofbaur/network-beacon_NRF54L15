# postprocessing.py

Turns the raw `*.log` files written by `dsa_logger.py` into per-beacon and
per-contact CSV summaries. Fully standalone (`python postprocessing.py`),
with no dependency on `dsa_logger.py` or any other script in this repo, and
nothing in this repo imports it either: `dsa_logger.py` only writes raw
`*.log` files and never calls into postprocessing - run this script
separately, after a capture, to produce the CSVs.

## Input

All `*.log` files in `--log-dir` (default: `Logs/` next to the script). Each
line has the form:

```
<timestamp>,ID: <beacon_id>,<rest>
```

where `<rest>` is one of several message kinds a beacon reports:
`Current Timer: N`, `Voltage: N`, `Status: N`, `Contact Count: N`,
`ID2: X, Timer: N, RSSI: N` (a contact), `Self-report time: N`, or
`Eco Session Enter: N, Leave: N`. Lines that don't parse are silently
skipped.

Beacons report past events (contacts, self-reports, eco sessions) as a timer
tick count relative to their own clock. The most recent preceding
`Current Timer` line for that beacon ties that clock to a real timestamp, so
every such event is resolved as `reference_timestamp - (reference_timer -
event_timer)` seconds. If a beacon's clock resets (reboot) mid-session, it
can still report a backlog of events from before the reset; the script
tracks the previous `Current Timer` reference too and retries against it
when resolving against the current one lands implausibly in the future
(`resolve_event_timestamp`).

## Output

All written into `--output-dir` (default: `Output/`):

| File | Contents |
|---|---|
| `beacon_summary.csv` | One row per beacon: last-seen time, last battery voltage, decoded fault bits from its last `Status` byte. |
| `contacts_YYYYMMDD.csv` | One file per calendar day: `ID1` (reporting beacon), `ID2` (seen beacon), `RSSI`, resolved local contact time. |
| `self_reports.csv` | One row per self-report: beacon ID and resolved local time. |
| `eco_sessions.csv` | One row per eco session: beacon ID, resolved enter/exit local time. |
| `measurements.csv` | One row per `Current Timer` readout: beacon, readout time, battery voltage, stored contact count, and the calculated time the beacon's timer last read 0. |
| `current_issues.csv` | One row per (beacon, issue): not seen recently, low battery, or a decoded fault bit - derived from `beacon_summary.csv` state. |
| `sanity_findings.csv` | One row per log file, one column per sanity check, counting how many entries that check skipped (or, for the informational checks, flagged) in that file. |
| `transfer_mislabel.md` | Every pair of chronologically adjacent contact lines that report a different `ID:` with no connection-boundary line between them - a potential mislabeling, flagged for manual review, not auto-dropped. |

`beacon_summary.csv`, `self_reports.csv`, and `eco_sessions.csv` are seeded
from their own previous output before a run, so beacons/events whose source
log files have since been archived away aren't lost. `contacts_*.csv` files
don't need this since each day's file is already final once no more logs for
that day remain.

## Data-quality handling

An entry is dropped (and counted in `sanity_findings.csv`) if:
- its beacon ID (or, for a contact, the ID2 it names) falls outside the
  fielded roster (`DEFAULT_VALID_ID_MIN`/`MAX`/`DEFAULT_VALID_EXTRA_IDS`),
- a contact's RSSI falls outside the plausible radio range,
- it has no `Current Timer` reference at all (neither a preceding nor, within
  the same run, a following one),
- its timer-resolved timestamp is physically impossible (later than the log
  line's own capture time, beyond a small tolerance) even after retrying
  against the previous `Current Timer` reference, or
- its timer-resolved timestamp falls outside `--valid-date-start`/
  `--valid-date-end` - a scoping exclusion, tracked separately from the
  checks above since it isn't a sign of corrupted data.

See `CLAUDE.md` in this repo for a known, narrower data defect (ID1
low/high-range double-mapping) that isn't caught by any of the above and
needs separate manual resolution.

## Useful CLI flags

Run `python postprocessing.py --help` for the full list. The most relevant:

- `--log-dir` / `--output-dir` - override the default `Logs/` / `Output/` locations.
- `--stale-hours` / `--low-battery-mv` - thresholds for `current_issues.csv`.
- `--valid-date-start` / `--valid-date-end` - the accepted observation window for resolved event timestamps.
- Per-output `--*-csv` flags to redirect any individual output file.
