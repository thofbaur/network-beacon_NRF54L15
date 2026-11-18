# Class H – Hardware tests with DK A and DK B

Run with `pytest --hil 2_Implementation/tests/hil/test_h_two_dk.py` (`--slow` for H8, H10).
Requirements for all H tests: DK A and DK B over USB, `0_Environment/boards.toml`
with DK B's VCOM port, J-Link + `pylink-square`, nRF Connect SDK toolchain,
`pyserial`; most also the computer's Bluetooth adapter. DK A runs the hil
beacon build (500 synthetic contacts, RTT console) and is reset before each
test. DK B is reprogrammed per test:

| DK B role | Image | Input signal DK B produces |
|---|---|---|
| Commander | `Network_Control_nrf54` + `2_Implementation/configs/control_uart_commands.conf` (**prerequisite**) | `"DSZ"` command advertisements, manufacturer data `[target, param, HI(value), LO(value)]`, for a given duration |
| Beacon partner | `Network_Beacon_nrf54` release config (ID from `boards.toml`, e.g. 253), or + `2_Implementation/configs/beacon_as_location_tag.conf` (name `DSL`) | Contact advertisements `DSA`/`DSL` with manufacturer `[id, status, status]` every 90–120 ms |
| Base | `Network_Base_nrf54` + `2_Implementation/configs/base_hil.conf`, `CONFIG_DSA_TARGET_BEACON_ID` = DK A | Connection, `"st"`, acks to DK A; raw frames on its UART |
| Scanner | `Scan_New_Tags` | Passive scan; `Device,<name>,<addr>` lines on its UART |

#### Commander UART protocol (required from the Control firmware addition)

Input line on DK B's VCOM, 115200 baud:
`CMD <target> <param> <value> [<duration_ms>]\n` (decimal or `0x` hex).
Control advertises `"DSZ"` with manufacturer data `[target, param, HI(value),
LO(value)]` for `duration_ms` (default 3000) using the same code path as the
button-triggered table, then answers `OK` (or `ERR <reason>`).

Parameter values used below (from `shared/common_include.h`): `0x21`
LED active, `0x22` LED interval, `0x64` RSSI network, `0x65` RSSI location,
`0x6C` network reset, `0x6D` tracking active, `0x87` eco scan period,
`0x8C` radio reset, `0x8D` radio active, `0xA2` motion inactivity timeout.

---

### H1 – Every command applied, invalid values rejected

| | |
|---|---|
| Requirements | **Prerequisite:** Commander option; DK A RTT (hil.conf) |
| Implementation | `test_h1_command_applied_and_invalid_rejected[<param>]`, `test_h1_broadcast_and_foreign_target`, `test_h1_parameters_persist_across_reset` |

| Case | Input (Commander lines; target = DK A's ID unless stated) | Expected result on DK A |
|---|---|---|
| `0x21` LED active | `CMD a 0x21 1`; then `CMD a 0x21 2` | RTT `Status LED enabled`; then `Rejecting`. |
| `0x22` LED interval | `CMD a 0x22 5`; then `… 0` | `Status LED interval set to 5 s`; then `Rejecting`. |
| `0x64` RSSI network | `CMD a 0x64 70`; then `… 256` | `Network RSSI threshold set to -70 dBm`; then `Rejecting`. |
| `0x65` RSSI location | `CMD a 0x65 90`; then `… 300` | `Location RSSI threshold set to -90 dBm`; then `Rejecting`. |
| `0x6D` tracking | `CMD a 0x6D 0`; then `… 2` | `Network contact tracking disabled`; then `Rejecting`. |
| `0xA2` motion timeout | `CMD a 0xA2 60`; then `… 0` | `Inactivity timeout set to 60 s`; then `Rejecting`. |
| broadcast / foreign target | `CMD 255 0x64 71`; `CMD a+1 0x64 72` | `… -71 dBm`; then `Ignoring command for target`. |
| persistence | `CMD a 0x64 66`; J-Link reset of DK A; `CMD a 0x6C 0` | `-66 dBm`; after reset no `No stored network parameters`; `Network parameters reset`. |

### H2 – Manual eco mode round trip

| | |
|---|---|
| Requirements | **Prerequisite:** Commander; Bluetooth adapter |
| Input | `CMD a 0x87 10` (burst period 10 s); `CMD a 0x8D 0` (eco); wait 5 s; `CMD a 0x8D 1 15000` (advertised for 15 s); BLE readout from the computer; `CMD a 0x8C 0` (cleanup) |
| Implementation | `test_h2_manual_eco_mode_round_trip` |
| Expected result | RTT `Command parameter 0x8d value 1` within 5 s; readout contains exactly 1 eco session with 0 < leave − enter < 120 s. **Expected to fail today – finding 4.** |

### H3 – Contact detection with a partner beacon

| Case | Input | Expected result |
|---|---|---|
| `test_h3_contacts_with_partner_recorded` | DK B = beacon partner at ~1 m; drain DK A (readout); wait 30 s; readout | ≥ 1 contact with ID2 = DK B, RSSI −100…−10 dBm. |
| `test_h3_thresholds_and_tracking` | DK B = Commander: `CMD a 0x64 0`; DK B = partner; drain; wait 30 s; readout; DK B = Commander: `CMD a 0x6C 0` | No contact with DK B. |

### H4 – Location tag uses the location threshold

| | |
|---|---|
| Input | DK B = Commander: `CMD a 0x64 0`, `CMD a 0x65 255`; DK B = partner advertising as `DSL`; drain; wait 30 s; readout; Commander: `CMD a 0x6C 0` |
| Implementation | `test_h4_location_tag_uses_location_threshold` |
| Expected result | Contacts with ID2 = DK B are recorded. |

### H5 – Production chain through the Base

| Case | Input | Expected result |
|---|---|---|
| `test_h5_production_chain_through_base` | DK B = Base targeting DK A; DK B UART captured and decoded with `dsa_logger` until `finished` (≤ 180 s) | Contacts 1–500 exactly once; `Contact Count: 500`; 10 self-report lines. |
| `test_h5_production_chain_with_id_48` | DK A image with `CONFIG_DSA_DEV_FORCE_ID=48` (**prerequisite**); Base targeting 48 | Every message has beacon ID `48`; 500 contacts. |

### H6 – Base recovers from a beacon reset mid-transfer

| | |
|---|---|
| Input | DK B = Base targeting DK A; as soon as the first DATA frame appears on the Base UART: J-Link reset of DK A; capture until the next `finished` (≤ 240 s) |
| Implementation | `test_h6_base_recovers_from_beacon_reset_mid_transfer` |
| Expected result | The later transfer contains contacts 1–500 of the new boot, complete. |

### H7 – Scan_New_Tags workflow

| | |
|---|---|
| Input | DK A = Nordic `peripheral_lbs` sample (advertises `Nordic_LBS`), DK B = Scan_New_Tags; reset DK A; scanner UART up to 60 s; CSV with the scanned address + `physical_id` 200 fed to `generate_ids.py` |
| Implementation | `test_h7_scan_new_tags_workflow` |
| Expected result | Scanner line parsed by `read_tags.LINE_RE`; `ids.c` contains the address byte-reversed. |

### H8 – Contacts arriving during readouts (slow)

| | |
|---|---|
| Input | DK B = partner advertising continuously; 20 × (BLE readout of DK A, wait 5 s) |
| Implementation | `test_h8_contacts_during_readouts` |
| Expected result | No corrupted record, no undecodable notification, ≤ 48 synthetic contacts delivered twice. |

### H9 – Base with several beacons

| | |
|---|---|
| Input | DK A (hil build) + tags from `boards.toml [tags]` (hil builds) in range; DK B = Base with target 255 (any); Base UART captured up to 600 s per beacon |
| Requirements | Tags |
| Implementation | `test_h9_base_reads_several_beacons` |
| Expected result | A completed transfer with contacts from DK A and every listed tag. |

### H10 – Contact detection rate (slow)

| | |
|---|---|
| Input | DK B = partner at ~1 m; drain DK A; wait 10 min; readout |
| Implementation | `test_h10_contact_detection_rate` |
| Expected result | 3–30 contacts/min with DK B (≈ 8.6 expected: 120 ms scan window every 7 s); rate printed. |
