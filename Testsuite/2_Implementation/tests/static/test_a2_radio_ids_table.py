"""A2 - the known-device table (radio_ids.c) is consistent.

See 1_Specification/A_static.md.
"""

from __future__ import annotations

import re
from collections import Counter

import pytest

from cparse import parse_known_device_table
from repo import BEACON, RADIO_IDS_C, read_text

FIELDED_IDS = set(range(1, 171))
DEVELOPMENT_KIT_IDS = {252, 253, 254}


@pytest.fixture(scope="module")
def table():
    devices = parse_known_device_table(RADIO_IDS_C)
    assert devices, "no entries parsed from radio_ids.c"
    return devices


def test_a2_table_parsed_completely(table):
    declared = len(re.findall(r"\.id\s*=", read_text(RADIO_IDS_C)))
    assert len(table) == declared


def test_a2_every_address_has_six_bytes(table):
    bad = [d for d in table if len(d.addr) != 6]
    assert not bad, f"entries with wrong address length: {[(d.device_id, d.line) for d in bad]}"


def test_a2_no_duplicate_addresses(table):
    counts = Counter((d.addr_type, d.addr) for d in table)
    duplicates = [d for d in table if counts[(d.addr_type, d.addr)] > 1]
    assert not duplicates, "duplicate addresses: " + ", ".join(
        f"id {d.device_id} (line {d.line})" for d in duplicates
    )


def test_a2_no_duplicate_ids(table):
    counts = Counter(d.device_id for d in table)
    duplicates = sorted(i for i, n in counts.items() if n > 1)
    assert not duplicates, f"IDs assigned more than once: {duplicates}"


def test_a2_ids_in_fielded_or_development_range(table):
    bad = sorted(d.device_id for d in table if d.device_id not in FIELDED_IDS | DEVELOPMENT_KIT_IDS)
    assert not bad, (
        f"IDs outside 1-170 and 252-254: {bad}. 0xFF means 'unknown' in "
        "lookup_device_id(); extend the fielded range deliberately."
    )


def test_a2_random_addresses_are_static_random(table):
    # Bluetooth Core spec: a static random address has the two most
    # significant bits of its most significant byte set. val[5] is the MSB.
    bad = [d for d in table if d.addr_type == "BT_ADDR_LE_RANDOM" and (d.addr[5] & 0xC0) != 0xC0]
    assert not bad, "not static random addresses: " + ", ".join(
        f"id {d.device_id} (line {d.line})" for d in bad
    )


def test_a2_table_fits_controller_accept_list(table):
    size = re.search(r"^CONFIG_BT_CTLR_FAL_SIZE=(\d+)", read_text(BEACON / "prj.conf"), re.M)
    assert size, "CONFIG_BT_CTLR_FAL_SIZE not set in Network_Beacon_nrf54/prj.conf"
    assert len(table) <= int(size.group(1)), (
        "radio.c adds every known device to the controller filter accept list; "
        f"{len(table)} entries exceed CONFIG_BT_CTLR_FAL_SIZE={size.group(1)}"
    )


def test_a2_development_kits_present(table):
    ids = {d.device_id for d in table}
    assert DEVELOPMENT_KIT_IDS <= ids, "hardware tests need the development kits 252-254 in the table"
