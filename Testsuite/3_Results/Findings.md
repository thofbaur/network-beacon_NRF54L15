# Findings register

Defects and document mismatches found by the test suite. A finding is
closed when the fix is committed **and** the listed tests pass in a
recorded run (`pytest --record`, see `3_Results/<run>/summary.md`).

Status values: **Open** · **Fix in progress** · **Fixed (commit)** · **Accepted (decision)**

| # | Severity | Finding | Failing tests | Found | Status |
|---|---|---|---|---|---|
| 1 | High | Stored contacts are wiped on every reboot (details below) | C1 `contacts_survive_reboot`, `incompatible_format_is_cleared_and_store_usable_in_same_boot`, `power_cut_at_every_write_contacts`; C3 `contacts_in_flash_survive_boot`, `saved_parameters_loaded_when_contacts_pending_at_boot`; hardware: F1, G8, G10, G11, I6 | 2026-10-02 | Open |
| 2 | Medium | Data levels 5–7 are never advertised | C3 `data_level_5_reported_above_100000_contacts`; G11 | 2026-10-02 | Open |
| 3 | Medium | The storage-full bit is never set | C3 `storage_full_bit_set_when_contacts_are_discarded`, C4 `self_report_storage_full_is_signalled`; G11 | 2026-10-02 | Open |
| 4 | High | Manual eco mode stops scanning for good | C9 `manual_eco_mode_keeps_periodic_scan_bursts`, `manual_eco_mode_can_be_left_by_command`; H2 | 2026-10-02 | Open |
| 5 | Low | Logger drops `DSA_NUS_FLAG_VOLTAGE` packets | B1 `voltage_package_is_decoded`, D1 `voltage_flag_decoded_by_both` | 2026-10-02 | Open |
| 6 | Low | Logger `--exclude` / `--omit-*` options have no effect | B3 `exclude_option_omits_matching_lines` | 2026-10-02 | Open |
| 7 | Low | Logger crashes with a traceback when the COM port disappears | B6 `lost_port_is_reported_as_error` | 2026-10-02 | Open |
| 8 | Low | Scan_New_Tags workflow gap and outdated HowTo | A3 `read_tags_csv_is_accepted_by_generate_ids`, `howto_matches_generate_ids_cli`; H7 works around it | 2026-10-02 | Open |
| 9 | Low | Release build still contains printk | E3 `release_build_config` with `--release-build-dir .build/west/beacon_release` | 2026-10-02 | Open |

## Details

### 1 – Stored contacts are wiped on every reboot

`flash_ring_store_init()` rejects stored metadata when
`pending_entries > pending_blocks * flush_batch`. The contact store uses
`flush_batch = 0` (variable block size), so every non-empty contact store
is treated as invalid and cleared at boot. `nvs_clear()` then leaves NVS
unmounted, so the following metadata write fails with `-EACCES`:

- all contacts flushed to flash before the reset are lost;
- `network_init()` returns early, so the saved network parameters (RSSI
  thresholds, tracking on/off) are not loaded for that boot;
- the same unmount problem breaks the store after any format/version
  change (the "incompatible format" path).

Fix direction: skip the `pending_entries` bound for `flush_batch == 0` (use
`block_entry_capacity`), and remount after `nvs_clear()`.

### 2 – Data levels 5–7 are never advertised

`network.c` clamps the contact count to `UINT16_MAX` (65535) and
`contact_status_from_count()` takes a `uint16_t`, but the thresholds of
levels 5–7 are 100000 / 150000 / 210000. Fix direction: use `uint32_t`
end to end.

### 3 – The storage-full bit is never set

`STORAGE_STATUS_STORAGE_FULL` is only set when a flash append returns
`-ENOSPC`. `flash_ring_store_append()` never does: when full it silently
discards the oldest block. Data loss is invisible in the field. Fix
direction: report "full" when the oldest block is discarded (and clear it on
export), or decide that discarding is not "full" and document it.

### 4 – Manual eco mode stops scanning for good

After `P_SET_RAD_ACTIVE = 0`, `radio_apply_mode_locked()` stops scanning and
schedules `eco_scan_handler()`, which returns immediately unless the motion
override is active. No scan bursts run, so no contacts are recorded and no
commands are received; the mode is persisted, so the tag stays deaf after a
reboot. DECISIONS 2026-08-07 says manual eco should behave like automatic
eco. Fix direction: run bursts whenever the effective mode is eco.

### 5 – Logger drops VOLTAGE packets

`parse_payload()` has no branch for `DSA_NUS_FLAG_VOLTAGE` (0x03); the Base
decodes it. The beacon does not send this flag today.

### 6 – Logger options without effect

`--exclude` and `--omit-network-base/--omit-timestamped-lines` only decide
whether discarded garbage text may be skipped; decoded lines are never
filtered, contrary to the option help.

### 7 – Logger crash on port loss

`main()` only handles `RuntimeError`; an `OSError` from the serial port
(USB unplugged, Base reset) ends the capture with a traceback. Lines
already received are kept.

### 8 – Scan_New_Tags workflow

`read_tags.py` writes no `physical_id` column, which `generate_ids.py`
requires (`KeyError`). `HowTo.md` documents a non-existent `--start-id`
option and sequential IDs.

### 9 – printk in the release build

`prj.conf` sets `CONFIG_PRINTK=n`, but NCS's default
`CONFIG_NCS_BOOT_BANNER=y` selects `PRINTK`. Every `printk()` stays in the
production image (output goes nowhere because the console is off – flash
and CPU cost only). Fix candidate: `CONFIG_NCS_BOOT_BANNER=n`.
