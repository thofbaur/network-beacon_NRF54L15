# Class I – Manual tests on the tag

Last step before a field deployment. Covers what the DK physically lacks:
the tag's button and LEDs, the ADXL367, a real battery and real radio
conditions. Record results in `TAG_CHECKLIST.md`.
"Manual → automatic check" means the stimulus is manual, the result is
checked by reading the tag out through a DK running the Base firmware and
`dsa_logger.py` (or `2_Implementation/hil/ble.py`).

**Input signals:** physical stimuli by the tester (button, movement,
battery removal, supply voltage, distance and placement), commands sent
with DK B as Commander, time.

---

### I1 – Production build programmed and identified

| | |
|---|---|
| Input | Release build for `nrf54l15tag/nrf54l15/cpuapp/ns` (E1) programmed with J-Link per `Production_HowTo.md`; power on; BLE scan |
| Requirements | Tag, J-Link |
| Automated | 👤 manual |
| Expected result | Advertises `DSA` with the tag's own ID from `radio_ids.c` (not 0xFF); no fault bits; reported version = release version (once E3's prerequisite exists). |

### I2 – Self-report button

| | |
|---|---|
| Input | Button presses: (a) short < 1 s, (b) long ≥ 3 s, (c) 5 quick presses then hold 3 s; note the PC time of each long press; readout |
| Requirements | Tag, DK B as Base, logger |
| Automated | 👤 → ✅ |
| Expected result | (a) no report, no red LED; (b) red LED 2 s, 1 report; (c) 1 report; resolved times within ± 2 s of the noted times. |

### I3 – LED patterns

| | |
|---|---|
| Input | Commander: `0x21 = 1`, `0x22 = 5`; then `0x23 = 1` (firework), `0x23 = 0`; dev build with `CONFIG_DSA_DEV_ECO_LED_INDICATOR=y` in eco mode |
| Requirements | Tag, DK B as Commander |
| Automated | 👤 manual (visual) |
| Expected result | Green 30 ms flash every 5 s; firework flickers all LEDs randomly and stops cleanly; dev build: blue solid in eco; production build: no LED in eco. |

### I4 – Motion and eco mode

| | |
|---|---|
| Input | Commander: `0xA1 = 1` (motion active), `0xA2 = 60`; lay the tag still ≥ 70 s (note time); move it (note time); scan advertisement before/after; readout |
| Requirements | Tag (ADXL367), DK B as Commander and Base |
| Automated | 👤 → ✅ |
| Expected result | Eco bit (network status bit 7) set after the timeout, cleared after movement; 1 eco session matching the still period ± 5 s; motion-unavailable bit (5) clear. |

### I5 – Battery voltage and brown-out

| | |
|---|---|
| Input | Lab supply instead of the battery at 3.0 / 2.8 / 2.6 / 2.5 V (reboot or wait 4 h per step); then, with a dev build filling synthetic contacts, ramp the supply slowly from 3.0 V to below brown-out and back |
| Requirements | Tag, multimeter, lab power supply, Base |
| Automated | 👤 → ✅ |
| Expected result | Reported voltage within ± 50 mV of the multimeter; battery status 0 / 1 / 2 / 3; after brown-out: reboot, readout without corrupted records. |

### I6 – Real power cut

| | |
|---|---|
| Input | Tag with several thousand contacts already flushed to flash; remove the battery during operation; reinsert; readout |
| Requirements | Tag, Base |
| Automated | 👤 → ✅ |
| Expected result | All contacts that were in flash before the cut are delivered. **Expected to fail today – finding 1.** |

### I7 – RSSI calibration in real rooms

| | |
|---|---|
| Input | Second tag at 0.5 / 1 / 1.5 / 2 / 3 m, worn on the body front/back, through a wall; same for the location tag; ≥ 5 min per position |
| Requirements | ≥ 2 tags, location tag, Base, logger |
| Automated | 👤 manual |
| Expected result | Median RSSI per distance recorded; chosen `P_RSSI_NETWORK` / `P_RSSI_LOCATION` documented with the measurement. |

### I8 – Soak test 24–48 h

| | |
|---|---|
| Input | Deployment setup (several tags, Base, logger) running 24–48 h with periodic readouts |
| Requirements | Several tags, Base, logger, PC |
| Automated | 👤 → ✅ |
| Expected result | Every readout completes; timers increase between readouts (no reset); no tag disappears; no fault bits; contacts per hour recorded. |

### I9 – Current draw (optional)

| | |
|---|---|
| Input | Power Profiler Kit II as supply; 10 min in normal mode, 10 min in eco mode |
| Requirements | Tag, PPK2 |
| Automated | 👤 manual |
| Expected result | Average current per mode recorded; battery life estimate ≥ planned deployment duration. |

### I10 – Clock drift of the tag

| | |
|---|---|
| Input | Two readouts 24 h apart; PC time of each readout and the `Current Timer` value |
| Requirements | Tag, Base, logger |
| Automated | 👤 → ✅ |
| Expected result | |drift| < 100 ppm (< 9 s per day). |
