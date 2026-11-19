# BeaconNRF54 test suite

Automated and manual tests for the DSA beacon system: beacon, Base, Control
and Scan_New_Tags firmware, the shared protocol header and the logger
(`Network_Logger/dsa_logger.py`). `Postprocessing/` and
`Evaluation/` are out of scope for now.

## Folders

| Folder | Content |
|---|---|
| `0_Environment/` | Setup scripts, `requirements.txt`, `boards.example.toml` (copy to `boards.toml` for hardware tests), the Python environment `.venv/` |
| `1_Specification/` | Test specifications per class (`A_static.md` … `I_manual_tag.md`: what is tested, inputs, expected result per case), `TRACEABILITY.md` (design decision → tests), `Release_Gate.md`, `TAG_CHECKLIST.md` (blank protocol for the manual tag tests) |
| `2_Implementation/` | Test code: `tests/` (pytest, all classes), `firmware_unit/` (host-compiled C tests), `firmware_target/` (F1 on-board app), `hil/` (hardware test support), `lib/` (shared helpers), `configs/` (test-only firmware configurations), `conftest.py` |
| `3_Results/` | `Findings.md` (defects found, with status), one folder per recorded run (git-ignored), filled tag checklists – see [Results](#results) |
| `.build/` | Regenerable build output (C tests, west builds) – git-ignored |

## Quick start

```powershell
# Windows                                     # macOS / Linux
.\0_Environment\setup.ps1                     # ./0_Environment/setup.sh
.\0_Environment\.venv\Scripts\python -m pytest
```

The setup script also installs the git pre-commit hook (see below).
Run pytest from `Testsuite/`. The first run compiles the C unit tests
(about a minute with the bundled `ziglang` compiler; gcc or clang are used
when installed).

| Command (from `Testsuite/`) | Runs | Needs |
|---|---|---|
| `pytest` | A, B, C, D, E3 (static) | Python only |
| `pytest --record` | same, stored in `3_Results/<date>_<git hash>/` | – |
| `pytest --coverage` | same, plus coverage report (firmware C + Python) | C: gcc + gcov or clang + llvm-cov (see below) |
| `pytest --run-builds 2_Implementation/tests/build` | E1, E2 (west build matrix) | nRF Connect SDK v3.3.0 (pinned in `hil/firmware.py`, found automatically) |
| `pytest 2_Implementation/tests/build --release-build-dir <dir>` | E3 on a real release build | a release build directory |
| `pytest --hil 2_Implementation/tests/hil/test_f_dk_a.py` | F | DK A, `0_Environment/boards.toml` |
| `pytest --hil 2_Implementation/tests/hil/test_g_ble.py` | G | DK A + Bluetooth adapter |
| `pytest --hil 2_Implementation/tests/hil/test_h_two_dk.py` | H | DK A + DK B (+ Bluetooth) |
| `pytest --hil --slow …` | adds F3, G7, G8, G10, G11, H8, H10 | as above, hours |
| `pytest --hil -m tags` | H9 | tags listed in `boards.toml` |
| `python 2_Implementation/firmware_unit/build.py --run` | C without pytest | C compiler |
| `1_Specification/TAG_CHECKLIST.md` | I | tags, Base, lab equipment |

For hardware tests copy `0_Environment/boards.example.toml` to
`0_Environment/boards.toml` and enter the J-Link serial numbers, beacon IDs
and DK B's VCOM port. Use `--record` for every run that should be kept
(release runs: required, see `1_Specification/Release_Gate.md`).

## Results

| Content of `3_Results/` | Created by | Versioned |
|---|---|---|
| `Findings.md` | Register of defects found by the suite, with status | yes |
| `<YYYY-MM-DD_HHMMSS>_<git hash>[-dirty]/` | One folder per recorded run: `pytest --record` writes `junit.xml`, `run_info.md` (commit, OS, options, boards) and `summary.md` (counts, failed tests); with `--coverage` also `coverage/` (`coverage.md`, HTML reports) | no (git-ignored) |
| `Tag_Checklists/` | Filled and signed copies of `1_Specification/TAG_CHECKLIST.md`, one per deployment | yes |

Run folders stay on the computer that made them; copy a run you need to
keep (e.g. a release record) somewhere permanent. A run on a commit with
uncommitted changes is marked `-dirty`; release records should come from a
clean checkout. Build output is regenerated in `.build/` (git-ignored).

## Test classes

| Class | Tests | Runs on | Automated | When |
|---|---|---|---|---|
| A Static checks | A1 protocol constants, A2 ID table, A3 ID generator/workflow, A4 partitions, A5 traceability | any OS | ✅ | every commit |
| B Logger / Scan_New_Tags | B1 decoders, B2 all 256 IDs, B3 stream resync, B4 log format, B5 read_tags, B6 robustness | any OS | ✅ | every commit |
| C Firmware on the PC | C1 flash store, C2 RAM ring, C3 network, C4 self-report/eco, C5 param storage, C6 device, C7 Base parser, C8 motion, C9 radio, C10 LED, C11 battery | any OS + C compiler | ✅ | every commit |
| D Contracts | D1 Base vs logger, D2 encoder round trip, D3 Control encoding | any OS | ✅ | every commit |
| E Builds | E1 beacon, E2 other apps, E3 release guard | nRF Connect SDK | ✅ | every PR |
| F DK A | F1 storage on target, F2 boot smoke, F3 drift, F4 watchdog | DK A | ⚙️ | before release |
| G DK A + Bluetooth | G1–G12 advertisement, readout, interruptions, resets, update, full storage, open readout | DK A + adapter | ⚙️ | before release |
| H DK A + DK B | H1–H10 commands, eco, contacts, Base chain, scanner, concurrency, several beacons, detection rate | 2 DKs | ⚙️ | before release |
| I Tag | I1–I10 | tags + lab | 👤 / 👤→✅ | before deployment |

## Current state (2026-11-18)

Default run: **1254 passed, 45 skipped, 16 xfailed**. The skips are hardware
tests and missing prerequisites. The xfails are the open findings 1–8 in the
code under test – see `3_Results/Findings.md`. The build matrix (E1/E2,
9 images) passed against NCS v3.3.0 and the F1 on-board app builds.

## Known defects and regressions

A test that fails because of an open finding is marked `@finding(n)`
(`2_Implementation/lib/findings.py`; C cases are listed there in `C_CASES`).
It reports as `xfailed`, so a green run means nothing new is broken.

- New finding: add it to `Findings.md` and `OPEN_FINDINGS`, mark the failing tests.
- Finding fixed: the marked tests pass, pytest reports `XPASS(strict)` as a
  failure. Remove the marks in the same commit as the fix and close the finding.

A git pre-commit hook runs the default suite (about 8 s) and blocks the
commit on any failure. Bypass once with `git commit --no-verify`. The hook is
versioned as `0_Environment/pre-commit`; `setup.ps1` / `setup.sh` install it
into `.git/hooks/` (an existing different hook is kept as `pre-commit.bak`).
Rerun the setup script after changing the hook.

## Prerequisites still missing in the firmware

Tests that depend on these are skipped with `PREREQUISITE MISSING` and start
running as soon as the addition exists.

| Addition | Enables |
|---|---|
| `CONFIG_DSA_CONTROL_UART_COMMANDS` in Network_Control_nrf54 (protocol in `1_Specification/H_hil_two_dk.md`) | D3 (last case), E2 commander build, H1, H2, H3 thresholds, H4 |
| `CONFIG_DSA_DEV_FORCE_ID` (test-only Kconfig) | H5 with ID 48 |
| `Network_Beacon_nrf54/VERSION` + version/git hash at boot | E3 version case, F2 version case |
| NUS packet builders split out of `nus.c` (`nus_packets.h`) | D2 |
| Watchdog + test-only hang trigger (open decision) | F4 |

## Coverage

`pytest --coverage` measures which code the tests reach:

- **Firmware (C):** the C tests are built a second time with coverage instrumentation into `.build/firmware_unit_coverage/`. The report covers only the firmware sources (`Network_Beacon_nrf54/src`, `Network_Base_nrf54/src`), not the Zephyr stand-in, fakes or tests. Firmware files that no host test compiles (e.g. `nus.c`, `main.c`, the Base's radio/UART code) are listed separately: they are covered only by the hardware tests or not at all.
- **Python:** `dsa_logger.py`, `read_tags.py`, `generate_ids.py` with branch coverage (coverage.py).

Output: `coverage/` inside the run folder with `--record`, otherwise `.build/coverage/` – `coverage.md` (per-file table) plus `c_html/` and `python_html/` (line by line). Use it to find untested branches before a release, not as a target number.

C coverage needs a compiler with a coverage runtime; the bundled zig compiler has none. The suite probes and picks one automatically:

| OS | Install |
|---|---|
| Windows | `winget install BrechtSanders.WinLibs.POSIX.UCRT.LLVM` (portable, no admin, no Visual Studio; its gcc + gcov are used) |
| macOS | Xcode command line tools (clang + llvm-cov) |
| Linux | gcc (gcov) or clang + llvm |

`DSA_LLVM_BIN` can point to the tool directory if it is not found.

Status 2026-10-03: firmware 86.6 % lines / 73.9 % branches / 94.1 % functions; Python 83.6 %.

## How the C unit tests work

`2_Implementation/firmware_unit/build.py` compiles the **unmodified**
firmware sources together with:

- `shim/` – the Zephyr API subset the firmware uses. Time and work items advance only when a test says so, which makes flush/export orderings reproducible.
- `fakes/` – an in-RAM NVS that survives simulated reboots and can cut power at any write, a settings store, and recording stand-ins for neighbouring modules.
- a generated `test_config.h` – from the real `Kconfig`, `dsa.conf`, `dsa_runtime.conf` and `pm_static.yml`, so the production configuration and the firmware's own `BUILD_ASSERT`s are exercised.

The fakes follow the Zephyr NVS/settings semantics of NCS v3.3.0
(`nvs_clear()` unmounts, reads return the stored length, …); F1 checks the
same behaviour on the real chip.

To add a C test: write `MT_TEST(name)` in the matching
`2_Implementation/firmware_unit/tests/test_cN_*.c`, list it in
`MT_MAIN_WITH_SETUP(...)`, and add a row with input and expected result to
`1_Specification/C_firmware_unit.md`. pytest picks it up automatically. A
new firmware module needs a `Target` entry in `build.py`.
