"""D2 - beacon packet encoder round trip through both decoders.

Needs the optional refactor that moves the NUS packet building in
Network_Beacon_nrf54/src/nus.c (send_networkdata, send_self_reports,
send_eco_log, send_uptime_contacts_voltage) into pure functions that can be
compiled for the host. Until then the test is skipped. See
1_Specification/D_contract.md.
"""

from __future__ import annotations

import pytest

from repo import BEACON_SRC, PREREQ, read_text

ENCODER_HEADER = BEACON_SRC / "nus_packets.h"


def test_d2_encoder_roundtrip():
    if not ENCODER_HEADER.exists():
        pytest.skip(
            f"{PREREQ}: {ENCODER_HEADER.name} (pure NUS packet builders split out of "
            "nus.c, see 'Test-only firmware additions' in the test plan)"
        )
    # Once the encoder exists: build it with 2_Implementation/firmware_unit/build.py, encode a
    # generated set of contacts/self-reports/eco sessions, and feed the bytes
    # to dsa_logger.parse_payload and the Base dump tool (as in D1).
    text = read_text(ENCODER_HEADER)
    pytest.fail(f"nus_packets.h exists ({len(text)} bytes): implement the D2 round trip now")
