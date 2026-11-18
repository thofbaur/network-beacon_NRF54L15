"""Coverage reports for `pytest --coverage`.

C (firmware): every test process writes a raw LLVM profile; they are merged
with llvm-profdata and reported with llvm-cov for the firmware sources only
(the Zephyr stand-in, fakes and tests are excluded). Firmware files that are
not compiled into any host test are listed as "not covered by host tests".

Python: coverage.py with branch coverage for the scripts under test.

Output (in the given directory): coverage.md (summary), c_html/ and
python_html/ (line-by-line reports).
"""

from __future__ import annotations

import io
import json
import subprocess
import sys
from pathlib import Path

from repo import BASE_SRC, BEACON_SRC, LOGGER_DIR, REPO, SCAN_TAGS

PYTHON_SOURCES = [LOGGER_DIR, SCAN_TAGS]
# Everything compiled into the C tests that is not firmware.
C_IGNORE_REGEX = r".*([\\/]Testsuite[\\/]|[\\/]mingw64[\\/]|[\\/]usr[\\/]|Xcode|[\\/]llvm[\\/]).*"


def firmware_sources() -> list[Path]:
    return sorted(BEACON_SRC.glob("*.c")) + sorted(BASE_SRC.glob("*.c"))


def _rel(path: str | Path) -> str:
    try:
        return Path(path).resolve().relative_to(REPO).as_posix()
    except ValueError:
        return Path(path).as_posix()


def _has_functions(path: Path) -> bool:
    """True when the C file defines at least one function body."""
    import re

    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8", errors="replace"), flags=re.S)
    return re.search(r"\)\s*\{", text) is not None


def _pct(covered: int, count: int) -> str:
    return "–" if count == 0 else f"{100.0 * covered / count:.1f} %"


C_HEADER = [
    "## Firmware (C, host tests)",
    "",
    "| File | Lines | Branches | Functions |",
    "|---|---|---|---|",
]


def _c_rows(files: list[tuple[str, tuple[int, int], tuple[int, int], tuple[int, int]]],
            totals: tuple[tuple[int, int], tuple[int, int], tuple[int, int]]) -> list[str]:
    """files: (relative path, (lines covered, total), (branches…), (functions…))."""
    lines = list(C_HEADER)
    covered = set()
    for name, ln, br, fn in sorted(files):
        covered.add(name)
        lines.append(f"| `{name}` | {_pct(*ln)} | {_pct(*br)} | {_pct(*fn)} |")
    ln, br, fn = totals
    lines.append(f"| **Total** | **{_pct(*ln)}** | **{_pct(*br)}** | **{_pct(*fn)}** |")
    missing = [p for p in firmware_sources() if _rel(p) not in covered]
    data_only = [p for p in missing if not _has_functions(p)]
    missing = [p for p in missing if p not in data_only]
    if missing:
        lines += ["", "Not compiled into any host test (covered only by hardware tests F–H or not at all):", ""]
        lines += [f"- `{_rel(p)}`" for p in missing]
    if data_only:
        lines += ["", "Data only, no code to measure: " + ", ".join(f"`{_rel(p)}`" for p in data_only)]
    lines += ["", "Line-by-line: `c_html/index.html`"]
    return lines


def c_report(toolchain, executables: list[Path], raw_dir: Path, out_dir: Path) -> list[str]:
    if toolchain.kind == "gcc":
        return c_report_gcov(toolchain, out_dir)
    return c_report_llvm(toolchain, executables, raw_dir, out_dir)


def c_report_gcov(toolchain, out_dir: Path) -> list[str]:
    """gcc --coverage: the .gcda files next to the instrumented executables
    are evaluated by gcov through gcovr."""
    from repo import firmware_unit_build

    build_dir = firmware_unit_build().COVERAGE_BUILD_DIR
    if not list(build_dir.glob("*.gcda")):
        return C_HEADER[:2] + ["No C coverage data (no instrumented C test ran)."]
    (out_dir / "c_html").mkdir(parents=True, exist_ok=True)
    summary = out_dir / "c_summary.json"
    cmd = [
        sys.executable, "-m", "gcovr",
        "--root", str(REPO),
        # gcovr splits this value like a command line: backslashes would be lost
        "--gcov-executable", Path(toolchain.gcov).as_posix(),
        "--filter", "Network_Beacon_nrf54/src/",
        "--filter", "Network_Base_nrf54/src/",
        "--json-summary", str(summary),
        "--html-details", str(out_dir / "c_html" / "index.html"),
        str(build_dir),
    ]
    result = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"gcovr failed:\n{result.stdout[-2000:]}\n{result.stderr[-2000:]}")
    data = json.loads(summary.read_text(encoding="utf-8"))
    files = [
        (Path(f["filename"]).as_posix(),
         (f["line_covered"], f["line_total"]),
         (f["branch_covered"], f["branch_total"]),
         (f["function_covered"], f["function_total"]))
        for f in data["files"]
    ]
    totals = ((data["line_covered"], data["line_total"]),
              (data["branch_covered"], data["branch_total"]),
              (data["function_covered"], data["function_total"]))
    return _c_rows(files, totals)


def c_report_llvm(toolchain, executables: list[Path], raw_dir: Path, out_dir: Path) -> list[str]:
    raws = sorted(raw_dir.glob("*.profraw"))
    if not raws or not executables:
        return C_HEADER[:2] + ["No C coverage data (no instrumented C test ran)."]
    profdata = out_dir / "c_merged.profdata"
    subprocess.run([toolchain.profdata, "merge", "-sparse", *map(str, raws), "-o", str(profdata)],
                   check=True, capture_output=True, text=True)
    objects = [str(executables[0])]
    for exe in executables[1:]:
        objects += ["-object", str(exe)]
    common = [f"-instr-profile={profdata}", f"-ignore-filename-regex={C_IGNORE_REGEX}"]

    subprocess.run([toolchain.cov, "show", *objects, *common, "-format=html", "-show-branches=count",
                    f"-output-dir={out_dir / 'c_html'}"], check=True, capture_output=True, text=True)
    exported = subprocess.run([toolchain.cov, "export", *objects, *common, "-summary-only"],
                              check=True, capture_output=True, text=True)
    data = json.loads(exported.stdout)["data"][0]

    def triple(summary):
        return tuple((summary[k]["covered"], summary[k]["count"]) for k in ("lines", "branches", "functions"))

    files = [(_rel(f["filename"]), *triple(f["summary"])) for f in data["files"]]
    return _c_rows(files, triple(data["totals"]))


def python_report(cov, out_dir: Path) -> list[str]:
    cov.html_report(directory=str(out_dir / "python_html"))
    buffer = io.StringIO()
    total = cov.report(file=buffer, show_missing=False)
    data = cov.get_data()
    lines = [
        "## Python",
        "",
        "| File | Statements | Branches | Coverage |",
        "|---|---|---|---|",
    ]
    for filename in sorted(data.measured_files()):
        analysis = cov._analyze(filename)  # noqa: SLF001 - public API has no per-file numbers
        numbers = analysis.numbers
        lines.append(
            f"| `{_rel(filename)}` | {numbers.n_executed}/{numbers.n_statements} "
            f"| {numbers.n_executed_branches}/{numbers.n_branches} | {numbers.pc_covered:.1f} % |"
        )
    lines += [f"| **Total** | | | **{total:.1f} %** |", "", "Line-by-line: `python_html/index.html`"]
    return lines


def write_summary(out_dir: Path, sections: list[list[str]]) -> Path:
    text = [
        "# Coverage",
        "",
        "Used to find untested branches, not as a target number (see",
        "`1_Specification/Release_Gate.md`).",
        "",
    ]
    for section in sections:
        text += section + [""]
    path = out_dir / "coverage.md"
    path.write_text("\n".join(text), encoding="utf-8")
    return path
