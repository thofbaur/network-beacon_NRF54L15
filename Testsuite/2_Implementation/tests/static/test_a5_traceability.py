"""A5 - every design decision in DECISIONS.md is covered by at least one
test, and every test ID referenced exists in the specifications.
See 1_Specification/A_static.md.
"""

from __future__ import annotations

import re

import pytest

from repo import DECISIONS, SPECS, read_text

TRACEABILITY = SPECS / "TRACEABILITY.md"
TEST_ID = re.compile(r"\b([A-I]\d{1,2})\b")


def decisions() -> list[str]:
    return [m.group(1).strip() for m in re.finditer(r"^##\s+(.+)$", read_text(DECISIONS), re.M)]


def trace_rows() -> dict[str, set[str]]:
    rows: dict[str, set[str]] = {}
    for line in read_text(TRACEABILITY).splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) >= 2 and cells[0] and not set(cells[0]) <= set("-: "):
            rows[cells[0]] = set(TEST_ID.findall(cells[1]))
    return rows


def specified_ids() -> set[str]:
    ids = set()
    for spec in SPECS.glob("*.md"):
        ids |= set(re.findall(r"^###\s+([A-I]\d{1,2})\b", read_text(spec), re.M))
    return ids


@pytest.mark.parametrize("decision", decisions())
def test_a5_decision_is_traced_to_tests(decision):
    rows = trace_rows()
    assert decision in rows, f"decision '{decision}' missing from TRACEABILITY.md"
    assert rows[decision], f"decision '{decision}' has no test IDs"


def test_a5_traced_test_ids_are_specified():
    known = specified_ids()
    unknown = sorted({tid for ids in trace_rows().values() for tid in ids} - known)
    assert not unknown, f"TRACEABILITY.md references unspecified tests: {unknown}"


def test_a5_no_stale_decisions_in_traceability():
    stale = sorted(set(trace_rows()) - set(decisions()) - {"Decision"})
    assert not stale, f"TRACEABILITY.md lists decisions not in DECISIONS.md: {stale}"
