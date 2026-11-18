"""pytest configuration of the BeaconNRF54 test suite.

Fast tests (classes A-E static/unit/contract) run by default. Hardware and
long-running tests are opt-in:

    --hil                 run hardware-in-the-loop tests (classes F, G, H)
    --boards FILE         board description (default: 0_Environment/boards.toml)
    --slow                include slow tests (soak, power-cut sweeps)
    --run-builds          run the west build matrix (E1, E2)
    --release-build-dir   build directory of a release build to inspect (E3)
    --record              store this run in 3_Results/<date>_<git hash>/
    --coverage            measure C (firmware) and Python coverage; report in the
                          recorded run folder, or .build/coverage/ without --record
"""

from __future__ import annotations

import platform
import subprocess
import sys
from datetime import datetime
from pathlib import Path

import pytest

IMPLEMENTATION = Path(__file__).resolve().parent
TESTSUITE = IMPLEMENTATION.parent
for extra in (IMPLEMENTATION / "lib", IMPLEMENTATION / "hil"):
    if str(extra) not in sys.path:
        sys.path.insert(0, str(extra))

RESULTS = TESTSUITE / "3_Results"


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("dsa", "BeaconNRF54 test suite")
    group.addoption("--hil", action="store_true", help="run hardware-in-the-loop tests")
    group.addoption(
        "--boards",
        default=str(TESTSUITE / "0_Environment" / "boards.toml"),
        help="board description file for hardware tests",
    )
    group.addoption("--slow", action="store_true", help="include slow tests")
    group.addoption("--run-builds", action="store_true", help="run the west build matrix (E1/E2)")
    group.addoption("--release-build-dir", default=None, help="release build directory for E3")
    group.addoption("--record", action="store_true", help="store junit.xml and a summary in 3_Results/")
    group.addoption("--coverage", action="store_true", help="measure firmware (C) and Python coverage")


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    skip_hil = pytest.mark.skip(reason="hardware test: run with --hil")
    skip_slow = pytest.mark.skip(reason="slow test: run with --slow")
    skip_build = pytest.mark.skip(reason="build matrix: run with --run-builds")
    # Markers, not keywords: keywords also contain directory names (2_Implementation/tests/hil,
    # 2_Implementation/tests/build) and would skip unmarked tests in those folders.
    for item in items:
        if item.get_closest_marker("hil") and not config.getoption("--hil"):
            item.add_marker(skip_hil)
        if item.get_closest_marker("slow") and not config.getoption("--slow"):
            item.add_marker(skip_slow)
        if item.get_closest_marker("build") and not config.getoption("--run-builds"):
            item.add_marker(skip_build)


# ---- result recording (--record) --------------------------------------------
def _git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=TESTSUITE, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else ""


@pytest.hookimpl(tryfirst=True)
def pytest_configure(config: pytest.Config) -> None:
    # tryfirst: the junit path must be set before pytest's junitxml plugin reads it
    _configure_record(config)
    _configure_coverage(config)


def _configure_record(config: pytest.Config) -> None:
    if not config.getoption("--record", default=False) or hasattr(config, "workerinput"):
        return
    commit = _git("rev-parse", "--short", "HEAD") or "nogit"
    dirty = "-dirty" if _git("status", "--porcelain") else ""
    run_dir = RESULTS / f"{datetime.now():%Y-%m-%d_%H%M%S}_{commit}{dirty}"
    run_dir.mkdir(parents=True, exist_ok=True)
    config._dsa_run_dir = run_dir
    config.option.xmlpath = str(run_dir / "junit.xml")  # picked up by pytest's junitxml plugin

    boards = Path(config.getoption("--boards"))
    lines = [
        f"# Test run {run_dir.name}",
        "",
        "| | |",
        "|---|---|",
        f"| Date | {datetime.now():%Y-%m-%d %H:%M:%S} |",
        f"| Commit | {_git('rev-parse', 'HEAD') or 'n/a'}{' (uncommitted changes)' if dirty else ''} |",
        f"| Branch | {_git('rev-parse', '--abbrev-ref', 'HEAD') or 'n/a'} |",
        f"| OS | {platform.platform()} |",
        f"| Python | {platform.python_version()} |",
        f"| Arguments | `{' '.join(config.invocation_params.args) or '(default)'}` |",
        f"| Hardware tests | {'yes' if config.getoption('--hil') else 'no'} |",
    ]
    if config.getoption("--hil") and boards.exists():
        lines += ["", "## Boards", "", "```toml", boards.read_text(encoding="utf-8").strip(), "```"]
    (run_dir / "run_info.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---- coverage (--coverage) ----------------------------------------------------
def _configure_coverage(config: pytest.Config) -> None:
    if not config.getoption("--coverage", default=False) or hasattr(config, "workerinput"):
        return
    import shutil

    import coverage

    from coverage_report import PYTHON_SOURCES

    run_dir = getattr(config, "_dsa_run_dir", None)
    out_dir = (run_dir / "coverage") if run_dir else (TESTSUITE / ".build" / "coverage")
    shutil.rmtree(out_dir, ignore_errors=True)
    (out_dir / "c_raw").mkdir(parents=True)
    # gcov accumulates counters across runs: start every coverage run from zero.
    for stale in (TESTSUITE / ".build" / "firmware_unit_coverage").glob("*.gcda"):
        stale.unlink()
    config._dsa_cov_dir = out_dir
    config._dsa_cov_executables = []  # filled by tests/firmware_unit/conftest.py
    cov = coverage.Coverage(source=[str(p) for p in PYTHON_SOURCES], branch=True,
                            data_file=str(out_dir / ".coverage"))
    cov.start()
    config._dsa_python_cov = cov


def _coverage_summary(config: pytest.Config, terminalreporter) -> list[str]:
    out_dir = getattr(config, "_dsa_cov_dir", None)
    if out_dir is None:
        return []
    import coverage_report

    cov = config._dsa_python_cov
    cov.stop()
    cov.save()
    sections = []
    try:
        from repo import firmware_unit_build

        toolchain = firmware_unit_build().find_coverage_toolchain()
        sections.append(coverage_report.c_report(toolchain, config._dsa_cov_executables,
                                                 out_dir / "c_raw", out_dir))
    except Exception as exc:  # report the problem, keep the test results
        sections.append(["## Firmware (C, host tests)", "", f"Not measured: {exc}"])
    sections.append(coverage_report.python_report(cov, out_dir))
    path = coverage_report.write_summary(out_dir, sections)
    raw = out_dir / "c_raw"
    if raw.exists() and not any(raw.iterdir()):
        raw.rmdir()  # gcc route: data lives next to the instrumented executables
    terminalreporter.write_line(f"Coverage report: {path}")

    def total(section: list[str]) -> list[str]:
        row = next((line for line in section if line.startswith("| **Total**")), None)
        return [] if row is None else [c.strip().strip("*").strip() for c in row.strip("|").split("|")][1:]

    rows = ["| Code | Lines | Branches | Functions |", "|---|---|---|---|"]
    c_total = total(sections[0])
    rows.append("| Firmware C (host tests) | " + (" | ".join(c_total) if c_total else "not measured | | ") + " |")
    py_total = total(sections[1])
    rows.append(f"| Python (statements + branches) | {py_total[-1] if py_total else 'not measured'} | | |")
    return rows


def pytest_terminal_summary(terminalreporter, exitstatus, config: pytest.Config) -> None:
    coverage_totals = _coverage_summary(config, terminalreporter)
    run_dir = getattr(config, "_dsa_run_dir", None)
    if run_dir is None:
        return
    stats = terminalreporter.stats
    counts = {key: len(stats.get(key, [])) for key in ("passed", "failed", "error", "skipped", "xfailed")}
    lines = [
        f"# Summary {run_dir.name}",
        "",
        "| Passed | Failed | Errors | Skipped |",
        "|---|---|---|---|",
        f"| {counts['passed']} | {counts['failed']} | {counts['error']} | {counts['skipped']} |",
        "",
        f"Exit status: {int(exitstatus)}",
    ]
    if coverage_totals:
        lines += ["", "## Coverage", "", *coverage_totals, "", "Per file: `coverage/coverage.md`"]
    failed = [r for key in ("failed", "error") for r in stats.get(key, [])]
    if failed:
        lines += ["", "## Failed", "", "Compare with `3_Results/Findings.md`.", ""]
        for report in failed:
            message = str(getattr(report, "longrepr", "")).strip().splitlines()
            reason = next((m.strip() for m in reversed(message) if m.strip().startswith("E ")), "")
            lines.append(f"- `{report.nodeid}` {reason[:200]}")
    (run_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    terminalreporter.write_line(f"Results recorded in {run_dir}")
