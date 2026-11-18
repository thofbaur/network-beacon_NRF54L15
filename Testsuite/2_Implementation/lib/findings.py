"""Open findings (3_Results/Findings.md) as strict expected failures.

A test that fails because of an open finding is marked with `@finding(n)`.
It then reports as `xfailed` and the run stays green, so any real failure is
new. The mark is strict: once the finding is fixed the test passes, pytest
reports `XPASS(strict)` as a failure, and the mark must be removed together
with the fix (and the finding closed in Findings.md).
"""

from __future__ import annotations

import pytest

# Finding number -> title in 3_Results/Findings.md (and Release 2.0 ID)
OPEN_FINDINGS = {
    1: "Stored contacts are wiped on every reboot (B1)",
    2: "Data levels 5-7 are never advertised (B4)",
    3: "The storage-full bit is never set (B2)",
    4: "Manual eco mode stops scanning for good (B3)",
    5: "Logger drops VOLTAGE packets (B10)",
    6: "Logger options without effect (B11)",
    7: "Logger crash on port loss (B9)",
    8: "Scan_New_Tags workflow (B14)",
}

# C test cases (pytest id "<target>::<case>" of test_c) failing because of a finding
C_CASES = {
    "c1_flash_ring_store::c1_contacts_survive_reboot": 1,
    "c1_flash_ring_store::c1_incompatible_format_is_cleared_and_store_usable_in_same_boot": 1,
    "c1_flash_ring_store::c1_power_cut_at_every_write_contacts": 1,
    "c3_network::c3_contacts_in_flash_survive_boot": 1,
    "c3_network::c3_saved_parameters_loaded_when_contacts_pending_at_boot": 1,
    "c3_network::c3_data_level_5_reported_above_100000_contacts": 2,
    "c3_network::c3_storage_full_bit_set_when_contacts_are_discarded": 3,
    "c4_self_report_eco_log::c4_self_report_storage_full_is_signalled": 3,
    "c9_radio::c9_manual_eco_mode_keeps_periodic_scan_bursts": 4,
    "c9_radio::c9_manual_eco_mode_can_be_left_by_command": 4,
}


def finding(number: int) -> pytest.MarkDecorator:
    """Strict expected failure caused by open finding `number`."""
    return pytest.mark.xfail(strict=True, reason=f"Finding {number}: {OPEN_FINDINGS[number]}")
