"""A3 - Scan_New_Tags/generate_ids.py produces valid known_device_table
entries, and the documented workflow (read_tags.py -> generate_ids.py) works.

See 1_Specification/A_static.md.
"""

from __future__ import annotations

import csv
import re

import pytest

from cparse import parse_known_device_table
from findings import finding
from repo import SCAN_TAGS, load_module, read_text

HEADER = ["name", "addr1", "addr2", "addr3", "addr4", "addr5", "addr6", "physical_id"]


@pytest.fixture
def gen(tmp_path, monkeypatch):
    module = load_module(SCAN_TAGS / "generate_ids.py", "dsa_test_generate_ids")
    monkeypatch.setattr(module, "SCRIPT_DIR", str(tmp_path))
    return module


def write_csv(path, rows, header=HEADER):
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(header)
        writer.writerows(rows)


def test_a3_address_bytes_are_reversed(gen, tmp_path):
    write_csv(tmp_path / "tag_adresses_1.csv", [["Nordic_LBS", "F7", "E9", "EF", "AF", "57", "97", "1"]])
    gen.main()
    devices = parse_known_device_table(tmp_path / "ids.c")
    assert len(devices) == 1
    assert devices[0].addr == (0x97, 0x57, 0xAF, 0xEF, 0xE9, 0xF7)
    assert devices[0].addr_type == "BT_ADDR_LE_RANDOM"


def test_a3_ids_come_from_physical_id(gen, tmp_path):
    write_csv(
        tmp_path / "tag_adresses_1.csv",
        [
            ["Nordic_LBS", "C1", "00", "00", "00", "00", "01", "17"],
            ["Nordic_LBS", "C1", "00", "00", "00", "00", "02", "5"],
        ],
    )
    gen.main()
    assert [d.device_id for d in parse_known_device_table(tmp_path / "ids.c")] == [17, 5]


def test_a3_rows_from_several_sessions_are_combined(gen, tmp_path):
    write_csv(tmp_path / "tag_adresses_1.csv", [["a", "C1", "0", "0", "0", "0", "1", "1"]])
    write_csv(tmp_path / "tag_adresses_2.csv", [["a", "C1", "0", "0", "0", "0", "2", "2"]])
    gen.main()
    assert len(parse_known_device_table(tmp_path / "ids.c")) == 2


def test_a3_duplicates_are_reported(gen, tmp_path, capsys):
    write_csv(
        tmp_path / "tag_adresses_1.csv",
        [
            ["a", "C1", "00", "00", "00", "00", "01", "7"],
            ["b", "C1", "00", "00", "00", "00", "01", "7"],
        ],
    )
    gen.main()
    out = capsys.readouterr().out
    assert "Duplicate physical_id" in out
    assert "Duplicate addresses" in out


def test_a3_output_is_valid_c_table_syntax(gen, tmp_path):
    write_csv(tmp_path / "tag_adresses_1.csv", [["a", "c1", "aa", "bb", "cc", "dd", "ee", "3"]])
    gen.main()
    text = read_text(tmp_path / "ids.c")
    assert re.search(r"\.val = \{ 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0xC1 \}", text), text


@finding(8)
def test_a3_read_tags_csv_is_accepted_by_generate_ids(gen, tmp_path):
    """The CSV written by read_tags.py must be usable by generate_ids.py."""
    read_tags = load_module(SCAN_TAGS / "read_tags.py", "dsa_test_read_tags")
    write_csv(
        tmp_path / "tag_adresses_1.csv",
        [["Nordic_LBS", "F7", "E9", "EF", "AF", "57", "97"]],
        header=read_tags.CSV_HEADER,
    )
    try:
        gen.main()
    except KeyError as exc:
        pytest.fail(
            f"generate_ids.py needs column {exc} but read_tags.py writes only "
            f"{read_tags.CSV_HEADER}: the documented workflow requires a manual "
            "step that is not described in HowTo.md"
        )


@finding(8)
def test_a3_howto_matches_generate_ids_cli():
    howto = read_text(SCAN_TAGS / "HowTo.md")
    script = read_text(SCAN_TAGS / "generate_ids.py")
    problems = []
    if "--start-id" in howto and "--start-id" not in script:
        problems.append("HowTo.md documents '--start-id', generate_ids.py has no such option")
    if re.search(r"assigned\s+sequentially", howto) and "physical_id" in script:
        problems.append("HowTo.md says IDs are assigned sequentially, the script uses physical_id")
    assert not problems, "; ".join(problems)
