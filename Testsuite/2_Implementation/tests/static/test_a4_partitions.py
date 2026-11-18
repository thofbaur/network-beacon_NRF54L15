"""A4 - the flash partition layout (pm_static.yml) is consistent and the
storage partitions are unchanged since the last release, so a firmware
update keeps the stored data readable. See 1_Specification/A_static.md.
"""

from __future__ import annotations

import re

import pytest

from repo import BEACON, git, latest_release_tag, read_text

STORAGE = ["contact_storage", "self_report_storage", "settings_storage", "eco_log_storage"]
NONSECURE_CEILING = 0x165000  # nRF54L15: FLPR + TF-M reservations above this
SECTOR = 0x1000


def parse_pm(text: str) -> dict[str, dict]:
    parts: dict[str, dict] = {}
    current = None
    for raw in text.replace("\r\n", "\n").splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        m = re.match(r"^(\w+):\s*$", raw)
        if m:
            current = m.group(1)
            parts[current] = {"span": []}
            continue
        m = re.match(r"^\s+(address|size):\s*(\S+)", raw)
        if m and current:
            parts[current][m.group(1)] = int(m.group(2), 0)
            continue
        m = re.match(r"^\s+-\s+(\w+)", raw)
        if m and current:
            parts[current]["span"].append(m.group(1))
    return parts


@pytest.fixture(scope="module")
def pm():
    return parse_pm(read_text(BEACON / "pm_static.yml"))


def leaves(pm):
    return {n: p for n, p in pm.items() if not p["span"]}


def test_a4_leaf_partitions_do_not_overlap(pm):
    items = sorted(leaves(pm).items(), key=lambda kv: kv[1]["address"])
    for (name_a, a), (name_b, b) in zip(items, items[1:]):
        assert a["address"] + a["size"] <= b["address"], f"{name_a} overlaps {name_b}"


def test_a4_spans_cover_their_children_exactly(pm):
    for name, part in pm.items():
        if not part["span"]:
            continue
        children = [pm[c] for c in part["span"]]
        start = min(c["address"] for c in children)
        end = max(c["address"] + c["size"] for c in children)
        assert part["address"] == start, f"{name} does not start at its first child"
        assert part["address"] + part["size"] == end, f"{name} does not end at its last child"
        assert sum(c["size"] for c in children) == part["size"], f"{name} has gaps"


def test_a4_storage_is_sector_aligned_and_below_ceiling(pm):
    for name in STORAGE:
        part = pm[name]
        assert part["address"] % SECTOR == 0 and part["size"] % SECTOR == 0, name
        assert part["size"] >= 2 * SECTOR, f"{name}: NVS needs at least two sectors"
        assert part["address"] + part["size"] <= NONSECURE_CEILING, f"{name} above 0x165000"


@pytest.mark.parametrize(
    "partition, symbol",
    [
        ("contact_storage", "CONFIG_DSA_NETWORK_FLASH_SIZE_BYTES"),
        ("self_report_storage", "CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES"),
        ("eco_log_storage", "CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES"),
    ],
)
def test_a4_partition_sizes_match_kconfig(pm, partition, symbol):
    conf = read_text(BEACON / "dsa.conf")
    m = re.search(rf"^{symbol}=(\S+)", conf, re.M)
    assert m, f"{symbol} not set in dsa.conf"
    assert pm[partition]["size"] == int(m.group(1), 0)


def test_a4_settings_partition_matches_prj_conf(pm):
    m = re.search(r"^CONFIG_PM_PARTITION_SIZE_SETTINGS_STORAGE=(\S+)", read_text(BEACON / "prj.conf"), re.M)
    assert m
    assert pm["settings_storage"]["size"] == int(m.group(1), 0)


@pytest.mark.parametrize("partition", STORAGE)
def test_a4_storage_partition_unchanged_since_last_release(pm, partition):
    tag = latest_release_tag()
    if tag is None:
        pytest.skip("no release tag (v*) in the repository")
    try:
        released = parse_pm(git("show", f"{tag}:Network_Beacon_nrf54/pm_static.yml"))
    except RuntimeError:
        pytest.skip(f"{tag} has no Network_Beacon_nrf54/pm_static.yml")
    assert partition in released, f"{partition} did not exist in {tag}"
    now, then = pm[partition], released[partition]
    assert (now["address"], now["size"]) == (then["address"], then["size"]), (
        f"{partition} moved/resized since {tag}: data stored by {tag} would be "
        "lost or misread after an update; record a migration decision first"
    )
