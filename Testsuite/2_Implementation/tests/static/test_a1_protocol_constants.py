"""A1 - the NUS protocol constants agree across all implementations.

Sources compared: shared/common_include.h (beacon + Base), the Base parser
(Network_Base_nrf54/src/message_parser.c), the beacon's entry sizes and
dsa_logger.py. See 1_Specification/A_static.md.
"""

from __future__ import annotations

import re

import pytest

from cparse import parse_defines
from repo import BASE, BASE_SRC, BEACON_SRC, COMMON_INCLUDE, dsa_logger, read_text

FLAG_NAMES = [
    "DSA_NUS_FLAG_TIME",
    "DSA_NUS_FLAG_DATA",
    "DSA_NUS_FLAG_VOLTAGE",
    "DSA_NUS_FLAG_CONTROL",
    "DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE",
    "DSA_NUS_FLAG_SELF_REPORT",
    "DSA_NUS_FLAG_ECO_LOG",
    "DSA_NUS_FLAG_CONNECT_STATUS",
]


@pytest.fixture(scope="module")
def header() -> dict[str, int]:
    return parse_defines(COMMON_INCLUDE)


@pytest.fixture(scope="module")
def base_parser() -> dict[str, int]:
    return parse_defines(BASE_SRC / "message_parser.c")


@pytest.mark.parametrize("name", FLAG_NAMES)
def test_a1_flag_values_match_logger(header, name):
    logger = dsa_logger()
    assert name in header, f"{name} missing from common_include.h"
    assert getattr(logger, name) == header[name]
    assert logger.DSA_NUS_FLAGS[header[name]] == name


def test_a1_logger_knows_no_extra_flags(header):
    logger = dsa_logger()
    assert set(logger.DSA_NUS_FLAGS) == {header[n] for n in FLAG_NAMES}


def test_a1_flags_are_unique(header):
    values = [header[n] for n in FLAG_NAMES]
    assert len(values) == len(set(values))


def test_a1_ack_magic_cannot_collide(header):
    magic = header["DSA_NUS_ACK_MAGIC"]
    assert magic == 0xAC
    assert magic not in {header[n] for n in FLAG_NAMES}
    assert magic not in (ord("s"), ord("t")), "ack must not look like the 'st' start command"


@pytest.mark.parametrize(
    "flag, base_define",
    [
        ("DSA_NUS_FLAG_TIME", "DSA_TIME_LEN"),
        ("DSA_NUS_FLAG_VOLTAGE", "DSA_VOLTAGE_LEN"),
        ("DSA_NUS_FLAG_CONTROL", "DSA_CONTROL_LEN"),
        ("DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE", "DSA_TIME_CONTACT_VOLTAGE_LEN"),
    ],
)
def test_a1_fixed_payload_lengths_match_base(header, base_parser, flag, base_define):
    logger = dsa_logger()
    assert logger.DSA_NUS_PAYLOAD_LENGTHS[header[flag]] == base_parser[base_define]


def test_a1_connect_status_payload_is_one_byte(header):
    assert dsa_logger().DSA_NUS_PAYLOAD_LENGTHS[header["DSA_NUS_FLAG_CONNECT_STATUS"]] == 1


@pytest.mark.parametrize(
    "logger_name, base_define, beacon_header, beacon_define",
    [
        ("DSA_DATA_SET_LEN", "DSA_DATA_SET_LEN", "network.h", "NETWORK_CONTACT_ENTRY_SIZE"),
        ("DSA_SELF_REPORT_ENTRY_LEN", "DSA_SELF_REPORT_SET_LEN", "self_report.h", "SELF_REPORT_ENTRY_SIZE"),
        ("DSA_ECO_LOG_ENTRY_LEN", "DSA_ECO_LOG_SET_LEN", "eco_log.h", "ECO_LOG_ENTRY_SIZE"),
    ],
)
def test_a1_entry_sizes_agree(base_parser, logger_name, base_define, beacon_header, beacon_define):
    beacon_value = parse_defines(BEACON_SRC / beacon_header)[beacon_define]
    assert getattr(dsa_logger(), logger_name) == base_parser[base_define] == beacon_value


def test_a1_logger_frame_limit_matches_base_mtu():
    mtu = re.search(r"^CONFIG_BT_L2CAP_TX_MTU=(\d+)", read_text(BASE / "prj.conf"), re.M)
    assert mtu, "CONFIG_BT_L2CAP_TX_MTU not set in Network_Base_nrf54/prj.conf"
    assert dsa_logger().MAX_NUS_RAW_PAYLOAD_LEN == int(mtu.group(1))


def test_a1_finished_marker_matches(header):
    beacon_nus = read_text(BEACON_SRC / "nus.c")
    base_parser = read_text(BASE_SRC / "message_parser.c")
    assert '"finished"' in beacon_nus
    assert '"finished"' in base_parser
    assert dsa_logger().DSA_CONTROL_FINISHED_PAYLOAD == b"finished"


def test_a1_status_byte_fields_are_disjoint(header):
    data = header["DATA_LEVEL_MASK"]
    battery = header["BATTERY_LEVEL_MASK"]
    eco = header["ECO_MODE_MASK"]
    assert (data, battery, eco) == (0x0F, 0x70, 0x80)
    assert data & battery == battery & eco == data & eco == 0
