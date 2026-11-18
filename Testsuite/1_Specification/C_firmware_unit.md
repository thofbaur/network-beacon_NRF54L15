# Class C – Firmware unit tests on the PC

No hardware, fully automated, run on every commit
(`pytest 2_Implementation/tests/firmware_unit` or `python 2_Implementation/firmware_unit/build.py --run`).

**How it works.** The unmodified firmware sources are compiled for the host
against a Zephyr stand-in (`2_Implementation/firmware_unit/shim/`) and recording fakes
(`2_Implementation/firmware_unit/fakes/`). Kconfig values and the partition table are
generated from the real `Kconfig`, `dsa.conf`, `dsa_runtime.conf` and
`pm_static.yml`, so the tests run the production configuration. Every case
runs in its own process (fresh module state).

**Requirements:** Python, pytest, a C compiler (gcc/clang, or the bundled
`ziglang` package – no installation rights needed); Windows, macOS, Linux.

**Fidelity notes.** Time and work items advance only when a test says so
(deterministic). Mutexes are no-ops, so races are not reproducible here
(→ H8). The NVS fake reproduces the Zephyr NVS behaviour the firmware relies
on, checked against `zephyr/subsys/fs/nvs/nvs.c` (NCS v3.3.0) and on
hardware by F1: reads return the stored length, identical writes return 0,
`nvs_clear()` leaves the file system unmounted, writes are atomic.

**Input signals available to the C tests**

| Signal | Injected through | Meaning |
|---|---|---|
| Time | `shim_set_uptime_ms()`, `shim_advance_ms()` | `k_uptime_*()`; advancing runs every work item that becomes due |
| Work execution | `shim_run_ready_work()` | Runs submitted/expired work (storage flush, status update, command handling) |
| Flash content | `fake_nvs_put_raw()`, records written by earlier steps | NVS records per partition; survive a simulated reboot |
| Reboot | Re-initialising the store struct + `fake_nvs_power_cycle()` | RAM state lost, flash kept |
| Power cut | `fake_nvs_cut_power_after(n)` | The (n+1)-th flash write/delete is lost, all later ones fail until power cycle |
| Flash faults | `fake_nvs_set_read/write/delete_error()`, `fake_nvs_corrupt_partition()`, `shim_flash_set_page_size/page_info_error/erase_error()`, `shim_flash_device.ready` | I/O errors, unmountable partition, wrong geometry, device not ready |
| Settings | `fake_settings_put_raw()`, `fake_settings_set_init/load/save_error()` | Stored parameter records, legacy records, settings errors |
| Button / GPIO | `shim_gpio_set_input(pin, v)` + `shim_gpio_fire(pin)`, `shim_gpio_set_configure_error()` | Button press/release edges (button0 = pin 20), GPIO failures |
| ADC | `shim_adc_set_raw()`, `shim_adc_set_setup/read_error()`, `shim_generic_device.ready` | Battery sample (raw = mV · 16384 / 3600) |
| BLE advertising reports | `shim_bt_deliver_adv(addr, rssi, AD bytes)` → captured scan callback | Raw AD structures `len, type, data` (name 0x09, manufacturer data 0xFF) |
| BLE stack results | `shim_bt.scan_start_err`, `adv_start_err`, `accept_list_err`, `identity` | Controller errors, own identity address |
| Development hooks | `network_development_validation_append_contact(id, t, rssi)`, `self_report_development_validation_append(t)` | Contacts/self-reports with explicit timestamp (code paths of `dsa_dev.conf` builds) |
| Peer behaviour | `fake_peers` (`nus_connected`, `nus_init_err`, `adv_update_err`) | State of neighbouring modules |

---

### C1 – flash_ring_store.c

| | |
|---|---|
| What is tested | The flash FIFO under contacts, self-reports and eco sessions. |
| Configurations | CONTACT = production contact store (entries 5 B, 804 per block, 239 blocks, `flush_batch = 0`, partition `contact_storage`); BATCH = production self-report store (3 B, 64 per block, 4 blocks, `flush_batch = 64`); SMALL = 3 blocks × 4 entries of 5 B |
| Entry pattern | Entry *i* = bytes derived from *i* (bytes 0–2 carry *i* XOR a fixed pattern), so every entry is self-checking for corruption and order |
| Implementation | `2_Implementation/firmware_unit/tests/test_c1_flash_ring_store.c` |

| Case | Input | Expected result |
|---|---|---|
| `c1_production_contact_geometry` | `network_storage.h` macros | 239 blocks of 804; block ≤ 4096 − 64 bytes. |
| `c1_init_empty_partition_has_no_entries` | CONTACT, empty flash | Init 0, count 0, no init fault. |
| `c1_append_then_peek_returns_entries_in_order` | CONTACT: append entries 0–9, drain | 10 entries in order, count 0. |
| `c1_append_rejects_invalid_entry_counts` | CONTACT: append NULL; 0 entries; 805; 804 | `-EINVAL` ×3; 804 accepted. |
| `c1_fixed_batch_store_rejects_other_batch_sizes` | BATCH: append 63; 64 | `-EINVAL`; accepted. |
| `c1_peek_returns_whole_entries_only` | CONTACT, 3 entries; peek with 4-byte, 12-byte buffer; NULL buffer | 0 bytes; 10 bytes; `-EINVAL`. |
| `c1_partial_drop_advances_within_block` | SMALL: append 4, drop 2, peek | Not retired; count 2; peek starts at entry 2. |
| `c1_drop_more_than_remaining_in_oldest_block_is_rejected` | SMALL: blocks of 4 + 4, drop 5 | `-EINVAL`; count 8. |
| `c1_drop_on_empty_store_returns_enoent` | SMALL empty: drop 1; drop 0; drop with NULL flag | `-ENOENT`; 0; `-EINVAL`. |
| `c1_full_block_drop_retires_and_deletes_block_record` | SMALL: append 4, drop 4 | Retired; record 0x100 deleted; count 0. |
| `c1_fifo_order_across_blocks` | SMALL: append 4 + 3 + 2 entries, drain | Entries 0–8 in order. |
| `c1_full_store_discards_oldest_block_on_append` | SMALL: 5 blocks of 4 | Count 12; entries 8–19 remain. |
| `c1_long_run_wraps_block_ids_and_stays_fifo` | SMALL: 200 rounds appending 1–4 entries, drain every 3rd round | Always FIFO; ≤ 4 records on flash. |
| `c1_fixed_batch_entries_survive_reboot` | BATCH: append 2 × 64, reboot | Count 128, entries 0–127. |
| `c1_contacts_survive_reboot` | CONTACT: append 10 + 20, reboot | Init 0, count 30. **Fails today – finding 1.** |
| `c1_partial_drop_survives_reboot_after_sync` | BATCH: 2 × 64, drop 10, sync, reboot | Count 118, first entry 10. |
| `c1_partial_drop_without_sync_is_resent_after_reboot` | BATCH: 64, drop 10 (no sync), reboot | Count 64, first entry 0. |
| `c1_full_block_drop_is_durable_without_sync` | BATCH: 2 × 64, drop 64, reboot | Count 64. |
| `c1_lost_metadata_is_rebuilt_from_blocks` | BATCH: 3 × 64, delete record id 1 (metadata), reboot | Count 192, first entry 0. |
| `c1_block_written_before_power_cut_is_recovered` | BATCH: 64; then `cut_power_after(1)` and append 64 (block written, metadata write lost); power on, reboot | Append returns error; after reboot count 128. |
| `c1_incompatible_format_is_cleared_and_store_usable_in_same_boot` | BATCH with version − 1: append 64; reboot with the current version | Init 0, count 0, new append works. **Fails today – finding 1.** |
| `c1_unmountable_partition_is_erased_once_and_mounted` | BATCH partition marked corrupt | One erase, init 0, count 0. |
| `c1_init_failure_sets_and_recovery_clears_init_fault` | Page size 2048, then 4096 | `-ENOTSUP` + init fault + storage error bit; then cleared. |
| `c1_flash_device_not_ready_fails_init` | `shim_flash_device.ready = false` | `-ENODEV`. |
| `c1_write_error_sets_write_fault_and_keeps_count` | Write error `-EIO`, append 64; error off, append 64 | `-EIO`, write fault, count 0; then accepted, fault cleared. |
| `c1_read_error_sets_read_fault` | BATCH 64 entries; read error `-EIO` on record 0x100; peek; error off; peek | `-EIO` + read fault; then 0, fault cleared. |
| `c1_delete_error_is_cleanup_only` | SMALL: 4 entries; delete error `-EIO`; drop 4 | Drop 0, retired, delete fault set, count 0. |
| `c1_network_storage_wrapper_validates_byte_lengths` | `network_storage_append_block` with 0, 7, 10 bytes; drop 4 then 7 bytes | `-EINVAL`, `-EINVAL`, 0; count 2, then 1. |
| `c1_power_cut_at_every_write_fixed_batch` | BATCH script: append, append, drop 10, sync, drop 54, append, drop 20, append, append, sync, drop 44 — repeated with the power cut after 0, 1, 2 … N writes, then power on + reboot | Init 0; count = drainable entries; entries uncorrupted and consecutive; first ≤ dropped; last ≥ last successfully appended. |
| `c1_power_cut_at_every_write_contacts` | Same script on CONTACT with 100-entry blocks | Same. **Fails today – finding 1.** |

---

### C2 – ram_log_ring.c

| | |
|---|---|
| What is tested | RAM ring with threshold flush and flash-then-RAM export, against an in-test flash fake. |
| Configuration | Ring 8 entries × 3 B, flush threshold 6, batch 4, retry 5000 ms, storage-full bit `STORAGE_FULL_SELF_REPORT` |
| Input signals | `ram_log_ring_push(value)` (3-byte big-endian value), export begin/commit/abort with a buffer, `shim_run_ready_work()`, `shim_advance_ms()`, fake-flash errors (`append_err_once`, `init_err`, `peek_err`), fake-flash capacity, fake-flash content (`flash_count`) |
| Implementation | `2_Implementation/firmware_unit/tests/test_c2_ram_log_ring.c` |

| Case | Input | Expected result |
|---|---|---|
| `c2_push_then_export_from_ram_in_order` | Push 10, 11, 12; export (64-byte buffer); commit | 9 bytes, values 10…12; count 0. |
| `c2_export_abort_keeps_entries` | Push 1, 2; export; abort; export | Count 2; first value 1. |
| `c2_export_limited_by_buffer_size` | Push 1, 2, 3; export with 8-byte buffer; commit | 6 bytes; count 1. |
| `c2_overflow_without_flush_overwrites_oldest` | Push 0…9 (no work run) | Count 8; values 2…9. |
| `c2_push_dropped_while_full_and_export_active` | Push 8; export; push 100; commit; push 101 | Push 100 = false; push 101 = true; count 1. |
| `c2_flush_moves_one_batch_to_flash_at_threshold` | Push 0…4, run work; push 5, run work | No flash write below 6; then one write of values 0…3; count 6. |
| `c2_export_drains_flash_before_ram` | Push 0…5, run work; export, commit; export | First 0…3 (flash); then 4, 5 (RAM); count 0. |
| `c2_flush_error_retries_after_retry_delay` | Flash append fails once (`-EIO`); push 0…5; run work; advance 4999 ms; advance 1 ms | 1 attempt, nothing in flash; still 1; 2nd attempt succeeds with 4 entries. |
| `c2_enospc_marks_storage_full_and_keeps_ram` | Flash capacity 0; push 0…5; run work; push 50; advance 50 s; capacity back, 1 entry in flash, export + commit (block retired) | Full bit set, RAM kept, no further attempts; full bit cleared after the export. |
| `c2_count_sums_flash_and_ram_and_saturates` | 2 in RAM; flash count 10; flash count 70000 | 12; 65535. |
| `c2_second_export_begin_is_busy` | Push 1; export; export | `-EBUSY`. |
| `c2_commit_without_export_is_rejected` | Commit only | `-EINVAL`. |
| `c2_empty_export_leaves_no_active_reservation` | Export twice on an empty ring; export with 2-byte buffer | 0 bytes each, no `-EBUSY`. |
| `c2_flush_deferred_while_export_active_then_resumes` | Push 0; export 1 entry; push 1…6; run work; commit; run work | No flash write during export; afterwards one write starting at value 1. |
| `c2_push_dropped_when_full_during_flush` | Push 0…5; during the flash write fill the ring to 8 and push once more | Last push dropped (`dropping newest entry` logged); 4 entries in flash. |
| `c2_storage_init_error_fails_export` | Push 1; storage init `-EIO`; export; error off; export | `-EIO`; then 0. |
| `c2_flash_peek_error_aborts_export` | 2 entries in flash; peek `-EIO`; export; error off; export | `-EIO`; then 6 bytes. |
| `c2_sync_delegates_to_storage` | `ram_log_ring_sync()` | Storage sync called once. |

---

### C3 – network.c (contacts, parameters, data level)

| | |
|---|---|
| What is tested | Contact policy, RAM ring + flash flush, export, network parameters and the advertised data level; real `network_storage.c`, `flash_ring_store.c`, `param_storage.c`, `device.c`. |
| Input signals | `network_evaluate_contact(id, rssi dBm, is_location_tag)` (as from radio.c's scan callback), `network_development_validation_append_contact(id, timer, rssi)` (fill), commands `begin/apply(param, value)/commit`, uptime, settings records `dsa/network` (planted), contacts planted in `contact_storage`, export with a 240-byte buffer (48 contacts = one NUS packet) |
| Implementation | `2_Implementation/firmware_unit/tests/test_c3_network.c` |

| Case | Input | Expected result |
|---|---|---|
| `c3_init_with_empty_storage_uses_defaults` | Empty flash/settings; contacts at −80 and −81 dBm | 1 contact. |
| `c3_location_tag_uses_location_threshold` | Location contacts at −100, −101 dBm; normal at −81 | 1 contact. |
| `c3_tracking_off_records_nothing` | `P_TRACKING_ACTIVE=0`, contact −30; `=1`, contact −30 | 0, then 1. |
| `c3_contact_record_encodes_id_uptime_and_rssi_magnitude` | Uptime 0x123456 s + 999 ms; contact id 0xAB at −67 dBm | Exported `AB 12 34 56 43`. |
| `c3_contact_uptime_is_low_24_bits` | Uptime 0x01000005 s; contact −50 | Timer 5. |
| `c3_rssi_command_validation` | `P_RSSI_NETWORK=256`, contact −81; `=90`, contact −90; `P_RSSI_LOCATION=300`, location contact −101 | 0; 1; still 1. |
| `c3_tracking_command_rejects_values_above_1` | `P_TRACKING_ACTIVE=2`; contact −30 | Rejected (logged); contact recorded. |
| `c3_commit_saves_only_when_parameters_change` | `P_RSSI_NETWORK=80` (default); `=70`; commit without begin | 0 saves; 1 save; still 1. |
| `c3_reset_command_restores_defaults` | RSSI 40, tracking 0, then `P_NETWORK_RESET_PARAMS` ; contact −80 | Recorded. |
| `c3_saved_parameters_are_loaded` | Record `{55, 1, 65}`; contacts −56, −55, location −66, −65 | 2 contacts. |
| `c3_v2_record_is_migrated_with_default_location_threshold` | Record `{60, 1}` | −60 and location −100 accepted; re-saved 15-byte record with location 100. |
| `c3_rssi_only_record_is_migrated_with_tracking_enabled` | Record `{45}` | −45 accepted, −46 rejected. |
| `c3_unversioned_legacy_byte_is_migrated` | Raw setting `50` (no header) | −50 accepted, −51 rejected. |
| `c3_corrupt_tracking_value_sets_param_fault` | Record `{55, 2, 65}` | Param fault + param error bit; default threshold applies. |
| `c3_ram_ring_overwrites_oldest_when_no_flush_ran` | 24001 fill contacts, no work run | Count 24000; first exported timer 1. |
| `c3_flush_moves_one_block_to_flash_at_threshold` | 23700 fill contacts; run work | 804 in flash; total 23700. |
| `c3_export_returns_every_contact_once_in_order` | 30000 fill contacts (work every 256); export all | 30000 contacts, timers 0…29999 in order; count 0. |
| `c3_export_abort_keeps_contacts` | 100 contacts; export; abort; commit | Count 100; commit `-EINVAL`. |
| `c3_second_export_begin_is_busy` | 10 contacts; export; export | `-EBUSY`. |
| `c3_contact_dropped_when_ram_full_during_export` | 24000 contacts; export from RAM; contact −30 | Count unchanged; `dropping newest contact` logged. |
| `c3_data_level_thresholds_up_to_level_4` | Fill to 0, 1, 2, 5000, 5001, 40001 contacts; advance 301 ms after each | Levels 0, 1, 2, 2, 3, 4. |
| `c3_data_level_5_reported_above_100000_contacts` | Fill 100001; advance 301 ms | Level 5. **Fails today – finding 2.** |
| `c3_capacity_allows_data_level_7` | Block count × 804 + ring 24000 | 216156 > 210000. |
| `c3_export_drain_lowers_data_level` | Fill 6000; export all | Level 3, then 0. |
| `c3_storage_full_bit_set_when_contacts_are_discarded` | Fill capacity + 2 × 804 contacts | Count < filled; storage-full bit set. **Fails today – finding 3.** |
| `c3_contacts_in_flash_survive_boot` | 50 contacts planted in `contact_storage`; `network_init()` | Count 50. **Fails today – finding 1.** |
| `c3_saved_parameters_loaded_when_contacts_pending_at_boot` | Record `{55, 1, 65}` + 50 planted contacts; `network_init()`; contact −60 | Init 0; −60 rejected (count 50). **Fails today – finding 1.** |

---

### C4 – self_report.c and eco_log.c

| | |
|---|---|
| What is tested | Button debounce/long press, 24-bit timestamps, eco session pairing, flush/export order and the storage-full signal. |
| Input signals | Button edges on pin 20 (`press` = input 1 + interrupt, `release` = input 0 + interrupt), time, `self_report_development_validation_append(t)`, `eco_log_enter()` / `eco_log_leave()` at set uptimes (as called by radio.c on mode transitions), export with a 240-byte buffer, flash page-info error, GPIO configure error |
| Implementation | `2_Implementation/firmware_unit/tests/test_c4_self_report_eco_log.c` |

| Case | Input | Expected result |
|---|---|---|
| `c4_long_press_records_one_self_report` | Uptime 100 s; press; advance 40 + 3000 ms | 1 report, timer 103 s; LED signalled once. |
| `c4_short_press_records_nothing` | Press; advance 1540 ms; release; advance 6040 ms | 0 reports. |
| `c4_bouncing_contact_is_debounced` | 5 × (press, 5 ms, release, 5 ms); press; advance 3040 ms | 1 report. |
| `c4_holding_button_records_only_once` | Press; advance 15040 ms | 1 report. |
| `c4_button_held_at_boot_records_after_long_press` | Input 1 before init; advance 3000 ms | 1 report. |
| `c4_self_report_time_is_low_24_bits` | Append t = 0x01000010 | Stored 0x10. |
| `c4_self_reports_flush_to_flash_and_export_in_order` | Append t = 0…299 (work run after each); export all | 300 reports, values 0…299 in order; count 0. |
| `c4_report_during_full_export_is_not_signalled` | Append 256; export; press 3040 ms | No LED signal. |
| `c4_self_report_storage_full_is_signalled` | Append 712 (work run after each) | Storage-full bit set. **Fails today – finding 3.** |
| `c4_self_report_storage_init_failure_is_reported` | Flash page info `-EIO`; `self_report_init()` | `-EIO`; init fault. |
| `c4_button_gpio_failure_keeps_storage_usable` | GPIO configure error on pin 20; init; append 5 | Init 0; 1 report. |
| `c4_eco_session_records_enter_and_leave_uptime` | Enter at 100 s; leave at 160.5 s | 1 entry (100, 160). |
| `c4_leave_without_enter_records_nothing` | Leave | 0 entries. |
| `c4_second_enter_restarts_session` | Enter 10 s; enter 20 s; leave 30 s; leave | 1 entry, enter 20. |
| `c4_eco_times_are_low_24_bits` | Enter 0x01000001 s; leave 0x01000002 s | (1, 2). |
| `c4_eco_sessions_flush_and_export_in_order` | 100 sessions (2i, 2i+1) s; export all | 100 entries in order. |

---

### C5 – param_storage.c

| | |
|---|---|
| What is tested | Versioned CRC-protected parameter records (magic `0x44534150`, version 1, length, CRC-32/IEEE) and the legacy fallback. |
| Input signals | `param_storage_save/load/load_legacy/delete(key, data, len)`, settings records modified or planted via `fake_settings_put_raw()`, settings error injection |
| Implementation | `2_Implementation/firmware_unit/tests/test_c5_param_storage.c` |

| Case | Input | Expected result |
|---|---|---|
| `c5_settings_init_error_is_propagated` | Settings init `-ENODEV`; save 3 bytes | `-ENODEV`. |
| `c5_crc32_matches_ieee_check_value` | `"123456789"` | 0xCBF43926. |
| `c5_save_then_load_roundtrip` | Save `{7, 1, 99}`; load 3 | Same bytes. |
| `c5_record_layout_is_stable` | Save `{1, 2, 3}`; read raw | 15 bytes: `44 53 41 50`, `00 01`, `00 03`, CRC, payload. |
| `c5_missing_key_is_enoent` | Load unknown key | `-ENOENT`. |
| `c5_length_mismatch_is_ebadmsg` | Saved 3 bytes; load 2, load 4 | `-EBADMSG` both. |
| `c5_corrupted_payload_fails_crc` | Flip one payload bit in the stored record | `-EBADMSG`. |
| `c5_wrong_magic_or_version_is_ebadmsg` | Flip magic byte; version 2 | `-EBADMSG` both. |
| `c5_invalid_arguments_are_rejected` | NULL key; NULL data; len 0; len 65 (load/save); save 64 | `-EINVAL` ×6; 64 accepted. |
| `c5_settings_errors_are_propagated` | Save error `-EIO`; load error `-EIO` | `-EIO` both. |
| `c5_legacy_load_reads_exact_raw_value` | Raw setting `42` (1 byte) | 0, value 42. |
| `c5_legacy_load_rejects_other_lengths` | Raw setting 2 bytes, load 1; unknown key | `-EINVAL`; `-ENOENT`. |
| `c5_delete_removes_record` | Save, delete, load | `-ENOENT`. |

---

### C6 – device.c

| | |
|---|---|
| What is tested | Identity lookup against the real `radio_ids.c` and the advertised status bits. |
| Input signals | Bluetooth addresses (type + 6 bytes), own identity (`shim_bt.identity`), `device_set_storage_fault/full/radio_status_bit/network_status_bits()` calls |
| Implementation | `2_Implementation/firmware_unit/tests/test_c6_device.c` |

| Case | Input | Expected result |
|---|---|---|
| `c6_lookup_known_development_kit_id` | Random `B1 7D 76 1A 92 D1` | 254. |
| `c6_lookup_known_tag_id` | Random `97 57 AF EF E9 F7` | 1. |
| `c6_lookup_every_table_entry_roundtrips` | Every table address | Its own ID. |
| `c6_lookup_unknown_address_returns_0xff` | Random `01 02 03 04 05 06` | 0xFF. |
| `c6_lookup_requires_matching_address_type` | Public `B1 7D 76 1A 92 D1` | 0xFF. |
| `c6_get_device_id_uses_bluetooth_identity` | Identity = tag 1's address | 1. |
| `c6_storage_fault_param_sets_param_error_bit` | Network-params fault on/off | Bit 3 on/off; bit 4 untouched. |
| `c6_every_storage_fault_sets_storage_error_bit` | Each of the 15 storage faults on/off | Bit 4 on/off each time. |
| `c6_storage_error_bit_stays_while_any_fault_remains` | Two faults on, one off, other off | Bit 4 until the last is off. |
| `c6_storage_full_bit_tracks_any_full_domain` | Contact full, eco full, contact clear, eco clear | Bit 2 until both clear. |
| `c6_storage_bits_do_not_touch_radio_bits` | Scan-error + motion bits on; storage bits on/off | Radio status = `0x21`. |
| `c6_status_update_scheduled_only_on_change` | Fault on; second fault (bit already set); both off | 1, 1, 2 update requests. |
| `c6_network_status_bits_respect_mask` | Status 0xFF; set data 0x03; battery 0; eco 0; data 0x13 | 0xF3, 0x83, 0x03, 0x03; 3 update requests. |
| `c6_network_status_field_layout_matches_decision_2026_08_05` | Mask macros | 0x0F / 0x70 / 0x80, shift 4. |

---

### C7 – Base message_parser.c

| | |
|---|---|
| What is tested | The Base's parsed text output per NUS notification. |
| Input signal | `message_parser_feed(beacon_id, notification bytes, len)`; notification = flag + payload as in B (no `"ID"` prefix, no `\r\n`) |
| Implementation | `2_Implementation/firmware_unit/tests/test_c7_base_message_parser.c` |

| Case | Input | Expected result |
|---|---|---|
| `c7_time_package` | ID 5, `01 00 01 02 03` | `ID: 5 Current Timer:66051`. |
| `c7_time_contacts_voltage_package` | ID 48, `05 00 00 10 00 FF FF FF 0B B8` | `ID: 48 Current Timer: 4096 Contact Count: 16777215 Voltage: 3000`. |
| `c7_data_block_with_two_contacts` | ID 1, `02 02` (7, 0x010000, 80) (9, 0xFFFFFF, 45) | Two lines with those values. |
| `c7_data_block_count_mismatch_is_rejected` | `02 02` + one record | No output. |
| `c7_empty_data_block` | `02 00` | No output. |
| `c7_voltage_package` | ID 3, `03 0A 8C` | `ID: 3 VOLTAGE: 2700`. |
| `c7_self_report_block` | ID 4, `06 02 00 00 2A 12 34 56` | Times 42 and 1193046. |
| `c7_eco_log_block` | ID 4, `07 01 00 00 64 00 00 A0` | `ID: 4 Eco session enter: 100 leave: 160`. |
| `c7_eco_log_length_mismatch_is_rejected` | `07 02` + one entry | No output. |
| `c7_finished_control_package_invokes_callback` | ID 2, `04 "finished"` | Line printed; callback once. |
| `c7_other_control_package_is_ignored` | `04 "unknown!"` | No output, no callback. |
| `c7_wrong_fixed_lengths_are_rejected` | TIME 3 bytes, VOLTAGE 3 bytes, TCV 2 bytes, CONTROL 2 bytes | No output. |
| `c7_unknown_flag_is_dumped_as_default_package` | ID 9, `42 01 02 FF` | `ID: 9 DEFAULT uint8=66 1 2 255`. |
| `c7_empty_notification_is_ignored` | Length 0 | No output. |
| `c7_beacon_id_printed_as_decimal_for_all_ids` | IDs 0…255, `03 00 01` | `ID: <id> VOLTAGE: 1`. |
| `c7_maximum_data_block_fits_one_notification` | `02 30` + 48 records (i, i, 60) | 48 lines. |

---

### C8 – motion.c (no accelerometer, like the DK)

| | |
|---|---|
| What is tested | Motion command validation, persistence and the "motion unavailable" bit. Sensor-driven eco transitions are covered by I4. |
| Input signals | `motion_init()`, commands `P_MOTION_ACTIVE` / `P_MOTION_INACTIVITY_TIMEOUT_S` / `P_MOTION_RESET_PARAMS`, settings record `dsa/motion` `{active, timeout BE16}`, time, settings save error |
| Implementation | `2_Implementation/firmware_unit/tests/test_c8_motion.c` |

| Case | Input | Expected result |
|---|---|---|
| `c8_init_without_sensor_reports_motion_unavailable` | `motion_init()` | Bit 5 set. |
| `c8_active_command_accepts_0_and_1_only` | Active 2; active 1 | 0 saves; then saved active 1. |
| `c8_timeout_command_rejects_zero` | Timeout 0; 65535 | 0 saves; then saved 65535. |
| `c8_commit_saves_only_on_change` | Timeout 1800 (default); 60 | 0 saves; 1 save. |
| `c8_reset_restores_defaults` | Timeout 60, then reset | Saved 1800 s, inactive. |
| `c8_saved_parameters_loaded_at_init` | Record `{1, 120}`; command timeout 120 | No fault; no save (unchanged). |
| `c8_invalid_saved_record_sets_param_fault` | Record `{2, 120}`; record `{1, 0}` | Param fault; `-EBADMSG`. |
| `c8_no_eco_without_sensor_even_when_enabled` | Timeout 1 s, active 1; advance 10 s | No eco override, no LED suspend, eco bit 0. |
| `c8_save_failure_sets_param_fault` | Settings save `-EIO`; timeout 77 | Param fault. |

---

### C9 – radio.c

| | |
|---|---|
| What is tested | Advertisement content, accept list, the command path from raw advertising reports, radio parameter validation and persistence, eco scanning. |
| Input signals | Own identity = DK address of ID 254; advertising reports via the captured scan callback as raw AD bytes `[len, 0x09, name…][len, 0xFF, manufacturer…]` with an RSSI; command reports from `"DSZ"` with manufacturer data `[target, param, HI(value), LO(value)]…` (exactly as Control builds them); BLE stack errors; `radio_set_eco_override(bool)` (as motion.c calls it); settings record `dsa/radio2` (17 bytes); time |
| Implementation | `2_Implementation/firmware_unit/tests/test_c9_radio.c` |

| Case | Input | Expected result |
|---|---|---|
| `c9_init_adds_every_known_device_to_accept_list` | `radio_init()` + `radio_start()` | 173 accept-list adds; no scan error. |
| `c9_accept_list_failure_sets_scan_error_but_keeps_advertising` | Accept-list add `-ENOMEM` | Scan-error bit; advertising. |
| `c9_advertisement_carries_name_id_and_status_bytes` | Network status 0x35 before boot | Name `DSA`; manufacturer `[254, radio status, 0x35]`. |
| `c9_default_radio_timing_is_applied` | Boot with production Kconfig | Adv 144/192; scan 11200/192, passive, accept-list filter. |
| `c9_start_fails_when_advertising_fails` | `adv_start_err = -EIO` | `radio_start()` = `-EIO`. |
| `c9_scan_failure_is_degraded_mode` | `scan_start_err = -EIO` | `radio_start()` = 0; scan-error bit. |
| `c9_nus_init_failure_sets_nus_error_bit` | `nus_service_init` → `-EIO` | Bit 1 set. |
| `c9_status_change_updates_advertised_bytes` | Contact write fault after boot; run work | Advertised radio status has bit 4. |
| `c9_status_update_skipped_while_nus_connected` | NUS connected; `adv_update()` | No advertising-data update. |
| `c9_dsa_dst_dsl_advertisements_are_contacts` | `DSA`/`DST`/`DSL` with manufacturer `[7, 0, 0]` at −61/−62/−63 dBm | 3 contacts id 7; only DSL as location tag. |
| `c9_other_names_and_missing_id_are_ignored` | `DSB`, `DSAX`, `DS` with `[7,0,0]`; `DSA` without manufacturer data | No contact. |
| `c9_malformed_ad_structures_do_not_crash` | `10 09 'D' 'S' 'A'` (length beyond end); `00 FF 01 02` | No contact, no crash. |
| `c9_command_for_own_id_is_routed_by_parameter_base` | `DSZ` `[254, 0x21 00 01, 0x64 00 46, 0xA2 02 58]` | LED (0x21, 1), network (0x64, 70), motion (0xA2, 600); each domain begin/commit once. |
| `c9_broadcast_command_is_accepted` | `[0xFF, 0x65 00 5A]` | Network command applied. |
| `c9_command_for_other_id_is_ignored` | `[1, 0x64 00 5A]` | Nothing applied. |
| `c9_command_with_partial_triple_is_rejected` | `[254, 0x64 00 46, 0x65 00]` | Nothing applied. |
| `c9_unknown_parameter_base_is_skipped` | `[254, 0x40 00 01, 0x64 00 46]` | Only the network command. |
| `c9_command_without_manufacturer_data_is_ignored` | `DSZ` without manufacturer data | Nothing applied. |
| `c9_command_queue_holds_four_commands` | 5 command reports before work runs | 4 applied; `Command queue full` logged. |
| `c9_adv_interval_command_applies_and_persists` | `P_ADV_INTERVAL_MS = 1000` | Adv 1600/1600; `dsa/radio2` saved. |
| `c9_adv_interval_bounds` | 19, 20, 10240, 10241 ms | 19 rejected; 32; 0x4000; 10241 rejected. |
| `c9_scan_window_above_interval_is_rejected` | `P_SCAN_WINDOW_MS = 8000` | Window stays 192; not saved. |
| `c9_scan_interval_bounds` | 10241, 10240, 100 ms | Rejected; 0x4000; rejected (below window). |
| `c9_zero_eco_scan_period_is_rejected` | `P_ECO_SCAN_PERIOD_S = 0` | Not saved. |
| `c9_reset_command_restores_default_timing` | Adv 1000 ms, then `P_RADIO_RESET_PARAMS` | Adv 144. |
| `c9_saved_radio_parameters_are_applied_at_boot` | Record: adv 1600/1600, eco 144/192, scan 11200/192, eco window 160, period 300, mode 1 | Adv 1600; no fault. |
| `c9_invalid_saved_radio_parameters_fall_back_to_defaults` | Record all zero, mode 1 | Adv 144; radio-param fault. |
| `c9_motion_eco_override_scans_in_bursts` | Override on; advance 100 ms; advance 299.999 s; advance 1 ms | Burst 160/160; scan stops after 100 ms; next burst exactly after 300 s; 1 eco enter. |
| `c9_eco_session_logged_only_on_real_transitions` | Override on, on, off, off | 1 enter, 1 leave; normal scanning restored. |
| `c9_eco_override_keeps_normal_advertising_interval` | Override on | Adv min 144. |
| `c9_manual_eco_mode_keeps_periodic_scan_bursts` | Command `P_SET_RAD_ACTIVE = 0`; advance 305 s | ≥ 1 new scan start. **Fails today – finding 4.** |
| `c9_manual_eco_mode_can_be_left_by_command` | As above, then `P_SET_RAD_ACTIVE = 1` | Scan running; eco leave logged. **Fails today – finding 4.** |

---

### C10 – led.c

| | |
|---|---|
| What is tested | Status blink timing, self-report flash, firework effect, eco suspend/resume, command validation, persistence and migration. |
| Input signals | `led_init()`, commands `P_MAIN_LED_ACTIVE` / `P_MAIN_LED_INTERVAL_S` / `P_MAIN_FIREWORK_ACTIVE` / `P_MAIN_RESET_PARAMS`, `led_signal_self_report()`, `led_suspend_blinking()` / `led_resume_blinking()` (as from motion.c), settings record `dsa/main` (current and older layouts), time, GPIO configure error, random seed. Outputs observed on pins 10 (status), 11 (self-report), 12 (eco) |
| Implementation | `2_Implementation/firmware_unit/tests/test_c10_led.c` |

| Case | Input | Expected result |
|---|---|---|
| `c10_default_is_no_blinking` | Init; advance 60 s | Status LED never set. |
| `c10_enabled_status_led_blinks_once_per_interval` | Active 1; advance 19.999 s, 1 ms, 30 ms, 20 s | Off, on, off, on. |
| `c10_interval_command_changes_blink_period` | Interval 5, active 1; advance 5 s | On. |
| `c10_command_validation` | Active 2; interval 0; firework 2 | 0 saves. |
| `c10_parameters_persisted_in_v3_layout` | Interval 300 | Stored `{0, 01 2C, 0}`. |
| `c10_saved_parameters_loaded_at_init` | Record `{1, 00 07, 0}`; advance 7 s | On. |
| `c10_v2_record_migrates_keeping_default_firework` | Record `{1, 00 09}` | Re-saved `{1, 00 09, 0}`. |
| `c10_v1_record_migrates_with_default_interval` | Record `{1}` | Re-saved `{1, 00 14, …}` (20 s). |
| `c10_unversioned_legacy_byte_migrates` | Raw setting `1` | Re-saved, active 1. |
| `c10_invalid_saved_record_sets_param_fault` | Record `{1, 00 00, 0}` | LED param fault. |
| `c10_self_report_flash_lasts_configured_time` | `led_signal_self_report()`; advance 1999 ms, 1 ms | On, on, off. |
| `c10_suspend_stops_blinking_and_resume_restarts` | Active 1; suspend; advance 60 s; resume; advance 20 s | Off while suspended, eco LED off; on after resume. |
| `c10_suspend_does_not_persist` | Active 1; suspend; resume | Still 1 save. |
| `c10_firework_flickers_and_stops_cleanly` | Firework 1; advance 5 s; firework 0; advance 5 s | > 20 LED writes; all off; no further writes. |
| `c10_firework_keeps_running_through_eco_suspend` | Firework 1; advance 1 s; suspend; advance 2 s | > 5 further LED writes. |
| `c10_saved_firework_starts_at_boot` | Record `{0, 00 14, 1}`; advance 2 s | > 5 LED writes. |
| `c10_reset_restores_defaults` | Active 1, interval 3, then reset | Stored defaults (inactive, 20 s). |
| `c10_missing_status_led_degrades_gracefully` | Configure error on pin 10; active 1; advance 40 s | No writes to pin 10, no crash. |

---

### C11 – battery_voltage.c

| | |
|---|---|
| What is tested | ADC → mV, mV → status 0–3, status field bits 6..4, periodic measurement. |
| Input signals | ADC raw sample (raw = ⌈mV · 16384 / 3600⌉ for 900 mV reference, gain 1/4, 14 bit), ADC setup/read errors, ADC device ready state, time (4 h period), network status before measurement |
| Implementation | `2_Implementation/firmware_unit/tests/test_c11_battery.c` |

| Case | Input | Expected result |
|---|---|---|
| `c11_status_thresholds` | 3300, 2900, 2899, 2750, 2749, 2550, 2549, 0 mV | 0, 0, 1, 1, 2, 2, 3, 3. |
| `c11_status_fits_three_bit_field` | 0…4000 mV in 10 mV steps | Status ≤ 3, inside mask 0x70. |
| `c11_init_measures_and_publishes_voltage` | Sample for 3000 mV; init | Read 3000 mV; status 0. |
| `c11_low_voltage_sets_status_3_without_touching_other_bits` | Network status 0x85; sample 2500 mV; init | Status 3; byte 0xB5. |
| `c11_read_before_measurement_is_enodata` | Read before init; read with NULL | `-ENODATA`; `-EINVAL`. |
| `c11_adc_not_ready_fails_init` | ADC device not ready | `-ENODEV`. |
| `c11_channel_setup_error_fails_init` | Channel setup `-EIO` | `-EIO`. |
| `c11_periodic_measurement_updates_status` | 3100 mV at init; 2600 mV; advance 4 h − 1 ms, 1 ms | 1 read, then 2; 2600 mV; status 2. |
| `c11_failed_read_keeps_last_value` | 3100 mV; read error; advance 4 h; error off; advance 4 h | 3100 mV kept; 3 reads. |
| `c11_negative_sample_is_rejected` | Raw −5 | No value (`-ENODATA`). |
