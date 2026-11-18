# Class B – Python unit tests (logger, Scan_New_Tags)

No hardware, fully automated, run on every commit
(`pytest 2_Implementation/tests/logger 2_Implementation/tests/scan_tags`). Requirements: Python ≥ 3.10, pytest,
pyserial (B5 imports it; the port itself is faked).

**Input signal format.** The logger's input is the Base's raw UART byte
stream (`CONFIG_DSA_OUTPUT_FORMAT_RAW`), one frame per NUS notification:

```
"ID" | beacon id (1 byte) | flag (1 byte) | payload | "\r\n"
```

| Flag | Payload (big-endian) |
|---|---|
| `0x01` TIME | uptime (4) |
| `0x02` DATA | count (1), count × [id2 (1), timer (3), RSSI magnitude (1)] |
| `0x03` VOLTAGE | mV (2) |
| `0x04` CONTROL | `"finished"` (8) |
| `0x05` TIME_CONTACTS_VOLTAGE | uptime (4), contact count (3), mV (2) |
| `0x06` SELF_REPORT | count (1), count × timer (3) |
| `0x07` ECO_LOG | count (1), count × [enter (3), leave (3)] |
| `0x08` CONNECT_STATUS | status byte (1) |

Frames are built with `2_Implementation/lib/frames.py`. B1 feeds payloads directly to
`parse_payload(flag, payload, raw)`; B2–B6 feed complete frames to
`MessageParser.feed()`.

---

### B1 – Payload decoders with edge values

| | |
|---|---|
| What is tested | `dsa_logger.parse_payload`/`validate_payload` for every flag; the 16-, 24- and 32-bit fields at 0, 1, max−1 and max. |
| Implementation | `2_Implementation/tests/logger/test_b1_payload_decode.py` |
| Automated | ✅ every commit |

| Case | Input | Expected result |
|---|---|---|
| `test_b1_uint16/24/32_decode[<value>]` | Big-endian bytes of 0, 1, max−1, max (e.g. `FF FF FE` for 24 bit) | The value exactly. |
| `test_b1_time[<uptime>]` | Flag `0x01`, uptime ∈ {0, 1, 0xFFFFFFFE, 0xFFFFFFFF} | `Current Time: <uptime>`. |
| `test_b1_time_contacts_voltage[...]` (64 combinations) | Flag `0x05`, uptime × count ∈ {0, 1, 0xFFFFFE, 0xFFFFFF} × mV ∈ {0, 1, 0xFFFE, 0xFFFF} | `Current Timer: …`, `Contact Count: …`, `Voltage: …`. |
| `test_b1_data_contact[...]` | Flag `0x02`, one contact id2 42, timer ∈ 24-bit edges, RSSI magnitude ∈ {0, 1, 127, 128, 255} | `ID2: 42, Timer: <t>, RSSI: -<r>`. |
| `test_b1_data_multiple_contacts_in_order` | 48 contacts (id2 = i, timer = 1000·i, RSSI 40+i) | 48 lines in order. |
| `test_b1_data_empty_batch` | `02 00` | No line. |
| `test_b1_self_report[<t>]` | Flag `0x06`, one timer at 24-bit edges | `Self-report time: <t>`. |
| `test_b1_eco_session[...]` | Flag `0x07`, (enter, leave) ∈ {(0,0), (1,2), (0xFFFFFE,0xFFFFFF), (0xFFFFFF,0)} | `Eco Session Enter: <a>, Leave: <b>`. |
| `test_b1_connect_status[<s>]` | `08 <s>`, s ∈ {0, 1, 0x7F, 0xFF} | `Status: <s>`. |
| `test_b1_finished_control` | `04 "finished"` | `Transfer complete. Disconnecting`. |
| `test_b1_other_control_payload_is_ignored` | `04 "unknown!"` | No line. |
| `test_b1_wrong_length_is_rejected[...]` | TIME with 3 bytes; TCV with 8; DATA count 2 with 1 record; SELF_REPORT count 1 with 2 bytes; ECO count 1 with 5 bytes; CONNECT_STATUS empty; DATA without count | No line. |
| `test_b1_voltage_package_is_decoded` | `03 0B 86` (2950 mV) | `Voltage: 2950`. **Fails today.** |

---

### B2 – All 256 beacon IDs, regression 2e1f52d

| | |
|---|---|
| What is tested | Every ID byte 0–255 is written as its decimal value for ID1 (frame header) and ID2 (contact record); no two IDs produce the same text. |
| Implementation | `2_Implementation/tests/logger/test_b2_ids_full_range.py` |
| Automated | ✅ every commit |

| Case | Input | Expected result |
|---|---|---|
| `test_b2_id1_rendered_as_decimal[0..255]` | Frame `"ID" <id> 08 07 "\r\n"` | `beacon_id == str(id)`. |
| `test_b2_id2_rendered_as_decimal[0..255]` | Frame from beacon 1 with one contact (id2 = value, timer 5, RSSI 50) | `ID2: <value>, Timer: 5, RSSI: -50`. |
| `test_b2_ids_never_collide` | `format_beacon_id(b)` for b = 0…255 | 256 distinct strings. |
| `test_b2_all_ids_in_one_stream` | 256 concatenated status frames, IDs 0…255 (includes `'I'` = 73, `'D'` = 68) | IDs 0…255 in order. |
| `test_regression_2e1f52d_ascii_digit_ids` | Status frames with IDs 48…57 (`'0'`…`'9'`) | `48`…`57`, never `0`…`9`. |

---

### B3 – Stream parser resynchronisation

| | |
|---|---|
| What is tested | `MessageParser.feed` on a complete transfer under arbitrary splitting and corruption. |
| Reference input | One transfer from beacon 7: `08 00` · `05` (123456, 3, 2950) · `02` 3 contacts (1,100,50), (2,101,60), (0x49 `'I'`, 0x444944 `"DID"`, 0x49) · `06` timers 5000, 6000 · `07` (10, 20) · `04 "finished"` |
| Implementation | `2_Implementation/tests/logger/test_b3_stream_parser.py` |
| Automated | ✅ every commit |

| Case | Input | Expected result |
|---|---|---|
| `test_b3_whole_stream` | Reference transfer in one chunk | 11 lines, first `Status: 0`, contact `ID2: 73, Timer: 4475204, RSSI: -73`, last `Transfer complete`. |
| `test_b3_every_split_point` | Reference transfer split into two chunks at every byte offset | Identical to the whole stream each time. |
| `test_b3_byte_by_byte` | Reference transfer, one byte per `feed()` | Identical. |
| `test_b3_garbage_between_frames` | `00 FF "boot banner\r\n"` + transfer + `"network_base: reconnect\r\n"` + transfer | Both transfers complete. |
| `test_b3_text_containing_id_does_not_lose_following_frames` | `"VALID config\r\nIDLE\r\n"` + transfer | Transfer complete. |
| `test_b3_bad_terminator_drops_only_that_frame` | Status frame ending `"XX"` + valid status frame (ID 3, status 1) | Only `Status: 1` from ID 3. |
| `test_b3_unknown_flag_is_skipped` | `"ID" 05 42 01 02 "\r\n"` + valid frame | Only the valid frame. |
| `test_b3_implausible_count_resyncs_immediately` | `"ID" 05 02 3C` + 10 zero bytes (count 60) + valid frame | Only the valid frame. |
| `test_b3_maximum_data_frame` | DATA frame with 48 contacts (242-byte notification) | 48 lines. |
| `test_b3_flush_discards_partial_frame` | First 4 bytes of a frame, `flush()`, the remaining bytes | No output. |
| `test_b3_exclude_option_omits_matching_lines` | `MessageParser(exclude_substrings=["Voltage"])`, TCV frame (1, 2, 3000) | No line containing `Voltage`. **Fails today.** |

---

### B4 – Log line format

| | |
|---|---|
| What is tested | `LogOutput` writes `<YYYY-mm-dd HH:MM:SS>,ID: <id>,<decoded line>` with LF endings to `dsa_YYYYMMDD_HHMM.log`. |
| Reference input | Frames from beacon 48: TCV (10, 1, 3000), DATA (5, 9, 70), `finished` → 5 decoded lines |
| Implementation | `2_Implementation/tests/logger/test_b4_log_output.py` |
| Automated | ✅ every commit |

| Case | Input | Expected result |
|---|---|---|
| `test_b4_file_name_pattern` | `LogOutput(tmp_path)` | `dsa_<8 digits>_<4 digits>.log` in `tmp_path`. |
| `test_b4_line_format` | Reference frames | 5 lines matching the format, ID `48`, LF only. |
| `test_b4_appends_to_existing_file` | Reference frames written twice to the same file | 10 lines. |
| `test_b4_lines_are_flushed_per_message` | Reference frames, file not closed | 5 lines readable on disk. |
| `test_b4_write_without_open_raises` | `write_message()` before `open()` | `RuntimeError`. |
| `test_b4_timestamp_is_wall_clock` | Clock fixed at 2026-08-15 22:00:01 | File `dsa_20260815_2200.log`, lines start `2026-08-15 22:00:01,ID: 48,`. |

---

### B5 – read_tags.py

| | |
|---|---|
| What is tested | Parsing of the Scan_New_Tags firmware output, de-duplication and the CSV, with a fake serial port. |
| Input signal | UART lines `Device,<name>,<AA:BB:CC:DD:EE:FF>[ (random)]\r\n`, delivered by a fake `serial.Serial`; the fake raises `KeyboardInterrupt` when exhausted |
| Implementation | `2_Implementation/tests/scan_tags/test_b5_read_tags.py` |
| Automated | ✅ every commit |

| Case | Input | Expected result |
|---|---|---|
| `test_b5_line_regex` | `Device,Nordic_LBS,D7:D0:FF:94:FF:04 (random)`; `Device,Nordic_LBS,D7:D0:FF` | Name and address extracted; incomplete address rejected. |
| `test_b5_new_devices_written_once` | Same address twice, a second address, an empty line | Header + 2 rows, bytes in `addr1..addr6`. |
| `test_b5_other_lines_ignored` | `00 "garbage"`, `Device,broken`, one valid line | 1 row; `Unrecognized line: Device,broken` printed. |

---

### B6 – Logger robustness during a capture

| | |
|---|---|
| What is tested | `run_logger`/`main` with a scripted input source. |
| Input signal | `ScriptedInput`: returns the listed byte chunks from `read()`, then raises the given exception (default `KeyboardInterrupt` = user stops the capture) |
| Implementation | `2_Implementation/tests/logger/test_b6_logger_robustness.py` |
| Automated | ✅ every commit |

| Case | Input | Expected result |
|---|---|---|
| `test_b6_base_reboot_mid_frame_resyncs` | First 7 bytes of a DATA frame, `00 00 "*** Booting nRF Connect SDK ***\r\n"`, the full frame twice | 2 log lines; input closed. |
| `test_b6_idle_reads_do_not_end_capture` | `b""`, `b""`, frame, `b""`, frame | 2 log lines. |
| `test_b6_capture_across_midnight_keeps_correct_dates` | Clock 2026-08-15 23:59:59 then 2026-08-16 00:00:01; two frames | Lines dated 15th and 16th. |
| `test_b6_lost_port_keeps_already_written_lines` | Two frames, then `OSError` | 2 lines on disk, `OSError` raised, input closed. |
| `test_b6_lost_port_is_reported_as_error` | `main([])` with one frame then `OSError` | Return code 1. **Fails today:** unhandled `OSError`. |
| `test_b6_disk_write_failure_is_not_silent` | One frame; `write_message` raises `OSError(28, "No space left on device")` | `OSError` raised, input closed. |
