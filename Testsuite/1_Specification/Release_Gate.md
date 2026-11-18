# Release gate

Which test classes must pass before code is merged, a firmware release is
made, or tags go into the field. "Pass" means: all tests green, or every
failure listed as an open finding in `3_Results/Findings.md` with an
accepted risk.

| Gate | Required | Record |
|---|---|---|
| Merge to main | A–E (`pytest`); F and G when storage, NUS or radio code changed | optional |
| Firmware release | A–H on the release commit including `--slow`; G10 against the previous release tag; E3 with `--release-build-dir` of the release build; coverage report reviewed for new untested branches | `pytest --record --coverage` run in `3_Results/` |
| Field deployment | Release gate passed; H9 with the deployment tags; class I signed off | Filled `TAG_CHECKLIST.md` in `3_Results/Tag_Checklists/` |

## Rules

- **Flaky hardware tests.** BLE steps retry the connection, never the
  check. A test that only passes on retry counts as flaky and is
  investigated before the release.
- **New decisions.** Every new entry in `DECISIONS.md` gets a test and a
  row in `TRACEABILITY.md` (test A5 fails otherwise).
- **New field bugs.** Every defect found in the field gets a regression
  test reproducing it before it is fixed.
- **Coverage.** `pytest --coverage` (gcov/llvm-cov for the firmware,
  coverage.py for Python) is used to find untested branches in the storage
  and command code, not as a target number. Before a release, check the
  uncovered branches of changed firmware files and add tests where a
  branch can lose or corrupt data.
