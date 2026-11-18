"""B3 - dsa_logger.MessageParser reassembles frames from an arbitrarily
split UART byte stream and resynchronises after garbage, bad terminators,
unknown flags and implausible counts. See 1_Specification/B_logger.md.
"""

from __future__ import annotations

import pytest

import frames
from findings import finding
from repo import dsa_logger


@pytest.fixture(scope="module")
def log():
    return dsa_logger()


def transfer(beacon_id: int = 7) -> bytes:
    """One complete transfer as the Base forwards it."""
    parts = [
        frames.connect_status(0x00),
        frames.time_contacts_voltage(123456, 3, 2950),
        frames.data([(1, 100, 50), (2, 101, 60), (0x49, 0x444944, 0x49)]),  # 'I','D' bytes inside
        frames.self_reports([5000, 6000]),
        frames.eco_log([(10, 20)]),
        frames.finished(),
    ]
    return b"".join(frames.raw_frame(beacon_id, p) for p in parts)


def lines(messages) -> list[tuple[str, str]]:
    return [(m.beacon_id, line) for m in messages for line in m.output_lines]


@pytest.fixture(scope="module")
def expected(log):
    out = lines(log.MessageParser().feed(transfer()))
    assert len(out) == 1 + 3 + 3 + 2 + 1 + 1
    return out


def test_b3_whole_stream(expected):
    assert expected[0] == ("7", "Status: 0")
    assert ("7", "ID2: 73, Timer: 4475204, RSSI: -73") in expected
    assert expected[-1] == ("7", "Transfer complete. Disconnecting")


def test_b3_every_split_point(log, expected):
    data = transfer()
    for split in range(1, len(data)):
        parser = log.MessageParser()
        got = lines(parser.feed(data[:split])) + lines(parser.feed(data[split:]))
        assert got == expected, f"split at byte {split}"


def test_b3_byte_by_byte(log, expected):
    parser = log.MessageParser()
    got = []
    for byte in transfer():
        got += lines(parser.feed(bytes([byte])))
    assert got == expected


def test_b3_garbage_between_frames(log, expected):
    data = b"\x00\xffboot banner\r\n" + transfer() + b"network_base: reconnect\r\n" + transfer()
    assert lines(log.MessageParser().feed(data)) == expected + expected


def test_b3_text_containing_id_does_not_lose_following_frames(log, expected):
    data = b"VALID config\r\nIDLE\r\n" + transfer()
    assert lines(log.MessageParser().feed(data))[-len(expected):] == expected


def test_b3_bad_terminator_drops_only_that_frame(log):
    good = frames.raw_frame(3, frames.connect_status(1))
    bad = good[:-2] + b"XX"
    got = lines(log.MessageParser().feed(bad + good))
    assert got == [("3", "Status: 1")]


def test_b3_unknown_flag_is_skipped(log):
    unknown = b"ID\x05\x42\x01\x02\r\n"
    good = frames.raw_frame(3, frames.connect_status(1))
    assert lines(log.MessageParser().feed(unknown + good)) == [("3", "Status: 1")]


def test_b3_implausible_count_resyncs_immediately(log):
    # 60 contacts would need 301 bytes > one NUS notification (247).
    bogus = b"ID\x05" + bytes([frames.FLAG_DATA, 60]) + b"\x00" * 10
    good = frames.raw_frame(3, frames.connect_status(1))
    assert lines(log.MessageParser().feed(bogus + good)) == [("3", "Status: 1")]


def test_b3_maximum_data_frame(log):
    contacts = [(i, i, 50) for i in range(48)]  # 1 + 1 + 240 = 242 bytes
    got = lines(log.MessageParser().feed(frames.raw_frame(9, frames.data(contacts))))
    assert len(got) == 48


def test_b3_flush_discards_partial_frame(log):
    parser = log.MessageParser()
    data = frames.raw_frame(3, frames.connect_status(1))
    assert lines(parser.feed(data[:4])) == []
    parser.flush()
    assert lines(parser.feed(data[4:])) == []


@finding(6)
def test_b3_exclude_option_omits_matching_lines(log):
    """--exclude is documented as 'substring to omit from the log'."""
    parser = log.MessageParser(exclude_substrings=["Voltage"])
    got = lines(parser.feed(frames.raw_frame(3, frames.time_contacts_voltage(1, 2, 3000))))
    assert not any("Voltage" in line for _, line in got), (
        "--exclude/--omit-* only affect discarded garbage text, never decoded "
        f"output lines: {got}"
    )
