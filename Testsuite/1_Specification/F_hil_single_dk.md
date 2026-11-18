# Class F – Hardware tests with DK A only

Run with `pytest --hil 2_Implementation/tests/hil/test_f_dk_a.py` (slow ones need `--slow`).
Requirements for all F tests: nRF54L15 DK A connected over USB (J-Link),
`0_Environment/boards.toml`, SEGGER J-Link software + `pylink-square`, nRF Connect SDK
toolchain (found automatically) for building and flashing.

**Input signals available on DK A:** firmware image flashed with
`west flash --dev-id <serial>` (optionally `--erase`), hardware reset over
SWD (`JLink.reset()`), elapsed wall-clock time, and – for F2/F3 – BLE scans
and readouts from the computer's adapter. Outputs: RTT console text, BLE
advertisement and readout data.

---

### F1 – flash_ring_store on the real flash

| | |
|---|---|
| What is tested | `flash_ring_store.c` on the nRF54L15's real NVS, using the beacon's own `pm_static.yml` partitions. Validates the NVS model of the host fake (C1). |
| Input | Image `2_Implementation/firmware_target/flash_ring_store_target` (ztest, `PM_STATIC_YML_FILE` = beacon's `pm_static.yml`), flashed to DK A; one reset; before every ztest case `contact_storage` and `eco_log_storage` are erased. "Reboot" inside a case = re-initialising the store struct with the flash content kept. |
| Requirements | DK A |
| Automated | ⚙️ before release |
| Implementation | `2_Implementation/tests/hil/test_f_dk_a.py::test_f1_flash_ring_store_on_target` (also `west twister --device-testing` via `testcase.yaml`) |
| Pass criterion | RTT shows `PROJECT EXECUTION SUCCESSFUL` within 300 s |

| ztest case | Input | Expected result |
|---|---|---|
| `test_f1_fixed_batch_entries_survive_reboot` | Eco-log store: 2 blocks × 16 entries; re-init | Count 32. |
| `test_f1_contacts_survive_reboot` | Contact store (`flush_batch = 0`): 10 + 20 entries; re-init | Init 0, count 30. **Expected to fail today – finding 1.** |
| `test_f1_nvs_clear_leaves_fs_unmounted` | Mount, write id 1, `nvs_clear()`, write, remount, read | Write after clear `-EACCES`; read after remount `-ENOENT`. |
| `test_f1_format_change_clears_and_store_usable` | 16 entries with version − 1; re-init with current version; append 16 | Init 0, count 0, append works. **Expected to fail today – finding 1.** |
| `test_f1_full_store_discards_oldest_block` | Eco-log store: block_count + 2 blocks | Count = block_count × 16. |
| `test_f1_full_production_contact_capacity` | 239 blocks × 804 contacts, then 1 more | Count 192156; then 191353. |

---

### F2 – Boot smoke test

| | |
|---|---|
| Requirements | DK A, Bluetooth adapter (`bleak`) |
| Automated | ⚙️ before release |
| Implementation | `2_Implementation/tests/hil/test_f_dk_a.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_f2_boot_smoke[hil]` | Image beacon + `dsa_dev.conf` + `hil.conf`; reset; RTT for 10 s/60 s; BLE scan for DK A's ID (20 s) | RTT `Starting DSA Network Beacon` and `Filled network contact buffer`, no line with "fail"; advertisement `DSA`, own ID, radio status bits 0–4 clear, bit 5 set. |
| `test_f2_boot_smoke[hil_flash]` | Same with `hil_flash.conf` (30000 contacts, flash flushes at boot) | Same. |
| `test_f2_version_reported_at_boot` | hil image; reset | RTT line `version x.y.z … <git hash>`. Skipped with `PREREQUISITE MISSING`. |

---

### F3 – Timer drift against the PC clock

| | |
|---|---|
| Input | hil image; reset; every 30 s for 60 min: BLE scan + readout, recording PC monotonic time at readout start and the uptime field of `TIME_CONTACTS_VOLTAGE` |
| Requirements | DK A, Bluetooth adapter; 60 min (`--slow`) |
| Automated | ⚙️ overnight before release |
| Implementation | `2_Implementation/tests/hil/test_f_dk_a.py::test_f3_timer_drift_against_pc_clock` |
| Expected result | Linear fit of uptime over PC time: |slope − 1| < 250 ppm (value printed). |

---

### F4 – Watchdog recovery

| | |
|---|---|
| Input | Image with watchdog and a test-only hang trigger; the trigger fired over RTT; readout afterwards |
| Requirements | **Prerequisite:** watchdog + hang trigger (open decision). DK A. |
| Automated | ⚙️ once implemented; skipped with `PREREQUISITE MISSING` until `CONFIG_WATCHDOG=y` |
| Implementation | `2_Implementation/tests/hil/test_f_dk_a.py::test_f4_watchdog_recovers_from_hang` |
| Expected result | Reboot within the watchdog timeout, advertising again, flash-stored contacts still delivered. |
