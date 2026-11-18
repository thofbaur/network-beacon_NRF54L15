"""B1 - dsa_logger.py decodes every NUS payload type, including the edge
values of its 16-, 24- and 32-bit fields. See 1_Specification/B_logger.md.
"""

from __future__ import annotations

import pytest

import frames
from findings import finding
from repo import dsa_logger


@pytest.fixture(scope="module")
def log():
    return dsa_logger()


def decode(log, notification: bytes) -> tuple[str, ...]:
    flag, payload = notification[0], notification[1:]
    return log.parse_payload(flag, payload, b"ID\x01" + notification)


EDGE_16 = [0, 1, 0xFFFE, 0xFFFF]
EDGE_24 = [0, 1, 0xFFFFFE, 0xFFFFFF]
EDGE_32 = [0, 1, 0xFFFFFFFE, 0xFFFFFFFF]


@pytest.mark.parametrize("value", EDGE_16)
def test_b1_uint16_decode(log, value):
    assert log.uint16_be_decode(value.to_bytes(2, "big")) == value


@pytest.mark.parametrize("value", EDGE_24)
def test_b1_uint24_decode(log, value):
    assert log.uint24_be_decode(value.to_bytes(3, "big")) == value


@pytest.mark.parametrize("value", EDGE_32)
def test_b1_uint32_decode(log, value):
    assert log.uint32_be_decode(value.to_bytes(4, "big")) == value


@pytest.mark.parametrize("uptime", EDGE_32)
def test_b1_time(log, uptime):
    assert decode(log, frames.time(uptime)) == (f"Current Time: {uptime}",)


@pytest.mark.parametrize("uptime", EDGE_32)
@pytest.mark.parametrize("count", EDGE_24)
@pytest.mark.parametrize("mv", EDGE_16)
def test_b1_time_contacts_voltage(log, uptime, count, mv):
    assert decode(log, frames.time_contacts_voltage(uptime, count, mv)) == (
        f"Current Timer: {uptime}",
        f"Contact Count: {count}",
        f"Voltage: {mv}",
    )


@pytest.mark.parametrize("timer", EDGE_24)
@pytest.mark.parametrize("rssi", [0, 1, 127, 128, 255])
def test_b1_data_contact(log, timer, rssi):
    assert decode(log, frames.data([(42, timer, rssi)])) == (f"ID2: 42, Timer: {timer}, RSSI: -{rssi}",)


def test_b1_data_multiple_contacts_in_order(log):
    contacts = [(i, i * 1000, 40 + i) for i in range(48)]
    lines = decode(log, frames.data(contacts))
    assert len(lines) == 48
    assert lines[0] == "ID2: 0, Timer: 0, RSSI: -40"
    assert lines[47] == "ID2: 47, Timer: 47000, RSSI: -87"


def test_b1_data_empty_batch(log):
    assert decode(log, frames.data([])) == ()


@pytest.mark.parametrize("t", EDGE_24)
def test_b1_self_report(log, t):
    assert decode(log, frames.self_reports([t])) == (f"Self-report time: {t}",)


@pytest.mark.parametrize("enter, leave", [(0, 0), (1, 2), (0xFFFFFE, 0xFFFFFF), (0xFFFFFF, 0)])
def test_b1_eco_session(log, enter, leave):
    assert decode(log, frames.eco_log([(enter, leave)])) == (f"Eco Session Enter: {enter}, Leave: {leave}",)


@pytest.mark.parametrize("status", [0, 1, 0x7F, 0xFF])
def test_b1_connect_status(log, status):
    assert decode(log, frames.connect_status(status)) == (f"Status: {status}",)


def test_b1_finished_control(log):
    assert decode(log, frames.finished()) == ("Transfer complete. Disconnecting",)


def test_b1_other_control_payload_is_ignored(log):
    assert decode(log, bytes([frames.FLAG_CONTROL]) + b"unknown!") == ()


@pytest.mark.parametrize(
    "notification",
    [
        bytes([frames.FLAG_TIME, 1, 2, 3]),  # 3 instead of 4 bytes
        bytes([frames.FLAG_TIME_CONTACTS_VOLTAGE]) + bytes(8),
        bytes([frames.FLAG_DATA, 2]) + bytes(5),  # count 2, one record
        bytes([frames.FLAG_SELF_REPORT, 1, 0, 0]),
        bytes([frames.FLAG_ECO_LOG, 1]) + bytes(5),
        bytes([frames.FLAG_CONNECT_STATUS]),
        bytes([frames.FLAG_DATA]),
    ],
    ids=["time-short", "tcv-short", "data-count", "self-short", "eco-short", "status-empty", "data-no-count"],
)
def test_b1_wrong_length_is_rejected(log, notification):
    assert decode(log, notification) == ()


@finding(5)
def test_b1_voltage_package_is_decoded(log):
    """DSA_NUS_FLAG_VOLTAGE is part of the protocol and decoded by the Base;
    the logger must not silently drop it."""
    assert decode(log, frames.voltage(2950)) == ("Voltage: 2950",)
