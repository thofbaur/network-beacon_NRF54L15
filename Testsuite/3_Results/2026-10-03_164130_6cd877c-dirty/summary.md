# Summary 2026-10-03_164130_6cd877c-dirty

| Passed | Failed | Errors | Skipped |
|---|---|---|---|
| 1254 | 16 | 0 | 45 |

Exit status: 1

## Coverage

| Code | Lines | Branches | Functions |
|---|---|---|---|
| Firmware C (host tests) | 86.6 % | 73.9 % | 94.1 % |
| Python (statements + branches) | 83.6 % | | |

Per file: `coverage/coverage.md`

## Failed

Compare with `3_Results/Findings.md`.

- `2_Implementation/tests/contract/test_d1_frame_contract.py::test_d1_voltage_flag_decoded_by_both` E         Use -v to get more diff
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c1_flash_ring_store::c1_contacts_survive_reboot]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c1_... wipes the contacts and the following metadata write fail
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c1_flash_ring_store::c1_incompatible_format_is_cleared_and_store_usable_in_same_boot]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c1_...ing metadata write returns -EACCES\nFAIL c1_incompatible_
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c1_flash_ring_store::c1_power_cut_at_every_write_contacts]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c1_...st_c1_flash_ring_store.c:697: cut after 3 writes: init fa
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c3_network::c3_data_level_5_reported_above_100000_contacts]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c3_... 5-7 (100000/150000/210000) can never be advertised\nFAIL
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c3_network::c3_storage_full_bit_set_when_contacts_are_discarded]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c3_...e discards silently and never returns -ENOSPC)\nFAIL c3_s
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c3_network::c3_contacts_in_flash_survive_boot]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c3_....c:478: 50 contacts in flash before boot, 0 after network
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c3_network::c3_saved_parameters_loaded_when_contacts_pending_at_boot]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c3_...SSI threshold 55 is ignored for this boot\nFAIL c3_saved_
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c4_self_report_eco_log::c4_self_report_storage_full_is_signalled]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c4_...ded (456 kept of 712) but the storage-full bit is not set
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c9_radio::c9_manual_eco_mode_keeps_periodic_scan_bursts]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c9_...ceive commands (the mode is persisted across reboot)\nFAI
- `2_Implementation/tests/firmware_unit/test_c_firmware_unit.py::test_c[c9_radio::c9_manual_eco_mode_can_be_left_by_command]` E        +  where 1 = CompletedProcess(args=['D:\\Users\\Tobias\\Documents\\DSA\\BeaconNRF54\\Testsuite\\.build\\firmware_unit_coverage\\c9_...o mode - a P_SET_RAD_ACTIVE=1 command cannot be received\
- `2_Implementation/tests/logger/test_b1_payload_decode.py::test_b1_voltage_package_is_decoded` E         Use -v to get more diff
- `2_Implementation/tests/logger/test_b3_stream_parser.py::test_b3_exclude_option_omits_matching_lines` E        +  where True = any(<generator object test_b3_exclude_option_omits_matching_lines.<locals>.<genexpr> at 0x0000022CB7F3DFF0>)
- `2_Implementation/tests/logger/test_b6_logger_robustness.py::test_b6_lost_port_is_reported_as_error` E           Failed: main() crashed with an unhandled OSError instead of returning 1
- `2_Implementation/tests/static/test_a3_generate_ids.py::test_a3_read_tags_csv_is_accepted_by_generate_ids` E           Failed: generate_ids.py needs column 'physical_id' but read_tags.py writes only ['name', 'addr1', 'addr2', 'addr3', 'addr4', 'addr5', 'addr6']: the documented workflow requires a manual st
- `2_Implementation/tests/static/test_a3_generate_ids.py::test_a3_howto_matches_generate_ids_cli` E       assert not ["HowTo.md documents '--start-id', generate_ids.py has no such option", 'HowTo.md says IDs are assigned sequentially, the script uses physical_id']
