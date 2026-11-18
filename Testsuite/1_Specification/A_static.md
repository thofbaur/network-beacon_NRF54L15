# Class A – Static checks

No hardware, fully automated, run on every commit (`pytest 2_Implementation/tests/static`).
Requirements for all A tests: Python ≥ 3.10, pytest; A4 also needs `git`.

Inputs of static checks are source and configuration files of the
repository, read as they are at test time.

---

### A1 – Protocol constants agree across all implementations

| | |
|---|---|
| What is tested | The NUS protocol constants are identical in `shared/common_include.h` (beacon and Base), the Base parser `Network_Base_nrf54/src/message_parser.c`, the beacon entry sizes (`network.h`, `self_report.h`, `eco_log.h`) and `Network_Log+Postprocessing/dsa_logger.py`. |
| Inputs | `shared/common_include.h`, `Network_Base_nrf54/src/message_parser.c`, `Network_Base_nrf54/prj.conf`, `Network_Beacon_nrf54/src/{network.h, self_report.h, eco_log.h, nus.c}`, module `dsa_logger.py` (imported) |
| Requirements | Python, pytest |
| Automated | ✅ every commit |
| Implementation | `2_Implementation/tests/static/test_a1_protocol_constants.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_a1_flag_values_match_logger[<flag>]` (8 flags) | `#define DSA_NUS_FLAG_*` from the header; logger constant of the same name and `DSA_NUS_FLAGS` table | Each value in the header equals the logger constant, and the logger's name table maps the value back to that name. |
| `test_a1_logger_knows_no_extra_flags` | Set of the 8 header flag values; keys of `DSA_NUS_FLAGS` | The logger knows exactly these 8 flags. |
| `test_a1_flags_are_unique` | The 8 header flag values | No two flags share a value. |
| `test_a1_ack_magic_cannot_collide` | `DSA_NUS_ACK_MAGIC`; flag values; characters `'s'`, `'t'` | Magic is `0xAC`, differs from every flag and from the `"st"` start command. |
| `test_a1_fixed_payload_lengths_match_base[...]` | Logger `DSA_NUS_PAYLOAD_LENGTHS`; Base `DSA_TIME_LEN`, `DSA_VOLTAGE_LEN`, `DSA_CONTROL_LEN`, `DSA_TIME_CONTACT_VOLTAGE_LEN` | TIME 4, VOLTAGE 2, CONTROL 8, TIME_CONTACTS_VOLTAGE 9 in both. |
| `test_a1_connect_status_payload_is_one_byte` | Logger length for `DSA_NUS_FLAG_CONNECT_STATUS` | 1. |
| `test_a1_entry_sizes_agree[...]` | Logger `DSA_DATA_SET_LEN` / `DSA_SELF_REPORT_ENTRY_LEN` / `DSA_ECO_LOG_ENTRY_LEN`; Base `*_SET_LEN`; beacon `NETWORK_CONTACT_ENTRY_SIZE` / `SELF_REPORT_ENTRY_SIZE` / `ECO_LOG_ENTRY_SIZE` | 5 / 3 / 6 bytes, identical in all three. |
| `test_a1_logger_frame_limit_matches_base_mtu` | Logger `MAX_NUS_RAW_PAYLOAD_LEN`; `CONFIG_BT_L2CAP_TX_MTU` in Base `prj.conf` | Both 247. |
| `test_a1_finished_marker_matches` | Text of beacon `nus.c` and Base `message_parser.c`; logger `DSA_CONTROL_FINISHED_PAYLOAD` | All contain / use `"finished"`. |
| `test_a1_status_byte_fields_are_disjoint` | `DATA_LEVEL_MASK`, `BATTERY_LEVEL_MASK`, `ECO_MODE_MASK` | `0x0F`, `0x70`, `0x80`, no overlap (DECISIONS 2026-08-05). |

---

### A2 – Known-device table is consistent

| | |
|---|---|
| What is tested | `Network_Beacon_nrf54/src/radio_ids.c` (`known_device_table`): uniqueness, ID ranges, address validity and the controller accept-list limit. |
| Inputs | `Network_Beacon_nrf54/src/radio_ids.c` (parsed entries: address type, 6 address bytes, ID), `Network_Beacon_nrf54/prj.conf` (`CONFIG_BT_CTLR_FAL_SIZE`) |
| Requirements | Python, pytest |
| Automated | ✅ every commit |
| Implementation | `2_Implementation/tests/static/test_a2_radio_ids_table.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_a2_table_parsed_completely` | Parsed entries; number of `.id =` occurrences in the file | Equal counts (guards the parser). |
| `test_a2_every_address_has_six_bytes` | `.val = { … }` of every entry | Exactly 6 bytes each. |
| `test_a2_no_duplicate_addresses` | (type, address) of every entry | No pair appears twice. |
| `test_a2_no_duplicate_ids` | `.id` of every entry | No ID appears twice. |
| `test_a2_ids_in_fielded_or_development_range` | `.id` of every entry | Every ID in 1–170 or 252–254. |
| `test_a2_random_addresses_are_static_random` | `val[5]` of every `BT_ADDR_LE_RANDOM` entry | Two MSBs set (`val[5] & 0xC0 == 0xC0`). |
| `test_a2_table_fits_controller_accept_list` | Number of entries; `CONFIG_BT_CTLR_FAL_SIZE` | Entries ≤ 255. |
| `test_a2_development_kits_present` | Set of IDs | Contains 252, 253, 254. |

---

### A3 – ID generator and Scan_New_Tags workflow

| | |
|---|---|
| What is tested | `Scan_New_Tags/generate_ids.py` output (byte order, IDs, duplicate report) and that the documented workflow `read_tags.py` → `generate_ids.py` (`HowTo.md`) works as written. |
| Inputs | CSV files `tag_adresses_*.csv` written by the test into a temporary directory (columns `name, addr1..addr6, physical_id` unless stated); `Scan_New_Tags/HowTo.md`; `generate_ids.py` source |
| Requirements | Python, pytest (scripts run against a temporary directory) |
| Automated | ✅ every commit |
| Implementation | `2_Implementation/tests/static/test_a3_generate_ids.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_a3_address_bytes_are_reversed` | One row `Nordic_LBS, F7,E9,EF,AF,57,97, 1` | `ids.c` contains `.val = { 0x97, 0x57, 0xAF, 0xEF, 0xE9, 0xF7 }`, type `BT_ADDR_LE_RANDOM`. |
| `test_a3_ids_come_from_physical_id` | Rows with `physical_id` 17 and 5 | Blocks with `.id = 17`, `.id = 5`, in CSV order. |
| `test_a3_rows_from_several_sessions_are_combined` | Two CSV files, one row each | Two blocks. |
| `test_a3_duplicates_are_reported` | Two rows with the same address `C1:00:00:00:00:01` and the same `physical_id` 7 | Console output contains `Duplicate physical_id` and `Duplicate addresses`. |
| `test_a3_output_is_valid_c_table_syntax` | Row with lower-case bytes `c1,aa,bb,cc,dd,ee`, ID 3 | `.val = { 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0xC1 }` (upper-case hex). |
| `test_a3_read_tags_csv_is_accepted_by_generate_ids` | CSV with exactly the header `read_tags.CSV_HEADER` (no `physical_id`) and one row | `generate_ids.py` runs without error. **Fails today:** `KeyError: 'physical_id'`. |
| `test_a3_howto_matches_generate_ids_cli` | Text of `HowTo.md` and `generate_ids.py` | No `--start-id` documented unless the script has it; no "assigned sequentially" when the script uses `physical_id`. **Fails today.** |

---

### A4 – Flash partition layout and update compatibility

| | |
|---|---|
| What is tested | `Network_Beacon_nrf54/pm_static.yml`: no overlaps, consistent spans, storage sizes equal to Kconfig, storage partitions unchanged since the last release tag (so an update keeps stored data readable). |
| Inputs | `Network_Beacon_nrf54/pm_static.yml` (working tree), `pm_static.yml` of the newest `v*` tag (`git show`), `dsa.conf`, `prj.conf` |
| Requirements | Python, pytest, git (release tags `v*`) |
| Automated | ✅ every commit |
| Implementation | `2_Implementation/tests/static/test_a4_partitions.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_a4_leaf_partitions_do_not_overlap` | Address/size of all partitions without `span` | No two overlap. |
| `test_a4_spans_cover_their_children_exactly` | Span partitions and their children | Span starts at the first child, ends at the last, no gaps. |
| `test_a4_storage_is_sector_aligned_and_below_ceiling` | `contact_storage`, `self_report_storage`, `settings_storage`, `eco_log_storage` | 4 KiB aligned, ≥ 2 sectors, end ≤ `0x165000`. |
| `test_a4_partition_sizes_match_kconfig[...]` | Partition size; `CONFIG_DSA_NETWORK/SELF_REPORT/ECO_LOG_FLASH_SIZE_BYTES` in `dsa.conf` | Equal. |
| `test_a4_settings_partition_matches_prj_conf` | `settings_storage` size; `CONFIG_PM_PARTITION_SIZE_SETTINGS_STORAGE` | Equal (`0x2000`). |
| `test_a4_storage_partition_unchanged_since_last_release[...]` | Address/size per storage partition now and in the newest `v*` tag (line endings normalised) | Identical. Skipped when no tag exists. |

---

### A5 – Decision traceability

| | |
|---|---|
| What is tested | Every `##` decision in `Network_Beacon_nrf54/DECISIONS.md` is listed in `Testsuite/TRACEABILITY.md` with at least one test ID, and every referenced test ID is specified in ``. |
| Inputs | `## ` headings of `DECISIONS.md`; table rows of `TRACEABILITY.md`; `### <ID>` headings in `*.md` |
| Requirements | Python, pytest |
| Automated | ✅ every commit – a new decision without a test fails the suite |
| Implementation | `2_Implementation/tests/static/test_a5_traceability.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_a5_decision_is_traced_to_tests[<decision>]` | One decision heading | Appears verbatim in `TRACEABILITY.md` with ≥ 1 test ID. |
| `test_a5_traced_test_ids_are_specified` | All test IDs in `TRACEABILITY.md` | Each has a `### <ID>` section in ``. |
| `test_a5_no_stale_decisions_in_traceability` | Decision column of `TRACEABILITY.md` | Every entry still exists in `DECISIONS.md`. |
