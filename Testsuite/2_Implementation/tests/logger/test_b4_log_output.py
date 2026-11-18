"""B4 - dsa_logger.LogOutput writes the line format that downstream
processing parses: '<YYYY-mm-dd HH:MM:SS>,ID: <id>,<decoded line>'.
See 1_Specification/B_logger.md.
"""

from __future__ import annotations

import re
from datetime import datetime as real_datetime

import pytest

import frames
from repo import dsa_logger

LINE_RE = re.compile(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},ID: (\d{1,3}),(.+)$")


@pytest.fixture(scope="module")
def log():
    return dsa_logger()


def write_transfer(log, out):
    parser = log.MessageParser()
    for message in parser.feed(
        frames.raw_frame(48, frames.time_contacts_voltage(10, 1, 3000))
        + frames.raw_frame(48, frames.data([(5, 9, 70)]))
        + frames.raw_frame(48, frames.finished())
    ):
        out.write_message(message)


def test_b4_file_name_pattern(log, tmp_path):
    out = log.LogOutput(tmp_path)
    assert re.fullmatch(r"dsa_\d{8}_\d{4}\.log", out.path.name)
    assert out.path.parent == tmp_path


def test_b4_line_format(log, tmp_path):
    out = log.LogOutput(tmp_path)
    out.open()
    write_transfer(log, out)
    out.close()
    text = out.path.read_text(encoding="utf-8")
    rows = text.splitlines()
    assert len(rows) == 5
    for row in rows:
        m = LINE_RE.match(row)
        assert m, f"unexpected log line format: {row!r}"
        assert m.group(1) == "48"
    assert rows[0].endswith(",ID: 48,Current Timer: 10")
    assert rows[3].endswith(",ID: 48,ID2: 5, Timer: 9, RSSI: -70")
    assert rows[4].endswith(",ID: 48,Transfer complete. Disconnecting")
    assert "\r" not in text, "log lines must use LF line endings on every OS"


def test_b4_appends_to_existing_file(log, tmp_path):
    first = log.LogOutput(tmp_path)
    first.open()
    write_transfer(log, first)
    first.close()
    second = log.LogOutput(tmp_path)
    second.path = first.path
    second.open()
    write_transfer(log, second)
    second.close()
    assert len(first.path.read_text(encoding="utf-8").splitlines()) == 10


def test_b4_lines_are_flushed_per_message(log, tmp_path):
    out = log.LogOutput(tmp_path)
    out.open()
    write_transfer(log, out)
    # Not closed yet: content must already be on disk (crash safety).
    assert len(out.path.read_text(encoding="utf-8").splitlines()) == 5
    out.close()


def test_b4_write_without_open_raises(log, tmp_path):
    out = log.LogOutput(tmp_path)
    msg = next(iter(log.MessageParser().feed(frames.raw_frame(1, frames.connect_status(0)))))
    with pytest.raises(RuntimeError):
        out.write_message(msg)


def test_b4_timestamp_is_wall_clock(log, tmp_path, monkeypatch):
    class FixedDatetime(real_datetime):
        @classmethod
        def now(cls, tz=None):
            return cls(2026, 8, 15, 22, 0, 1)

    monkeypatch.setattr(log, "datetime", FixedDatetime)
    out = log.LogOutput(tmp_path)
    out.open()
    write_transfer(log, out)
    out.close()
    assert out.path.name == "dsa_20260815_2200.log"
    assert out.path.read_text(encoding="utf-8").startswith("2026-08-15 22:00:01,ID: 48,")
