# Tag checklist (class I) – deployment protocol

Fill in one copy per deployment. Specification: `I_manual_tag.md`.

| | |
|---|---|
| Deployment | |
| Firmware release / git hash | |
| Tester | |
| Date | |
| Automated gate passed (A–H, see `Release_Gate.md`) | ☐ yes |

| ID | Test | Tags tested (IDs) | Result | Notes / measured values |
|---|---|---|---|---|
| I1 | Production build programmed, own ID advertised, no fault bits | | ☐ pass ☐ fail | |
| I2 | Self-report: short press ignored, long press → 1 report, time ± 2 s | | ☐ pass ☐ fail | |
| I3 | LED: 30 ms blink / 5 s, firework start/stop, no LED in eco (production) | | ☐ pass ☐ fail | |
| I4 | Motion: eco after timeout, leaves on movement, eco session in readout (± 5 s) | | ☐ pass ☐ fail | |
| I5 | Battery: ± 50 mV vs multimeter; status 0/1/2/3 at 3.0/2.8/2.6/2.5 V; brown-out recovery | | ☐ pass ☐ fail | V@3.0: … V@2.8: … V@2.6: … V@2.5: … |
| I6 | Battery removed during operation → flash contacts still delivered | | ☐ pass ☐ fail | before: … after: … |
| I7 | RSSI calibration table and chosen thresholds | | ☐ done | network: … location: … |
| I8 | 24–48 h soak: all readouts complete, no resets, no hangs, no faults | | ☐ pass ☐ fail | contacts/h: … |
| I9 | Current: normal … µA, eco … µA (optional) | | ☐ done ☐ n/a | |
| I10 | Clock drift over 24 h < 100 ppm | | ☐ pass ☐ fail | … ppm |

Sign-off: ______________________  Date: __________
