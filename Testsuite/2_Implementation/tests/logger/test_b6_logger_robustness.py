"""B6 - the logger's capture loop behaves sensibly when the Base reboots
mid-frame, the capture runs across midnight, the serial port disappears or
the disk fails. See 1_Specification/B_logger.md.
"""

from __future__ import annotations

from datetime import datetime as real_datetime

import pytest

import frames
from findings import finding
from repo import dsa_logger


@pytest.fixture(scope="module")
def log():
    return dsa_logger()


class ScriptedInput:
    """InputSource fake: returns chunks, then raises the given exception."""

    def __init__(self, log, chunks, end=KeyboardInterrupt):
        self.chunks = list(chunks)
        self.end = end
        self.opened = False
        self.closed = False

    def open(self):
        self.opened = True

    def read(self):
        if self.chunks:
            return self.chunks.pop(0)
        raise self.end()

    def close(self):
        self.closed = True


def run(log, tmp_path, chunks, end=KeyboardInterrupt, monkeypatch=None):
    source = ScriptedInput(log, chunks, end)
    out = log.LogOutput(tmp_path)
    if monkeypatch:
        monkeypatch.setattr(log.time, "sleep", lambda s: None)
    log.run_logger(source, log.MessageParser(), out)
    return source, out


def test_b6_base_reboot_mid_frame_resyncs(log, tmp_path, monkeypatch):
    good = frames.raw_frame(4, frames.data([(1, 2, 3)]))
    chunks = [good[:7], b"\x00\x00*** Booting nRF Connect SDK ***\r\n", good, good]
    source, out = run(log, tmp_path, chunks, monkeypatch=monkeypatch)
    rows = out.path.read_text(encoding="utf-8").splitlines()
    assert len(rows) == 2
    assert all(r.endswith(",ID: 4,ID2: 1, Timer: 2, RSSI: -3") for r in rows)
    assert source.closed


def test_b6_idle_reads_do_not_end_capture(log, tmp_path, monkeypatch):
    good = frames.raw_frame(4, frames.connect_status(0))
    source, out = run(log, tmp_path, [b"", b"", good, b"", good], monkeypatch=monkeypatch)
    assert len(out.path.read_text(encoding="utf-8").splitlines()) == 2


def test_b6_capture_across_midnight_keeps_correct_dates(log, tmp_path, monkeypatch):
    times = iter(
        [
            real_datetime(2026, 8, 15, 23, 59, 59),  # file name
            real_datetime(2026, 8, 15, 23, 59, 59),  # first message
            real_datetime(2026, 8, 16, 0, 0, 1),  # second message
        ]
    )

    class SteppingDatetime(real_datetime):
        @classmethod
        def now(cls, tz=None):
            return next(times)

    monkeypatch.setattr(log, "datetime", SteppingDatetime)
    good = frames.raw_frame(4, frames.connect_status(0))
    _, out = run(log, tmp_path, [good, good], monkeypatch=monkeypatch)
    rows = out.path.read_text(encoding="utf-8").splitlines()
    assert rows[0].startswith("2026-08-15 23:59:59,")
    assert rows[1].startswith("2026-08-16 00:00:01,")


def test_b6_lost_port_keeps_already_written_lines(log, tmp_path, monkeypatch):
    good = frames.raw_frame(4, frames.connect_status(0))
    source = ScriptedInput(log, [good, good], end=OSError)
    out = log.LogOutput(tmp_path)
    monkeypatch.setattr(log.time, "sleep", lambda s: None)
    with pytest.raises(OSError):
        log.run_logger(source, log.MessageParser(), out)
    assert len(out.path.read_text(encoding="utf-8").splitlines()) == 2
    assert source.closed


@finding(7)
def test_b6_lost_port_is_reported_as_error(log, tmp_path, monkeypatch):
    """A vanished COM port (USB unplugged, Base reset) must end with an error
    message and exit code 1, not an unhandled traceback."""
    good = frames.raw_frame(4, frames.connect_status(0))
    monkeypatch.chdir(tmp_path)
    monkeypatch.setattr(log, "build_input_source", lambda args: ScriptedInput(log, [good], end=OSError))
    monkeypatch.setattr(log.time, "sleep", lambda s: None)
    try:
        code = log.main([])
    except OSError as exc:
        pytest.fail(f"main() crashed with an unhandled {type(exc).__name__} instead of returning 1")
    assert code == 1


def test_b6_disk_write_failure_is_not_silent(log, tmp_path, monkeypatch):
    good = frames.raw_frame(4, frames.connect_status(0))
    source = ScriptedInput(log, [good])
    out = log.LogOutput(tmp_path)
    monkeypatch.setattr(log.time, "sleep", lambda s: None)

    def failing_write(message):
        raise OSError(28, "No space left on device")

    out.write_message = failing_write
    with pytest.raises(OSError):
        log.run_logger(source, log.MessageParser(), out)
    assert source.closed
