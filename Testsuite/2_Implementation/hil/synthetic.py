"""Expected content of the synthetic data a dsa_dev/hil build creates at
every boot (Network_Beacon_nrf54/src/development_validation.c):

  contact i = 1..N : id2 = i & 0xFF, timer = i & 0xFFFFFF, rssi magnitude
                     30 + rand % 70  (30..99)
  self-reports     : CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORT_COUNT random times
"""

from __future__ import annotations

from collections import Counter

HIL_CONTACTS = 500
HIL_SELF_REPORTS = 10
FLASH_CONTACTS = 30000
FLUSH_THRESHOLD = 23700
BLOCK_CONTACTS = 804


def contact_is_synthetic(contact: tuple[int, int, int]) -> bool:
    id2, timer, rssi = contact
    return id2 == (timer & 0xFF) and -99 <= rssi <= -30


def check_complete(contacts: list[tuple[int, int, int]], n: int) -> list[str]:
    """Problems found when a readout should contain exactly contacts 1..n."""
    problems = []
    bad = [c for c in contacts if not contact_is_synthetic(c)]
    if bad:
        problems.append(f"{len(bad)} corrupted/unexpected records, e.g. {bad[:3]}")
    timers = Counter(t for _, t, _ in contacts)
    missing = [t for t in range(1, n + 1) if t not in timers]
    dupes = [t for t, k in timers.items() if k > 1]
    extra = [t for t in timers if not 1 <= t <= n]
    if missing:
        problems.append(f"{len(missing)} missing, e.g. timers {missing[:5]}")
    if dupes:
        problems.append(f"{len(dupes)} duplicated, e.g. timers {dupes[:5]}")
    if extra:
        problems.append(f"{len(extra)} outside 1..{n}, e.g. {extra[:5]}")
    return problems


def flushed_contacts(n: int) -> int:
    """Contacts a boot with n synthetic contacts leaves in flash."""
    flushed = 0
    ram = n
    while ram >= FLUSH_THRESHOLD:
        flushed += BLOCK_CONTACTS
        ram -= BLOCK_CONTACTS
    return flushed


def data_level(count: int) -> int:
    for level, threshold in ((7, 210000), (6, 150000), (5, 100000), (4, 40000), (3, 5000), (2, 1), (1, 0)):
        if count > threshold:
            return level
    return 0
