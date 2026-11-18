# Class D – Contract tests between implementations

No hardware, fully automated, run on every commit (`pytest 2_Implementation/tests/contract`).

---

### D1 – Base and logger decode the same frames to the same values

| | |
|---|---|
| What is tested | The Base's `message_parser.c` (host build, `2_Implementation/firmware_unit/tools/d1_base_parser_dump.c`) and `dsa_logger.py` decode an identical, generated set of NUS notifications to identical values. Both outputs are normalised to tuples such as `("contact", id2, timer, rssi)` because the two print different text. |
| Input signal | Each notification is fed to the Base dump tool as one stdin line `<beacon id> <hex bytes of flag+payload>` and to the logger as the raw UART frame `"ID" <id> <flag+payload> "\r\n"` (format in `B_logger.md`). |
| Requirements | Python, pytest, C compiler (as class C) |
| Automated | ✅ every commit |
| Implementation | `2_Implementation/tests/contract/test_d1_frame_contract.py` |

| Case | Input | Expected result |
|---|---|---|
| `time-<v>` (beacon 1) | `01` + uptime v ∈ {0, 1, 0xFFFFFFFE, 0xFFFFFFFF} | Equal `("time", v)`. |
| `tcv-<u>-<c>-<mv>` (beacon 2) | `05` + uptime ∈ {0, 0xFFFFFFFF} × count ∈ 24-bit edges × mV ∈ 16-bit edges | Equal timer, count, voltage. |
| `contact-t<t>-r<r>` (beacon 3) | `02 01` + (7, t, r), t ∈ 24-bit edges, r ∈ {0, 1, 128, 255} | Equal contact tuple. |
| `contact-max-batch` | `02 30` + 48 records (i, 997·i, i) | 48 equal tuples. |
| `contact-empty-batch` | `02 00` | Both empty. |
| `id1-<id>` (id 0…255) | Beacon id = id, `02 01` + (id, id, 60) | Equal tuples; the Base prints the same beacon ID. |
| `self-<t>` (beacon 4) | `06 02` + timers t and 0xFFFFFF − t | Equal. |
| `self-max-batch` | `06 50` + timers 0…79 | 80 equal tuples. |
| `eco-<a>-<b>` (beacon 5) | `07 01` + (a, b) ∈ {(0,0), (1,2), (0xFFFFFE, 0xFFFFFF)} | Equal. |
| `eco-max-batch` | `07 28` + 40 sessions (i, i+1) | 40 equal tuples. |
| `finished` (beacon 6) | `04 "finished"` | Both `("finished",)`. |
| `test_d1_voltage_flag_decoded_by_both` | Beacon 6, `03 0B 86` (2950 mV) | Both `("voltage", 2950)`. **Fails today:** the logger drops `0x03`. |

`CONNECT_STATUS` (`0x08`) is not part of the set: the Base synthesises it and
never passes it through `message_parser.c`.

---

### D2 – Beacon encoder round trip through both decoders

| | |
|---|---|
| What is tested | Packets built by the beacon's own encoder decode identically in Base and logger. |
| Input signal | Generated contacts, self-reports and eco sessions at 24-bit edge values, encoded by the beacon's packet builders. |
| Requirements | **Prerequisite:** the NUS packet builders of `Network_Beacon_nrf54/src/nus.c` moved into pure functions (`nus_packets.h/.c`). |
| Automated | ✅ once the prerequisite exists; skipped with `PREREQUISITE MISSING` until then |
| Implementation | `2_Implementation/tests/contract/test_d2_encoder_roundtrip.py` |
| Expected result | Both decoders return the input values. |

---

### D3 – Control tool encodes commands the way the beacon decodes them

| | |
|---|---|
| What is tested | `Network_Control_nrf54/src/main.c` statically; the beacon side of the same byte layout is exercised in C9. |
| Inputs | `Network_Control_nrf54/src/main.c` (`HI`/`LO` macros, `CMD_TARGET`, `DEVICE_NAME`, active entries of `mfg_data[]`), `Network_Beacon_nrf54/src/radio.c`, `shared/common_include.h`, `radio_ids.c`, `Network_Control_nrf54/Kconfig` |
| Requirements | Python, pytest |
| Automated | ✅ every commit |
| Implementation | `2_Implementation/tests/contract/test_d3_command_encoding.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_d3_value_byte_order_is_big_endian` | `HI(v)`, `LO(v)` definitions; `sys_get_be16(&data[offset + 1])` in radio.c | HI = high byte, LO = low byte, beacon reads big-endian. |
| `test_d3_triples_are_parameter_hi_lo` | Active `P_x, HI(a), LO(b)` entries | a == b for every triple. |
| `test_d3_device_name_triggers_command_handling` | `DEVICE_NAME`; beacon `target_devices[]` | `"DSZ"` → `ACTION_DSZ`. |
| `test_d3_target_is_known_device_or_broadcast` | `CMD_TARGET` (today 0x86 = 134); IDs of `radio_ids.c` | 0xFF or a known ID. |
| `test_d3_parameters_exist_in_protocol_header` | All `P_*` names in Control (comments removed) | All defined in `common_include.h`. |
| `test_d3_active_command_values_are_accepted_by_beacon` | Active entries (today `P_SET_RAD_ACTIVE=1`, `P_MOTION_ACTIVE=0`) | Booleans 0/1, periods ≠ 0, RSSI ≤ 255, intervals inside BLE ranges. |
| `test_d3_commander_uart_option_exists` | `Network_Control_nrf54/Kconfig` | `DSA_CONTROL_UART_COMMANDS` defined and used. Skipped with `PREREQUISITE MISSING`. |
