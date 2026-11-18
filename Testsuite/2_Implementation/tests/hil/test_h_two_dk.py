"""H1-H10 - hardware tests with DK A (beacon under test) and DK B in a
changing role: Commander (Control with UART commands), Beacon partner,
Base, or Scan_New_Tags. See 1_Specification/H_hil_two_dk.md.
"""

from __future__ import annotations

import csv
import time

import pytest

import ble
import firmware
import frames
import synthetic
from repo import SCAN_TAGS, load_module
from uart import BaseLink, Commander, ScannerLink

pytestmark = [pytest.mark.hil, pytest.mark.two_dk]

# P_* parameter values (shared/common_include.h)
P_MAIN_LED_ACTIVE = 0x21
P_MAIN_LED_INTERVAL_S = 0x22
P_RSSI_NETWORK = 0x64
P_RSSI_LOCATION = 0x65
P_NETWORK_RESET_PARAMS = 0x6C
P_TRACKING_ACTIVE = 0x6D
P_ADV_INTERVAL_MS = 0x81
P_SCAN_INTERVAL_MS = 0x83
P_SCAN_WINDOW_MS = 0x85
P_ECO_SCAN_PERIOD_S = 0x87
P_RADIO_RESET_PARAMS = 0x8C
P_SET_RAD_ACTIVE = 0x8D
P_MOTION_INACTIVITY_TIMEOUT_S = 0xA2
BROADCAST = 0xFF

# (parameter, valid value, RTT line on DK A, invalid value or None)
COMMAND_TABLE = [
    (P_MAIN_LED_ACTIVE, 1, r"Status LED enabled", 2),
    (P_MAIN_LED_INTERVAL_S, 5, r"Status LED interval set to 5 s", 0),
    (P_RSSI_NETWORK, 70, r"Network RSSI threshold set to -70 dBm", 256),
    (P_RSSI_LOCATION, 90, r"Location RSSI threshold set to -90 dBm", 300),
    (P_TRACKING_ACTIVE, 0, r"Network contact tracking disabled", 2),
    (P_MOTION_INACTIVITY_TIMEOUT_S, 60, r"Inactivity timeout set to 60 s", 0),
]


@pytest.fixture
def commander(hil_beacon, require_commander):
    hil_beacon.ensure_b(firmware.control_commander())
    with hil_beacon.dk_b_uart(Commander) as cmd:
        yield cmd


@pytest.mark.parametrize("param, valid, rtt, invalid", COMMAND_TABLE, ids=[hex(c[0]) for c in COMMAND_TABLE])
def test_h1_command_applied_and_invalid_rejected(hil_beacon, commander, param, valid, rtt, invalid):
    dut = hil_beacon.dut_a
    dut.clear()
    commander.send(hil_beacon.boards.dk_a.beacon_id, param, valid)
    dut.wait_for(rtt, timeout=15)
    if invalid is not None:
        dut.clear()
        commander.send(hil_beacon.boards.dk_a.beacon_id, param, invalid)
        dut.wait_for(r"Rejecting", timeout=15)


def test_h1_broadcast_and_foreign_target(hil_beacon, commander):
    dut = hil_beacon.dut_a
    dut.clear()
    commander.send(BROADCAST, P_RSSI_NETWORK, 71)
    dut.wait_for(r"Network RSSI threshold set to -71 dBm", timeout=15)
    dut.clear()
    commander.send((hil_beacon.boards.dk_a.beacon_id + 1) & 0xFF, P_RSSI_NETWORK, 72)
    dut.wait_for(r"Ignoring command for target", timeout=15)


def test_h1_parameters_persist_across_reset(hil_beacon, commander):
    dut = hil_beacon.dut_a
    commander.send(hil_beacon.boards.dk_a.beacon_id, P_RSSI_NETWORK, 66)
    dut.wait_for(r"set to -66 dBm", timeout=15)
    hil_beacon.reset_a()
    assert not any("No stored network parameters" in line for line in dut.lines)
    commander.send(hil_beacon.boards.dk_a.beacon_id, P_NETWORK_RESET_PARAMS, 0)
    dut.wait_for(r"Network parameters reset", timeout=15)


def test_h2_manual_eco_mode_round_trip(hil_beacon, commander):
    a = hil_beacon.boards.dk_a.beacon_id
    commander.send(a, P_ECO_SCAN_PERIOD_S, 10)
    commander.send(a, P_SET_RAD_ACTIVE, 0)
    time.sleep(5)
    # In eco mode scanning runs in bursts every 10 s; a command sent for
    # 15 s must be received.
    hil_beacon.dut_a.clear()
    commander.send(a, P_SET_RAD_ACTIVE, 1, duration_ms=15000)
    hil_beacon.dut_a.wait_for(r"Command parameter 0x8d value 1", timeout=5)
    result = ble.readout(hil_beacon.advert_a())
    assert len(result.eco_sessions) == 1, "leaving eco mode must log one eco session"
    enter, leave = result.eco_sessions[0]
    assert 0 < leave - enter < 120
    commander.send(a, P_RADIO_RESET_PARAMS, 0)


@pytest.fixture
def partner_beacon(hil_beacon):
    hil_beacon.ensure_b(firmware.beacon_partner())
    return hil_beacon.boards.dk_b.beacon_id


def test_h3_contacts_with_partner_recorded(hil_beacon, partner_beacon):
    ble.readout(hil_beacon.advert_a())  # drain synthetic data
    time.sleep(30)  # default scan: 120 ms window every 7 s
    result = ble.readout(hil_beacon.advert_a())
    partner = [c for c in result.contacts if c[0] == partner_beacon and not synthetic.contact_is_synthetic(c)]
    assert partner, f"no contacts with beacon {partner_beacon} recorded in 30 s"
    assert all(-100 <= rssi <= -10 for _, _, rssi in partner)


def test_h3_thresholds_and_tracking(hil_beacon, partner_beacon, require_commander):
    a = hil_beacon.boards.dk_a.beacon_id
    hil_beacon.ensure_b(firmware.control_commander())
    with hil_beacon.dk_b_uart(Commander) as commander:
        commander.send(a, P_RSSI_NETWORK, 0)  # nothing is closer than 0 dBm
    hil_beacon.ensure_b(firmware.beacon_partner())
    ble.readout(hil_beacon.advert_a())
    time.sleep(30)
    none = ble.readout(hil_beacon.advert_a())
    assert not [c for c in none.contacts if c[0] == partner_beacon], "threshold 0 must reject every contact"
    hil_beacon.ensure_b(firmware.control_commander())
    with hil_beacon.dk_b_uart(Commander) as commander:
        commander.send(a, P_NETWORK_RESET_PARAMS, 0)


def test_h4_location_tag_uses_location_threshold(hil_beacon, require_commander):
    a = hil_beacon.boards.dk_a.beacon_id
    hil_beacon.ensure_b(firmware.control_commander())
    with hil_beacon.dk_b_uart(Commander) as commander:
        commander.send(a, P_RSSI_NETWORK, 0)
        commander.send(a, P_RSSI_LOCATION, 255)
    hil_beacon.ensure_b(firmware.beacon_partner(location_tag=True))
    ble.readout(hil_beacon.advert_a())
    time.sleep(30)
    result = ble.readout(hil_beacon.advert_a())
    assert [c for c in result.contacts if c[0] == hil_beacon.boards.dk_b.beacon_id], (
        "DSL partner not recorded although the location threshold accepts everything"
    )
    hil_beacon.ensure_b(firmware.control_commander())
    with hil_beacon.dk_b_uart(Commander) as commander:
        commander.send(a, P_NETWORK_RESET_PARAMS, 0)


def contacts_from_base(transfer) -> list[tuple[int, int, int]]:
    out = []
    for msg in transfer:
        for line in msg.output_lines:
            if line.startswith("ID2: "):
                parts = [p.split(": ")[1] for p in line.split(", ")]
                out.append((int(parts[0]), int(parts[1]), -int(parts[2])))
    return out


def test_h5_production_chain_through_base(hil_beacon):
    a = hil_beacon.boards.dk_a.beacon_id
    hil_beacon.ensure_b(firmware.base_hil(a))
    with hil_beacon.dk_b_uart(BaseLink) as base:
        transfer = base.wait_finished(a, timeout=180)
    problems = synthetic.check_complete(contacts_from_base(transfer), synthetic.HIL_CONTACTS)
    assert not problems, "; ".join(problems)
    lines = [line for m in transfer for line in m.output_lines]
    assert f"Contact Count: {synthetic.HIL_CONTACTS}" in lines
    assert sum(line.startswith("Self-report time:") for line in lines) == synthetic.HIL_SELF_REPORTS


def test_h5_production_chain_with_id_48(lab, require_force_id):
    """End-to-end regression for 2e1f52d: a beacon with ID 48 must arrive
    in the log as ID 48, not as '0'."""
    image = firmware.beacon_hil(defines=("CONFIG_DSA_DEV_FORCE_ID=48",), suffix="_id48")
    lab.ensure_a(image)
    lab.reset_a()
    lab.ensure_b(firmware.base_hil(48))
    with lab.dk_b_uart(BaseLink) as base:
        transfer = base.wait_finished(48, timeout=180)
    assert all(m.beacon_id == "48" for m in transfer)
    assert len(contacts_from_base(transfer)) == synthetic.HIL_CONTACTS


def test_h6_base_recovers_from_beacon_reset_mid_transfer(hil_beacon):
    a = hil_beacon.boards.dk_a.beacon_id
    hil_beacon.ensure_b(firmware.base_hil(a))
    with hil_beacon.dk_b_uart(BaseLink) as base:
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            base.poll()
            if any(m.flag_value == frames.FLAG_DATA for m in base.messages):
                break
            time.sleep(0.05)
        hil_beacon.dut_a.reset()  # beacon disappears mid-transfer
        base.messages.clear()
        transfer = base.wait_finished(a, timeout=240)
    assert not synthetic.check_complete(contacts_from_base(transfer), synthetic.HIL_CONTACTS)


def test_h7_scan_new_tags_workflow(lab, tmp_path):
    lab.ensure_a(firmware.nordic_lbs())
    lab.ensure_b(firmware.scan_new_tags())
    with lab.dk_b_uart(ScannerLink) as scanner:
        lab.dut_a.reset(clear_log=False)
        line = scanner.wait_device("Nordic_LBS", timeout=60)
    read_tags = load_module(SCAN_TAGS / "read_tags.py", "dsa_test_read_tags")
    m = read_tags.LINE_RE.match(line)
    assert m, f"scanner line not understood by read_tags.py: {line}"
    addr = m.group("addr").split(":")
    gen = load_module(SCAN_TAGS / "generate_ids.py", "dsa_test_generate_ids")
    gen.SCRIPT_DIR = str(tmp_path)
    with open(tmp_path / "tag_adresses_hil.csv", "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([*read_tags.CSV_HEADER, "physical_id"])  # manual step (see A3)
        writer.writerow(["Nordic_LBS", *addr, "200"])
    gen.main()
    text = (tmp_path / "ids.c").read_text()
    expected = ", ".join(f"0x{b.upper()}" for b in reversed(addr))
    assert expected in text, f"reversed address {expected} not in generated ids.c"
    lab.current.pop(lab.boards.dk_a.jlink_serial, None)  # DK A must be reprogrammed next


@pytest.mark.slow
def test_h8_contacts_during_readouts(hil_beacon, partner_beacon):
    """Contacts keep arriving while the store is exported and flushed."""
    seen: dict[int, int] = {}
    for _ in range(20):
        result = ble.readout(hil_beacon.advert_a())
        for c in result.contacts:
            if synthetic.contact_is_synthetic(c) and c[0] != partner_beacon:
                seen[c[1]] = seen.get(c[1], 0) + 1
            elif c[0] != partner_beacon:
                pytest.fail(f"corrupted record {c}")
        assert not result.undecodable
        time.sleep(5)
    dupes = {t: n for t, n in seen.items() if n > 1}
    assert len(dupes) <= 48, f"{len(dupes)} synthetic contacts delivered more than once"


@pytest.mark.tags
def test_h9_base_reads_several_beacons(lab):
    if not lab.boards.tag_ids:
        pytest.skip("no tags listed in boards.toml [tags]")
    lab.ensure_a(firmware.beacon_hil())
    lab.reset_a()
    lab.ensure_b(firmware.base_hil(255))  # 255: any beacon
    with lab.dk_b_uart(BaseLink) as base:
        for beacon_id in [lab.boards.dk_a.beacon_id, *lab.boards.tag_ids]:
            transfer = base.wait_finished(beacon_id, timeout=600)
            assert contacts_from_base(transfer), f"no contacts from beacon {beacon_id}"


@pytest.mark.slow
def test_h10_contact_detection_rate(hil_beacon, partner_beacon):
    ble.readout(hil_beacon.advert_a())
    minutes = 10
    time.sleep(minutes * 60)
    result = ble.readout(hil_beacon.advert_a())
    partner = [c for c in result.contacts if c[0] == partner_beacon and not synthetic.contact_is_synthetic(c)]
    rate = len(partner) / minutes
    print(f"contacts per minute with a partner at ~1 m: {rate:.1f}")
    # 120 ms scan window every 7 s, partner advertising every 90-120 ms:
    # about 8.6 windows/min with ~1 advert each.
    assert 3 <= rate <= 30, f"unexpected contact rate {rate:.1f}/min"
