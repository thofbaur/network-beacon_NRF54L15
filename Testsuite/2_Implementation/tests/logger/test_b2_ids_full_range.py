"""B2 - every possible beacon ID byte (0-255) is written as its decimal
value, for ID1 (frame header) and ID2 (contact record), and no two IDs ever
produce the same text. Regression for commit 2e1f52d (IDs 48-57 were
written as the ASCII digits 0-9). See 1_Specification/B_logger.md.
"""

from __future__ import annotations

import pytest

import frames
from repo import dsa_logger


@pytest.fixture(scope="module")
def log():
    return dsa_logger()


def feed_all(log, data: bytes):
    return list(log.MessageParser().feed(data))


@pytest.mark.parametrize("beacon_id", range(256))
def test_b2_id1_rendered_as_decimal(log, beacon_id):
    msgs = feed_all(log, frames.raw_frame(beacon_id, frames.connect_status(7)))
    assert len(msgs) == 1
    assert msgs[0].beacon_id == str(beacon_id)


@pytest.mark.parametrize("id2", range(256))
def test_b2_id2_rendered_as_decimal(log, id2):
    msgs = feed_all(log, frames.raw_frame(1, frames.data([(id2, 5, 50)])))
    assert msgs[0].output_lines == (f"ID2: {id2}, Timer: 5, RSSI: -50",)


def test_b2_ids_never_collide(log):
    rendered = {log.format_beacon_id(b) for b in range(256)}
    assert len(rendered) == 256


def test_b2_all_ids_in_one_stream(log):
    stream = b"".join(frames.raw_frame(i, frames.connect_status(i)) for i in range(256))
    msgs = feed_all(log, stream)
    assert [m.beacon_id for m in msgs] == [str(i) for i in range(256)]


def test_regression_2e1f52d_ascii_digit_ids(log):
    """IDs 48-57 are the ASCII codes of '0'-'9'. Before 2e1f52d they were
    logged as 0-9 and became indistinguishable from the real beacons 0-9."""
    for beacon_id in range(48, 58):
        msgs = feed_all(log, frames.raw_frame(beacon_id, frames.connect_status(0)))
        assert msgs[0].beacon_id == str(beacon_id)
        assert msgs[0].beacon_id != chr(beacon_id)
