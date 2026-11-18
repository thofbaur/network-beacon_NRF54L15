"""F1-F4 - hardware tests that need only DK A (J-Link USB).

See 1_Specification/F_hil_single_dk.md.
"""

from __future__ import annotations

import re
import time

import pytest

import ble
import firmware
from dut import Dut
from repo import BEACON, PREREQ, read_text

pytestmark = pytest.mark.hil


def test_f1_flash_ring_store_on_target(lab):
    image = firmware.f1_target()
    lab.ensure_a(image)
    dut = lab.dut_a
    dut.reset()
    end = dut.wait_for(r"PROJECT EXECUTION (SUCCESSFUL|FAILED)", timeout=300)
    failed = [line for line in dut.lines if re.search(r"FAIL - test_", line)]
    assert end.group(1) == "SUCCESSFUL", "on-target storage tests failed: " + "; ".join(failed)


@pytest.mark.ble
@pytest.mark.parametrize("variant", ["hil", "hil_flash"])
def test_f2_boot_smoke(lab, variant):
    image = firmware.beacon_hil() if variant == "hil" else firmware.beacon_hil(
        (firmware.CONFIGS / "hil_flash.conf",), suffix="_flash")
    lab.ensure_a(image)
    dut = lab.dut_a
    dut.reset()
    dut.wait_for(r"Starting DSA Network Beacon", timeout=10)
    dut.wait_for(r"Filled network contact buffer", timeout=60)
    log = "\n".join(dut.collect(2.0) + dut.lines)
    failures = [line for line in log.splitlines() if re.search(r"(?i)\bfail(ed|ure)?\b", line)]
    assert not failures, f"error lines at boot: {failures}"
    advert = lab.advert_a()
    assert advert.name == "DSA"
    assert advert.beacon_id == lab.boards.dk_a.beacon_id, "beacon resolved a different own ID"
    storage_bits = advert.radio_status & 0b0001_1111
    assert storage_bits == 0, f"radio/storage status bits set at boot: 0x{advert.radio_status:02x}"
    assert advert.radio_status & 0x20, "DK has no ADXL367: 'motion unavailable' (bit 5) must be set"


def test_f2_version_reported_at_boot(lab):
    if not (BEACON / "VERSION").exists():
        pytest.skip(f"{PREREQ}: Network_Beacon_nrf54/VERSION (firmware version + git hash in the image)")
    lab.ensure_a(firmware.beacon_hil())
    lab.dut_a.reset()
    lab.dut_a.wait_for(r"(?i)version\s+\d+\.\d+\.\d+.*[0-9a-f]{7}", timeout=10)


@pytest.mark.slow
@pytest.mark.ble
def test_f3_timer_drift_against_pc_clock(lab):
    """Beacon uptime (TIME_CONTACTS_VOLTAGE) vs PC clock over 60 minutes."""
    lab.ensure_a(firmware.beacon_hil())
    lab.reset_a()
    samples = []
    end = time.monotonic() + 3600
    while time.monotonic() < end:
        advert = lab.advert_a()
        t_pc = time.monotonic()
        result = ble.readout(advert, timeout=60)
        assert result.tcv is not None
        samples.append((t_pc, result.tcv[0]))
        time.sleep(30)
    n = len(samples)
    mean_x = sum(x for x, _ in samples) / n
    mean_y = sum(y for _, y in samples) / n
    slope = sum((x - mean_x) * (y - mean_y) for x, y in samples) / sum((x - mean_x) ** 2 for x, _ in samples)
    drift_ppm = (slope - 1.0) * 1e6
    print(f"timer drift: {drift_ppm:+.1f} ppm over {n} samples")
    assert abs(drift_ppm) < 250, f"beacon timer drifts {drift_ppm:+.1f} ppm against the PC clock"


def test_f4_watchdog_recovers_from_hang(lab):
    if "CONFIG_WATCHDOG=y" not in read_text(BEACON / "prj.conf"):
        pytest.skip(f"{PREREQ}: watchdog (open decision) and its test-only hang trigger")
    pytest.fail("watchdog present: implement the hang trigger and recovery check (see spec F4)")
