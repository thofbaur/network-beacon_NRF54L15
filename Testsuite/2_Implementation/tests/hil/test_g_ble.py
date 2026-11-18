"""G1-G12 - hardware tests with DK A and the computer's Bluetooth adapter.

DK A runs the hil beacon build (Network_Beacon_nrf54 + dsa_dev.conf +
2_Implementation/configs/hil.conf): after every reset it holds 500 synthetic contacts
(id = timer & 0xFF, timer 1..500) and 10 self-reports in RAM. The computer
reads it out over NUS like the Base. See 1_Specification/G_hil_ble.md.
"""

from __future__ import annotations

import random
import time

import pytest

import ble
import firmware
import frames
import synthetic
from repo import git, latest_release_tag, REPO

pytestmark = [pytest.mark.hil, pytest.mark.ble]

N = synthetic.HIL_CONTACTS


def full_readout(lab, **kwargs) -> ble.Readout:
    result = ble.readout(lab.advert_a(), **kwargs)
    assert result.tcv is not None, "no TIME_CONTACTS_VOLTAGE packet received"
    return result


def test_g1_advertisement_content(hil_beacon):
    advert = hil_beacon.advert_a()
    assert advert.name == "DSA"
    assert advert.beacon_id == hil_beacon.boards.dk_a.beacon_id
    assert advert.network_status & 0x0F == synthetic.data_level(N), "data level after synthetic fill"
    battery = (advert.network_status & 0x70) >> 4
    assert battery in (0, 1, 2, 3)
    assert not advert.network_status & 0x80, "eco bit must be 0 (no inactivity eco on the DK)"
    assert advert.radio_status & 0x20, "motion-unavailable bit must be set (DK has no ADXL367)"
    assert not advert.radio_status & 0x1F, f"fault bits set: 0x{advert.radio_status:02x}"


def test_g2_full_readout_of_synthetic_data(hil_beacon):
    result = full_readout(hil_beacon)
    assert result.finished, "no 'finished' control packet"
    assert result.tcv[1] == N, "contact count in TIME_CONTACTS_VOLTAGE"
    problems = synthetic.check_complete(result.contacts, N)
    assert not problems, "; ".join(problems)
    assert len(result.self_reports) == synthetic.HIL_SELF_REPORTS
    assert all(0 <= t <= 0xFFFFFF for t in result.self_reports)
    assert result.eco_sessions == []
    assert not result.undecodable, f"undecodable notifications: {result.undecodable[:3]}"


def test_g3_readout_drains_the_store(hil_beacon):
    first = full_readout(hil_beacon)
    assert len(first.contacts) == N
    time.sleep(2)
    second = full_readout(hil_beacon)
    assert second.tcv[1] == 0
    assert second.contacts == [] and second.self_reports == []
    time.sleep(1)
    assert hil_beacon.advert_a().network_status & 0x0F == 0, "data level not back to 0 after drain"


def test_g4_unacknowledged_batch_is_resent(hil_beacon):
    def withhold_first_data_ack(flag, count, index):
        if flag == frames.FLAG_DATA and index == 0:
            return None
        return frames.ack(flag, count)

    first = full_readout(hil_beacon, ack_policy=withhold_first_data_ack)
    assert len(first.contacts) <= 48, "beacon must stop contact export after a missing ack"
    second = full_readout(hil_beacon)
    problems = synthetic.check_complete(second.contacts, N)
    assert not problems, "after a missing ack: " + "; ".join(problems)


def test_g4_disconnect_mid_transfer_loses_nothing(hil_beacon):
    first = full_readout(hil_beacon, disconnect_after_batches=3)
    time.sleep(6)  # NUS idle timeout
    second = full_readout(hil_beacon)
    timers = {t for _, t, _ in first.contacts} | {t for _, t, _ in second.contacts}
    assert timers == set(range(1, N + 1)), "contacts lost after an interrupted transfer"
    resent = {t for _, t, _ in first.contacts} & {t for _, t, _ in second.contacts}
    assert len(resent) <= 48, "only the unacknowledged batch may be sent twice"


def test_g5_start_before_notifications_is_ignored(hil_beacon):
    result = ble.readout(hil_beacon.advert_a(), enable_notify_first=False, send_start=False, timeout=8)
    assert result.notifications == [], "transfer started before notifications were enabled"
    second = full_readout(hil_beacon)
    assert len(second.contacts) == N


def test_g5_second_start_command_is_ignored(hil_beacon):
    result = full_readout(hil_beacon, extra_writes=[b"st"])
    tcv_packets = [n for n in result.notifications if n[0] == frames.FLAG_TIME_CONTACTS_VOLTAGE]
    assert len(tcv_packets) == 1, "two transfers started"
    assert not synthetic.check_complete(result.contacts, N)


@pytest.mark.parametrize(
    "bad_ack",
    [
        lambda f, c: bytes([0xAB, f, c]),
        lambda f, c: bytes([0xAC, frames.FLAG_ECO_LOG, c]),
        lambda f, c: bytes([0xAC, f, (c + 1) & 0xFF]),
        lambda f, c: bytes([0xAC, f]),
    ],
    ids=["wrong-magic", "wrong-domain", "wrong-count", "too-short"],
)
def test_g5_wrong_ack_does_not_commit(hil_beacon, bad_ack):
    def policy(flag, count, index):
        if flag == frames.FLAG_DATA and index == 0:
            return bad_ack(flag, count)
        return frames.ack(flag, count)

    full_readout(hil_beacon, ack_policy=policy)
    second = full_readout(hil_beacon)
    assert not synthetic.check_complete(second.contacts, N), "a wrong ack committed the batch"


def test_g6_idle_connection_times_out(hil_beacon):
    seconds = ble.idle_connection(hil_beacon.advert_a(), 15)
    assert seconds is not None, "beacon kept an idle connection open for 15 s"
    assert 3.5 <= seconds <= 8.0, f"idle disconnect after {seconds:.1f} s (expected ~5 s)"


@pytest.mark.slow
def test_g7_reset_during_flash_write(lab):
    """Reset DK A at random moments while synthetic contacts are flushed,
    then read out: every record intact, counts consistent."""
    image = firmware.beacon_hil((firmware.CONFIGS / "hil_flash.conf",), suffix="_flash")
    lab.ensure_a(image, erase=True)
    rng = random.Random(7)
    for cycle in range(20):
        lab.dut_a.reset()
        time.sleep(rng.uniform(0.5, 6.0))  # boot fill + flush in progress
    lab.reset_a(timeout=90)
    time.sleep(10)  # let the flush of this boot finish
    result = full_readout(lab, timeout=600)
    bad = [c for c in result.contacts if not synthetic.contact_is_synthetic(c)]
    assert not bad, f"{len(bad)} corrupted records after resets, e.g. {bad[:3]}"
    assert result.tcv[1] == len(result.contacts), "advertised count != delivered contacts"
    timers = [t for _, t, _ in result.contacts]
    assert set(range(1, synthetic.FLASH_CONTACTS + 1)) <= set(timers), "current boot's contacts incomplete"


@pytest.mark.slow
def test_g8_flushed_contacts_survive_reset(lab):
    image = firmware.beacon_hil((firmware.CONFIGS / "hil_flash.conf",), suffix="_flash")
    lab.ensure_a(image, erase=True)
    lab.reset_a(timeout=90)
    time.sleep(10)
    flushed = synthetic.flushed_contacts(synthetic.FLASH_CONTACTS)
    lab.reset_a(timeout=90)  # RAM contacts are lost by design, flash ones must stay
    time.sleep(10)
    result = full_readout(lab, timeout=600)
    expected = synthetic.FLASH_CONTACTS + flushed
    assert len(result.contacts) == expected, (
        f"{len(result.contacts)} contacts after reset, expected {expected} "
        f"({flushed} flushed before the reset + {synthetic.FLASH_CONTACTS} of this boot)"
    )


@pytest.mark.ble
def test_g9_readout_duration_and_packet_size(hil_beacon):
    result = full_readout(hil_beacon)
    assert result.finished
    payload_limit = (result.mtu or 247) - 3
    oversized = [len(n) for n in result.notifications if len(n) > payload_limit]
    assert not oversized, f"notifications larger than MTU-3 ({payload_limit}): {oversized[:3]}"
    assert result.duration_s < 30, f"readout of {N} contacts took {result.duration_s:.1f} s"
    print(f"MTU {result.mtu}, {len(result.notifications)} notifications, {result.duration_s:.1f} s")


@pytest.mark.slow
def test_g10_firmware_update_keeps_data(lab, tmp_path):
    tag = latest_release_tag()
    if tag is None:
        pytest.skip("no release tag to update from")
    worktree = tmp_path / tag
    git("worktree", "add", "--detach", str(worktree), tag)
    try:
        old = firmware.Image(f"beacon_{tag}_flash", worktree / "Network_Beacon_nrf54", firmware.DK,
                             (worktree / "Network_Beacon_nrf54" / "dsa_dev.conf",
                              firmware.CONFIGS / "hil.conf", firmware.CONFIGS / "hil_flash.conf"))
        lab.ensure_a(old, erase=True)
        lab.reset_a(timeout=90)
        time.sleep(10)
        flushed = synthetic.flushed_contacts(synthetic.FLASH_CONTACTS)
        new = firmware.beacon_hil(suffix="_after_update")
        lab.ensure_a(new)  # no erase: storage partitions keep their content
        lab.reset_a()
        advert = lab.advert_a()
        assert not advert.radio_status & 0x1C, f"storage fault after update: 0x{advert.radio_status:02x}"
        result = full_readout(lab, timeout=600)
        assert len(result.contacts) >= flushed + synthetic.HIL_CONTACTS, (
            f"contacts stored by {tag} were lost by the update ({len(result.contacts)} read)"
        )
    finally:
        git("worktree", "remove", "--force", str(worktree))


@pytest.mark.slow
def test_g11_storage_full(lab):
    image = firmware.beacon_hil((firmware.CONFIGS / "hil_full.conf",), suffix="_full")
    lab.ensure_a(image, erase=True)
    for _ in range(4):  # 4 x 65535 contacts > 216156 capacity
        lab.reset_a(timeout=180)
        time.sleep(60)
    advert = lab.advert_a()
    assert advert.radio_status & 0x04, "storage-full bit (2) not set although contacts were discarded"
    assert advert.network_status & 0x0F == 7, "data level 7 expected above 210000 contacts"
    result = full_readout(lab, timeout=1800)
    bad = [c for c in result.contacts if not synthetic.contact_is_synthetic(c)]
    assert not bad


def test_g12_unknown_central_can_read_out(hil_beacon):
    """DOCUMENTED RISK: any device can connect and drain a tag. This test
    records today's behaviour; invert it when the readout-security decision
    adds a protection."""
    result = full_readout(hil_beacon)
    assert result.finished and len(result.contacts) == N
