# Class G – Hardware tests with DK A and the computer's Bluetooth adapter

Run with `pytest --hil 2_Implementation/tests/hil/test_g_ble.py` (`--slow` for G7, G8,
G10, G11). Requirements for all G tests: DK A over USB, `0_Environment/boards.toml`,
J-Link software + `pylink-square`, nRF Connect SDK toolchain, a Bluetooth
adapter usable by `bleak` (Windows, macOS, Linux).

**Setup.** DK A runs `Network_Beacon_nrf54` + `dsa_dev.conf` +
`2_Implementation/configs/hil.conf` and is reset before each test. After every reset it holds
500 synthetic contacts in RAM (contact *i*: id = *i* & 0xFF, timer = *i*,
RSSI 30–99) and 10 self-reports.

**Input signals from the computer (BLE central, `2_Implementation/hil/ble.py`):**

| Signal | Bytes | Characteristic |
|---|---|---|
| Connect / disconnect | – | – |
| Enable notifications | CCCD write | NUS TX `6e400003-…` |
| Start command | `73 74` (`"st"`) | NUS RX `6e400002-…` (write with response) |
| Acknowledgement | `AC <flag> <count>` – flag and count of the batch just received | NUS RX |
| Wrong acknowledgement | as specified per case | NUS RX |

Default ack policy: every DATA/SELF_REPORT/ECO_LOG batch is acknowledged
immediately. Other inputs: J-Link resets, firmware images, waiting times.
Beacons are identified by the ID byte in their manufacturer data (macOS
hides addresses).

---

### G1 – Advertisement content

| | |
|---|---|
| Input | Reset DK A; passive BLE scan until DK A's ID is seen (≤ 20 s) |
| Implementation | `test_g1_advertisement_content` |
| Expected result | Name `DSA`; ID = DK A; data level 2 (500 contacts); battery field 0–3; eco bit 0; motion-unavailable bit set; bits 0–4 clear. |

### G2 – Full readout of synthetic data

| | |
|---|---|
| Input | Connect, notifications on, `"st"`, ack every batch |
| Implementation | `test_g2_full_readout_of_synthetic_data` |
| Expected result | `finished`; TCV count 500; contacts 1–500 exactly once and uncorrupted; 10 self-reports; no eco sessions; no undecodable notification. |

### G3 – Readout drains the store

| | |
|---|---|
| Input | Full readout; wait 2 s; second full readout; wait 1 s; scan |
| Implementation | `test_g3_readout_drains_the_store` |
| Expected result | Second readout count 0, no data; advertised data level 0. |

### G4 – Interrupted readout loses nothing

| Case | Input | Expected result |
|---|---|---|
| `test_g4_unacknowledged_batch_is_resent` | Readout without the ack of the first DATA batch (beacon waits 2 s); then a normal readout | First readout ≤ 48 contacts; second contains all 500. |
| `test_g4_disconnect_mid_transfer_loses_nothing` | Readout, disconnect after 3 acknowledged batches; wait 6 s; normal readout | Union of both = timers 1–500; ≤ 48 delivered twice. |

### G5 – Handshake edge cases

| Case | Input | Expected result |
|---|---|---|
| `test_g5_start_before_notifications_is_ignored` | Connect, `"st"` **before** enabling notifications, wait 8 s; then normal readout | No notification in the first connection; second readout 500 contacts. |
| `test_g5_second_start_command_is_ignored` | Notifications on, `"st"`, `"st"` | One TCV packet; contacts complete. |
| `test_g5_wrong_ack_does_not_commit[wrong-magic]` | First DATA batch acked with `AB 02 <count>` | Next readout still contains all 500. |
| `…[wrong-domain]` | `AC 07 <count>` | Same. |
| `…[wrong-count]` | `AC 02 <count+1>` | Same. |
| `…[too-short]` | `AC 02` | Same. |

### G6 – Idle connection timeout

| | |
|---|---|
| Input | Connect, notifications on, send nothing, wait up to 15 s |
| Implementation | `test_g6_idle_connection_times_out` |
| Expected result | Beacon disconnects after 3.5–8 s (`CONFIG_DSA_NUS_IDLE_TIMEOUT_MS` = 5000). |

### G7 – Reset during flash writes (slow)

| | |
|---|---|
| Input | Image + `hil_flash.conf` (30000 contacts per boot → flash flushes), flashed with `--erase`; 20 J-Link resets at random intervals of 0.5–6 s (seed 7); final reset, wait for fill + 10 s; full readout (≤ 600 s) |
| Implementation | `test_g7_reset_during_flash_write` |
| Expected result | All records uncorrupted (id = timer & 0xFF); TCV count = delivered contacts; timers 1–30000 of the last boot complete. |

### G8 – Flushed contacts survive a reset (slow)

| | |
|---|---|
| Input | `hil_flash` image with `--erase`; boot, wait 10 s (6432 contacts flushed); reset; boot, wait 10 s; full readout |
| Implementation | `test_g8_flushed_contacts_survive_reset` |
| Expected result | 30000 + 6432 contacts. **Expected to fail today – finding 1.** |

### G9 – Readout duration and packet size

| | |
|---|---|
| Input | Full readout; negotiated MTU from the BLE stack |
| Implementation | `test_g9_readout_duration_and_packet_size` |
| Expected result | No notification > MTU − 3; duration < 30 s. Run on Windows **and** macOS. |

### G10 – Firmware update with data on the device (slow)

| | |
|---|---|
| Input | `git worktree` of the newest `v*` tag; old beacon built with its `dsa_dev.conf` + `hil.conf` + `hil_flash.conf`, flashed with `--erase`; boot, wait 10 s; current hil image flashed **without** erase; reset; scan; full readout |
| Implementation | `test_g10_firmware_update_keeps_data` |
| Expected result | No storage fault bits; ≥ 6432 + 500 contacts delivered. |

### G11 – Storage full (slow)

| | |
|---|---|
| Input | Image + `hil_full.conf` (65535 contacts per boot) with `--erase`; 4 × (reset, wait 60 s) without readout; scan; full readout (≤ 1800 s) |
| Implementation | `test_g11_storage_full` |
| Expected result | Radio status bit 2 (storage full) set; data level 7; all records uncorrupted. **Expected to fail today – findings 1, 2, 3.** |

### G12 – Open readout (documented risk)

| | |
|---|---|
| Input | Full readout from the computer, whose address is not in `radio_ids.c` |
| Implementation | `test_g12_unknown_central_can_read_out` |
| Expected result | Today: all 500 contacts delivered. **Invert when the readout-security decision adds a protection.** |
