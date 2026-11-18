"""Builders for DSA NUS notifications and the Base's raw UART frames.

A NUS notification sent by the beacon is   <flag><payload...>
The Base (CONFIG_DSA_OUTPUT_FORMAT_RAW) forwards it over UART as
    b"ID" + <beacon id byte> + <notification> + b"\\r\\n"
which is what dsa_logger.py parses.
"""

from __future__ import annotations

FLAG_TIME = 0x01
FLAG_DATA = 0x02
FLAG_VOLTAGE = 0x03
FLAG_CONTROL = 0x04
FLAG_TIME_CONTACTS_VOLTAGE = 0x05
FLAG_SELF_REPORT = 0x06
FLAG_ECO_LOG = 0x07
FLAG_CONNECT_STATUS = 0x08
ACK_MAGIC = 0xAC


def be(value: int, size: int) -> bytes:
    return value.to_bytes(size, "big")


def time(uptime: int) -> bytes:
    return bytes([FLAG_TIME]) + be(uptime, 4)


def voltage(mv: int) -> bytes:
    return bytes([FLAG_VOLTAGE]) + be(mv, 2)


def time_contacts_voltage(uptime: int, contacts: int, mv: int) -> bytes:
    return bytes([FLAG_TIME_CONTACTS_VOLTAGE]) + be(uptime, 4) + be(contacts, 3) + be(mv, 2)


def data(contacts: list[tuple[int, int, int]]) -> bytes:
    """contacts: (id2, timer, rssi_magnitude)."""
    body = b"".join(bytes([i]) + be(t, 3) + bytes([r]) for i, t, r in contacts)
    return bytes([FLAG_DATA, len(contacts)]) + body


def self_reports(times: list[int]) -> bytes:
    return bytes([FLAG_SELF_REPORT, len(times)]) + b"".join(be(t, 3) for t in times)


def eco_log(sessions: list[tuple[int, int]]) -> bytes:
    body = b"".join(be(a, 3) + be(b, 3) for a, b in sessions)
    return bytes([FLAG_ECO_LOG, len(sessions)]) + body


def finished() -> bytes:
    return bytes([FLAG_CONTROL]) + b"finished"


def connect_status(status: int) -> bytes:
    return bytes([FLAG_CONNECT_STATUS, status])


def raw_frame(beacon_id: int, notification: bytes) -> bytes:
    return b"ID" + bytes([beacon_id]) + notification + b"\r\n"


def ack(domain: int, count: int) -> bytes:
    return bytes([ACK_MAGIC, domain, count])
