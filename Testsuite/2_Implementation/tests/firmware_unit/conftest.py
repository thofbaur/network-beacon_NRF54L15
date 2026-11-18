"""Collect every C test case of 2_Implementation/firmware_unit/ as its own pytest item.

The C executables are (re)built at collection time by
2_Implementation/firmware_unit/build.py (incremental; the first build with
zig takes about a minute). Each case runs in a fresh process:
`<exe> --run <case>`. With --coverage an instrumented copy is built with
clang and every process writes a raw LLVM profile.
"""

from __future__ import annotations

import pytest

from findings import C_CASES, finding
from repo import firmware_unit_build

_cases: list[tuple[str, str, str]] | None = None
_build_error: str | None = None


def collect_cases(config: pytest.Config) -> list[tuple[str, str, str]]:
    global _cases, _build_error
    if _cases is not None:
        return _cases
    build = firmware_unit_build()
    coverage = getattr(config, "_dsa_cov_dir", None) is not None
    try:
        built = build.build_all(coverage=coverage)
    except build.BuildError as exc:
        _build_error = str(exc)
        _cases = []
        return _cases
    _cases = []
    for name in sorted(built):
        if name.startswith("d1_"):
            continue
        if coverage:
            config._dsa_cov_executables.append(built[name])
        for case in build.list_cases(built[name]):
            _cases.append((name, case, str(built[name])))
    return _cases


def pytest_generate_tests(metafunc: pytest.Metafunc) -> None:
    if "c_case" not in metafunc.fixturenames:
        return
    cases = collect_cases(metafunc.config)
    if _build_error is not None:
        metafunc.parametrize("c_case", [("BUILD", _build_error, "")], ids=["firmware-unit-build"])
        return
    params = []
    for t, c, exe in cases:
        case_id = f"{t}::{c}"
        marks = [finding(C_CASES[case_id])] if case_id in C_CASES else []
        params.append(pytest.param((t, c, exe), id=case_id, marks=marks))
    metafunc.parametrize("c_case", params)


@pytest.fixture
def c_env(request) -> dict[str, str] | None:
    """Process environment for a C test: raw profile path when measuring coverage."""
    import os

    cov_dir = getattr(request.config, "_dsa_cov_dir", None)
    if cov_dir is None:
        return None
    env = dict(os.environ)
    # %p = process id, %m = binary signature: one file per test process
    env["LLVM_PROFILE_FILE"] = str(cov_dir / "c_raw" / "%p-%m.profraw")
    return env
