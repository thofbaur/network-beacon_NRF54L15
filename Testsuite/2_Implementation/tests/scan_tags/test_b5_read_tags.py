"""B5 - Scan_New_Tags/read_tags.py parses the scanner firmware's UART
lines, de-duplicates addresses and writes the CSV. The serial port is
replaced by a fake. See 1_Specification/B_logger.md.
"""

from __future__ import annotations

import csv

import pytest

from repo import SCAN_TAGS, load_module


@pytest.fixture
def read_tags(tmp_path, monkeypatch):
    module = load_module(SCAN_TAGS / "read_tags.py", "dsa_test_read_tags")
    monkeypatch.setattr(module, "SCRIPT_DIR", str(tmp_path))
    return module


class FakeSerial:
    lines: list[bytes] = []

    def __init__(self, port, baud, timeout=None):
        self.port = port
        self._lines = list(FakeSerial.lines)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def readline(self):
        if self._lines:
            return self._lines.pop(0)
        raise KeyboardInterrupt


def run(read_tags, monkeypatch, tmp_path, lines):
    FakeSerial.lines = lines
    monkeypatch.setattr(read_tags.serial, "Serial", FakeSerial)
    monkeypatch.setattr(read_tags.sys, "argv", ["read_tags.py", "-p", "COMX"])
    with pytest.raises(KeyboardInterrupt):
        read_tags.main()
    files = list(tmp_path.glob("tag_adresses_*.csv"))
    assert len(files) == 1
    with open(files[0], newline="") as f:
        return list(csv.reader(f))


def test_b5_line_regex(read_tags):
    m = read_tags.LINE_RE.match("Device,Nordic_LBS,D7:D0:FF:94:FF:04 (random)")
    assert m and m.group("name") == "Nordic_LBS"
    assert m.group("addr") == "D7:D0:FF:94:FF:04"
    assert read_tags.LINE_RE.match("Device,Nordic_LBS,D7:D0:FF") is None


def test_b5_new_devices_written_once(read_tags, monkeypatch, tmp_path):
    rows = run(
        read_tags,
        monkeypatch,
        tmp_path,
        [
            b"Device,Nordic_LBS,D7:D0:FF:94:FF:04 (random)\r\n",
            b"Device,Nordic_LBS,D7:D0:FF:94:FF:04 (random)\r\n",
            b"Device,Nordic_LBS,E2:1A:BA:00:D5:EB (random)\r\n",
            b"",
        ],
    )
    assert rows[0] == read_tags.CSV_HEADER
    assert rows[1:] == [
        ["Nordic_LBS", "D7", "D0", "FF", "94", "FF", "04"],
        ["Nordic_LBS", "E2", "1A", "BA", "00", "D5", "EB"],
    ]


def test_b5_other_lines_ignored(read_tags, monkeypatch, tmp_path, capsys):
    rows = run(
        read_tags,
        monkeypatch,
        tmp_path,
        [b"\x00garbage\r\n", b"Device,broken\r\n", b"Device,Nordic_LBS,C1:00:00:00:00:01\r\n"],
    )
    assert len(rows) == 2
    assert "Unrecognized line: Device,broken" in capsys.readouterr().out
