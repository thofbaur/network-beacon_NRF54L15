# Traceability: design decisions → tests

Every `##` decision in `Network_Beacon_nrf54/DECISIONS.md` must appear here
with at least one test ID; test A5 enforces this. When you add a decision,
add a row and the test that proves it.

| Decision | Tests | What the tests check |
|---|---|---|
| 2026-06-20: Keep Startup Thin | E1 | Structural rule; builds stay green (E1). Enforced by review. |
| 2026-06-20: Route Commands Through Radio, Execute In Owner Domains | C9, H1, D3 | Command parsing/routing in radio.c, domains validate and apply. |
| 2026-06-20: Keep Persistence Domain-Owned | C3, C5, C8, C10 | Each domain persists, validates and migrates its own parameters. |
| 2026-06-20: Keep Advertised Status Compact | C9, G1 | Advertisement carries only ID + 2 status bytes. |
| 2026-06-20: Treat Contact Export Format As Protocol | A1, B1, D1, G2 | Record layout identical in beacon, Base and logger. |
| 2026-06-20: Known Device Identity Is Centralized | A2, C6 | One table; lookup and accept list use it. |
| 2026-06-20: NUS Export Drains Stored Contacts | C3, G3 | Successful export removes the data. |
| 2026-07-11: Self-Report Uptime Uses 24 Bits | C4, B1 | 24-bit self-report time, 3-byte entries. |
| 2026-07-29: NUS Contact Count Uses 24 Bits | B1, D1 | 24-bit contact count, saturation. |
| 2026-08-03: Export Drop Checkpoints Minimize Flash Writes | C1, G4 | Partial drops checkpointed only by sync; resend after reset. |
| 2026-08-04: Contact Data Packets Carry Per-Packet Count | B1, B3, D1 | Per-packet count byte in DATA packets. |
| 2026-08-05: Battery Status Shares Network Status Byte | C11, A1, G1 | Battery field in the network status byte. |
| 2026-08-05: Energy Conservation Bit Narrows Battery Status To 3 Bits | A1, C6, C11 | Battery field 0x70, eco bit 0x80. |
| 2026-08-05: Inactivity Detection Overrides Radio/LED Without Persisting | C9, C10, C8, I4 | Eco override and LED suspend are not persisted. |
| 2026-08-07: Energy Conservation Keeps Normal Advertising, Duty-Cycles Scanning | C9, H2 | Eco keeps advertising, scans in bursts. |
| 2026-08-07: Development LED Indicator For Energy Conservation Mode | C10, E3, I3 | Indicator only in development builds. |
| 2026-08-07: Low-Activity Radio Mode Fully Replaced By Eco Mode | C9, D3, H2 | P_SET_RAD_ACTIVE selects high/eco; dsa/radio2 key. |
| 2026-08-07: Eco Session History Logged As Paired Enter/Leave Records | C4, C9, H2 | Enter/leave pairs logged on real transitions. |
| 2026-08-09: Advertised Status Byte Redesigned Around Field Diagnosability | C6, C9, G1 | Status byte bit layout. |
| 2026-08-11: Motion Bring-Up Failure Splits Into Two Advertised Bits | C8, F2, I4 | Motion unavailable / probe timeout bits. |
| 2026-08-11: `param_storage_load()` Now Treats A 0-Byte Read As Not-Found | C5 | 0-byte settings read = not found. |
| 2026-08-11: Motion Bring-Up Retries `device_init()` Itself, By Forcing It | F2, I4 | Sensor bring-up retries; needs the ADXL367 (tag). |
| 2026-08-12: `CONFIG_DSA_MOTION_DEFAULT_ACTIVE` Moved To `dsa_runtime.conf` | C8, E3 | Motion default from dsa_runtime.conf. |
| 2026-08-12: Exported Batches Now Require A Base Ack Before Commit | G4, G5, H5 | Batch committed only after the matching ack. |
| 2026-08-13: Firework LED Effect Runs Independent Of Eco Mode | C10 | Firework continues through eco suspend. |
