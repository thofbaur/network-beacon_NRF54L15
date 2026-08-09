# Decisions

Short records of design intent. Add entries when the reason for a choice would not be obvious from the code.

## 2026-06-20: Keep Startup Thin

`main.c` should only initialize and start the system. Domain behavior belongs in owner modules.

## 2026-06-20: Route Commands Through Radio, Execute In Owner Domains

Commands arrive through BLE scanning, so radio owns transport and routing. The affected domain owns validation, behavior, and persistence.

## 2026-06-20: Keep Persistence Domain-Owned

Each domain owns its defaults and parameter meaning. The storage layer remains a generic load/save wrapper.

## 2026-06-20: Keep Advertised Status Compact

Advertisements expose only compact identity and status information so nearby devices can inspect state without a connection.

## 2026-06-20: Treat Contact Export Format As Protocol

The contact export shape is part of the external protocol. Changes require an explicit compatibility decision.

## 2026-06-20: Known Device Identity Is Centralized

Device identity mappings are kept in one place so scan filtering and local ID lookup stay aligned.

## 2026-06-20: NUS Export Drains Stored Contacts

The current export model treats a successful readout as consuming stored contact data.

## 2026-07-11: Self-Report Uptime Uses 24 Bits

Self-report timestamps are stored and exported as three big-endian bytes containing
the low 24 bits of uptime seconds. Values above `0x00ffffff` therefore wrap modulo
`0x01000000`. NUS receivers must parse flag `0x06` payloads as a sequence of
three-byte entries instead of the previous four-byte entries.

## 2026-07-29: NUS Contact Count Uses 24 Bits

The `DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE` packet contains the contact count as
an unsigned three-byte big-endian value. The packet layout is flag (1 byte),
uptime (4 bytes), contact count (3 bytes), and battery voltage (2 bytes).
Internal contact counting remains 32-bit; values above `0x00ffffff` saturate
on the wire.

## 2026-08-03: Export Drop Checkpoints Minimize Flash Writes

Contact and self-report exports defer metadata checkpoints for partial flash
block drops and sync once after the export loop. A reset before that sync can
therefore resend already acknowledged entries. This is intentional to reduce
flash writes; full-block retirement still persists the new queue head before
deleting the retired block.

## 2026-08-04: Contact Data Packets Carry Per-Packet Count

Each `DSA_NUS_FLAG_DATA` packet now carries a one-byte contact count directly
after the flag byte, followed by that many five-byte contact records. The count
is per packet, not the total remaining contact count.

## 2026-08-05: Battery Status Shares Network Status Byte

Battery voltage is sampled periodically and cached. NUS status responses report
the cached voltage instead of reading ADC during transfer. The advertised
network status byte keeps contact data level in bits 3..0 and carries battery
status in bits 7..4.

## 2026-08-05: Energy Conservation Bit Narrows Battery Status To 3 Bits

`BATTERY_LEVEL_MASK` moves from bits 7..4 to bits 6..4. Battery status only
ever takes values 0-3, so the narrower field loses no range. Bit 7 becomes
`ECO_MODE_MASK`, reporting whether the device is in inactivity-triggered
energy conservation mode. This is a protocol/advertised-data compatibility
change: any receiver that masked the network status byte with the old
`0xF0` for battery status must update to `0x70`.

## 2026-08-05: Inactivity Detection Overrides Radio/LED Without Persisting

A new `motion` domain (`src/motion.c`) watches the onboard ADXL367
accelerometer and, after a configurable period with no activity interrupt,
forces the tag into energy conservation mode: `radio_set_eco_override()`
temporarily forces `LOW_ACTIVITY` radio behavior and `led_suspend_blinking()`
stops the status LED. Both are non-persisted overrides layered on top of the
user's stored radio/LED preferences (`params_radio.mode`, `params_led`) —
they never call `radio_params_save()`/`led_params_save()`. Motion resuming
reverts to whatever the user had stored, not necessarily to `HIGH_ACTIVITY`.
The inactivity timeout itself is implemented as a software timer reset by
the accelerometer's hardware activity interrupt, rather than the ADXL367's
own inactivity register, so the configurable timeout isn't bounded by the
sensor's sample-count register width. Energy conservation state is not
persisted across reset; every boot starts active and re-evaluates from
there.

## 2026-08-07: Energy Conservation Keeps Normal Advertising, Duty-Cycles Scanning

Energy conservation mode no longer reuses `LOW_ACTIVITY` radio parameters.
Advertising keeps the normal (high-activity) interval throughout; only
scanning is reduced, on a much longer period (`CONFIG_DSA_ECO_SCAN_INTERVAL_MS`,
default 300 s) than the BLE scan-interval field can express (max ~10.24 s
per the Core spec). This is implemented as periodic application-triggered
scan bursts — `bt_le_scan_start`/`stop` toggled by a delayable work item in
`radio.c` — rather than a single `bt_le_scan_param`, since scanning is the
dominant power cost at any duty cycle above roughly 0.01%: a scan window
holds the receiver active for its full duration, while an advertising
event is a sub-millisecond burst regardless of interval. The
manually-selectable `LOW_ACTIVITY` radio command mode is unchanged by this;
only the automatic inactivity-triggered override behaves differently now.

## 2026-08-07: Development LED Indicator For Energy Conservation Mode

`CONFIG_DSA_DEV_ECO_LED_INDICATOR` (default `y`) makes `led_suspend_blinking()`
light the onboard blue LED solid instead of turning the status LED off while
in energy conservation mode, so the mode is visible on the bench. This works
against the point of energy conservation (an LED left on draws more power
than one that's off) and must be disabled before release; it's grouped with
the other `DSA_DEV_*` flags for that reason. The indicator LED
(`led1_blue`) degrades gracefully when a board doesn't provide it, unlike
the status/self-report LEDs which are required.

## 2026-08-07: Low-Activity Radio Mode Fully Replaced By Eco Mode

`LOW_ACTIVITY` is retired; `ECO_ACTIVITY` is now the only reduced-power
radio mode, for both triggers:

- **Manual and persisted**: `P_SET_RAD_ACTIVE` still exists and still
  persists via `radio_params_save()`, but now selects `HIGH_ACTIVITY` or
  `ECO_ACTIVITY` directly — a user can permanently put the tag in eco mode
  over BLE, independent of motion detection.
- **Automatic and temporary**: `motion.c`'s `radio_set_eco_override()` still
  layers a non-persisted override on top of whichever mode is stored,
  exactly as before, just renamed.

Both paths now funnel through one function, `radio_apply_mode_locked()` in
`radio.c`, which knows how to start/stop the eco scan-burst cycle
(`eco_scan_handler`) as well as the advertising restart — the two
mechanisms can no longer diverge in how "eco" is actually implemented,
unlike the old design where `set_ble_params()`/`update_ble_params()`
handled the persisted-mode path and a separate bespoke block in
`radio_set_eco_override()` handled the motion-triggered path.

**Protocol renames** in `shared/common_include.h`:

- `P_ADV_INTERVAL_LOWACTIVITY_MS` → `P_ADV_INTERVAL_ECO_MS` (same ID, same
  ms/BLE-unit semantics — kept runtime-adjustable and independent from the
  normal advertising interval per explicit request, even though its
  default now equals the normal interval rather than a slow ~10s rate).
- `P_SCAN_WINDOW_LOWACTIVITY_MS` → `P_ECO_SCAN_WINDOW_MS` (same ID, same
  semantics: the burst duration).
- `P_SCAN_INTERVAL_LOWACTIVITY_MS` is **retired, not renamed** — replaced by
  a new `P_ECO_SCAN_PERIOD_S` (a different ID, `P_BASE_RADIO + 7`). The old
  parameter was a millisecond BLE scan-interval value (max ~10.24 s); the
  new one is a plain-seconds burst period with no such ceiling, so reusing
  the old ID with new semantics would silently misinterpret any external
  tooling still sending the old format. Do not reuse `P_BASE_RADIO + 4`.

**Storage**: `radio_params` persists under a new key, `"dsa/radio2"`
(was `"dsa/radio"`), because the struct's field semantics changed at the
same byte length — reusing the old key would silently misinterpret
previously-stored bytes rather than falling back to defaults. Any
device already flashed with the old firmware starts fresh on radio
parameters after this update.

**Kconfig renames**: `DSA_LOW_ACTIVITY_ADV_INTERVAL_MIN/MAX_MS` →
`DSA_ECO_ADV_INTERVAL_MIN/MAX_MS` (defaults changed from ~10s to match the
high-activity defaults, 90/120 ms, per explicit request — advertising is
not reduced in eco mode by default, but remains independently
configurable). `DSA_LOW_ACTIVITY_SCAN_WINDOW_MS` → `DSA_ECO_SCAN_WINDOW_MS`
(default unchanged, 100 ms). `DSA_LOW_ACTIVITY_SCAN_INTERVAL_MS` is
removed with no replacement Kconfig default of its own; the equivalent
concept is `DSA_ECO_SCAN_INTERVAL_MS` (already introduced 2026-08-07,
default 300 s), which seeds `params_radio.eco_scan_period_s` in
`set_radio_params_init()`.

## 2026-08-07: Eco Session History Logged As Paired Enter/Leave Records

A new `eco_log` domain (`src/eco_log.c`, `src/eco_log_storage.c`) records
when the tag enters and leaves eco mode, structurally identical to
`self_report.c`/`self_report_storage.c` (RAM ring, own flash partition,
NUS export) but with a 6-byte paired entry — `[enter_time(3B),
leave_time(3B)]`, each the low 24 bits of uptime seconds, matching the
existing self-report/contact timestamp convention — instead of a single
timestamp.

A record is written only when a session **completes** (on leaving eco
mode), once both timestamps are known, rather than as two independent
immediate events. This was an explicit choice over logging enter and leave
as separate momentary events (which would match self-report's pattern
more closely and survive a reset mid-session): a paired record is directly
useful as a session duration without reconstruction, at the cost that a
session still in progress at reset is never recorded.

`radio.c` is the sole writer: `radio_apply_mode_locked()` — the single
function both the manual `P_SET_RAD_ACTIVE` command and motion.c's
override funnel through — tracks the last mode it actually applied and
calls `eco_log_enter()`/`eco_log_leave()` only on a real transition. This
means the log reflects actual radio behavior regardless of which trigger
caused it, not just motion-triggered eco specifically.

New NUS packet flag `DSA_NUS_FLAG_ECO_LOG` (0x07). `nus.c`'s
`send_eco_log()` mirrors `send_self_reports()` exactly and is sent in the
same transfer, after self-reports and before contact data.

New flash partition `eco_log_storage` (0x2000 bytes, matching
`CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES`) added to `pm_static.yml`, growing
`nonsecure_storage` accordingly. New storage-fault bits
`STORAGE_FAULT_ECO_LOG_*` (BIT 13-17) added to `device.h`, folded into the
existing `STORAGE_FAULT_CONTACT_MASK`/`STORAGE_STATUS_CONTACT_ERROR` and
`STORAGE_STATUS_META_ERROR` — matching the same (already-deferred)
conflation of contact/self-report storage faults under one status bit,
rather than introducing a third inconsistency in a system already
scheduled for a status-reporting redesign.

## 2026-08-09: Advertised Status Byte Redesigned Around Field Diagnosability

The status-reporting redesign deferred in the eco-log entry above happened
now, driven by a specific framing: in production, a tag is worn/deployed in
the field and the advertised status byte (`ADV_POS_RADIO_STATUS`) — readable
by a passive BLE scan, no connection required — is the *only* diagnostic
channel available. That reframes which bits are worth the 8 available:
distinctions that don't change what a field technician would do (a scan
runtime failure vs. a scan config failure; a storage record fault vs. a
metadata fault) are worth less than currently-invisible conditions that mean
"this tag needs physical attention."

`STORAGE_FAULT_CONTACT_MASK`/`STORAGE_STATUS_CONTACT_ERROR`/
`STORAGE_STATUS_META_ERROR` (referenced by name in the eco-log entry above)
no longer exist. New layout in `device.h`/`shared/common_include.h`:

- Bit 0 `RADIO_STATUS_SCAN_ERROR` (was two bits, `RADIO_STATUS_SCAN_RUNTIME_ERROR`
  and `RADIO_STATUS_SCAN_CONFIG_ERROR`): both meant "not tracking contacts
  right now" to an outside observer, so `radio.c` now tracks
  `scan_runtime_fault`/`scan_config_fault` independently and ORs them into
  one advertised bit via `radio_update_scan_status()` — kept as two internal
  booleans specifically so one clearing doesn't clobber the other's state.
- Bit 1 `RADIO_STATUS_NUS_ERROR`: unchanged. Ranked most severe of the
  pre-existing bits on reflection — if set, the tag's data can never be
  retrieved over BLE at all.
- Bit 2 `STORAGE_STATUS_STORAGE_FULL` (new): contact/self-report/eco-log
  storage is full and actively discarding its oldest un-exported entries.
  This condition already existed as internal state
  (`contact_nvm_full`/`ram_log_ring`'s `nvm_full`) purely to gate flush
  retries, but was never surfaced — despite being real, ongoing data loss on
  a device whose entire purpose is data collection. `device_set_storage_full()`
  in `device.c` aggregates per-domain `STORAGE_FULL_*` bits the same way
  `device_set_storage_fault()` already aggregated fault bits.
- Bit 3 `STORAGE_STATUS_PARAM_ERROR`: unchanged position; mask now also
  includes `STORAGE_FAULT_MOTION_PARAMS`. `motion.c` was the only one of the
  four persisted-parameter domains (LED/network/radio/motion) that never
  called `device_set_storage_fault()` — an oversight, not a deliberate
  omission, closed here rather than left inconsistent.
- Bit 4 `STORAGE_STATUS_STORAGE_ERROR` (was `STORAGE_STATUS_CONTACT_ERROR` +
  `STORAGE_STATUS_META_ERROR`): merged, via `STORAGE_FAULT_STORAGE_MASK` now
  including the three `*_META` fault bits alongside init/read/write/delete.
  A record fault and a metadata fault both mean "the flash storage subsystem
  is unhealthy" to someone who can only reach the tag over BLE.
- Bit 5 `RADIO_STATUS_MOTION_UNAVAILABLE` (new): set once in `motion_init()`
  from `motion_available`. If the ADXL367 fails `device_is_ready()` or
  trigger setup, inactivity detection silently never runs and the tag is
  permanently stuck in high-activity mode — a battery-life risk with no
  prior visibility.
- Bits 6-7: reserved.

`self_report.c`/`eco_log.c` share a generic `ram_log_ring.c` module (this
session's RAM-ring counterpart to `flash_ring_store.c`'s earlier dedup of
the flash layer), so the new storage-full bit was wired once, via a
`storage_full_bit` field on `ram_log_ring_config`, and both domains got it
for free.
