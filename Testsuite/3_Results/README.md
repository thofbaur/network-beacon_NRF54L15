# Results

| Content | Created by |
|---|---|
| `Findings.md` | Register of defects found by the suite, with status |
| `<YYYY-MM-DD_HHMMSS>_<git hash>[-dirty]/` | One folder per recorded run: `pytest --record` writes `junit.xml`, `run_info.md` (commit, OS, options, boards) and `summary.md` (counts, failed tests); with `--coverage` also `coverage/` (`coverage.md`, HTML reports) |
| `Tag_Checklists/` | Filled and signed copies of `1_Specification/TAG_CHECKLIST.md`, one per deployment |

Only results worth keeping belong here. Build output is regenerated in
`Testsuite/.build/` (git-ignored). A run on a commit with uncommitted
changes is marked `-dirty`; release records should come from a clean
checkout.
