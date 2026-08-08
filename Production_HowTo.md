# DSA Network Beacon Production HowTo

This repo contains the production workflow for a DSA BLE beacon system:

- `Network_Beacon_nrf54`: firmware for nRF54L15 tags/beacons.
- `Network_Base_nrf54`: firmware for an nRF54L15 Development Kit used as the readout base station.
- `Network_Python_RRT+UART`: PC logger for base-station output via UART or SEGGER RTT.
- `shared/common_include.h`: shared wire/protocol constants. Treat changes here as protocol changes.

## Part 1: Installation And Hardware Connection

Install the required software on the production PC:

- nRF Connect for Desktop with the Toolchain Manager and VS Code/nRF Connect extension.
- Nordic nRF Connect SDK/Zephyr with nRF54L15 board support. The existing build artifacts indicate NCS `v3.3.0`; use one documented SDK version for production unless the firmware is requalified.
- SEGGER J-Link drivers, needed for flashing and RTT access.
- Python 3 for the optional PC logger.
- nRF Connect mobile app or another BLE scanner for quick advertisement checks.
- A serial terminal with timestamped logging, or the Python logger in this repo.

Install the Python logger dependencies once:

```powershell
cd Network_Python_RRT+UART
python -m pip install -r requirements.txt
```

Connect hardware as follows:

- For tag firmware on real tag hardware, connect the tag/debug board to the J-Link/debug connector according to the nRF54L15 tag hardware setup.
- For bench testing tag firmware on a DK, connect the nRF54L15 DK over USB.
- For the base station, connect one nRF54L15 DK over USB. Use the COM port shown by VS Code/nRF Connect or Windows Device Manager. The default UART speed is `115200`.
- If RTT logging is used, keep the J-Link connection attached and use RTT device `nRF54L15_M33`.

The main production artifacts are one programmed tag population and one programmed base station DK. The base station is the device connected to the PC during data collection.

## Part 2: Flashing Code, Targets, And Pre-Flash Parameters

Before flashing production tags, set the production identity table in `Network_Beacon_nrf54/src/radio_ids.c`:

1. Read each tag BLE address from its label, from nRF Connect scanning logs, or from a temporary scanner build.
2. Add every production address to `known_device_table`.
3. Store addresses in the byte order used by Zephyr's `bt_addr_le_t` initializer. The existing `Kurzanleitung.md` notes that discovered addresses must be entered in reverse order.
4. Assign each tag a stable one-byte ID. Unknown devices advertise ID `0xff`, which is useful for bench tests but not for production records.

Review these parameters before building a production release:

- Tag defaults in `Network_Beacon_nrf54/dsa_runtime.conf`: LED behavior, RSSI threshold, tracking enable, high/low activity radio timing, NUS timeout, and motion inactivity timeout.
- Tag fixed production settings in `Network_Beacon_nrf54/dsa.conf`: RAM/flash buffer sizes, flush thresholds, LED on-times, and self-report settings.
- Development-only data generation in `Network_Beacon_nrf54/dsa_dev.conf`: do not include this in production.
- Base station readout threshold in `Network_Base_nrf54/dsa.conf`: `CONFIG_DSA_READOUT_LEVEL`, currently `0`.
- Base station output mode in `Network_Base_nrf54/prj.conf`: currently raw UART output for use with `dsa_logger.py`.
- Data-level thresholds in `shared/common_include.h`: currently marked provisional in code and should be confirmed before rollout.
- Tag `Network_Beacon_nrf54/prj.conf` still has TODO comments for disabling logging/console in release; decide whether production should keep them enabled.

Build and flash tag firmware for actual nRF54L15 tag hardware:

```powershell
cd Network_Beacon_nrf54
west build -b nrf54l15tag/nrf54l15/cpuapp/ns -d build --sysbuild .
west flash -d build
```

Build and flash tag firmware on a DK for bench tests only:

```powershell
cd Network_Beacon_nrf54
west build -b nrf54l15dk/nrf54l15/cpuapp/ns -d build_debug --sysbuild .
west flash -d build_debug
```

Build and flash the readout base station:

```powershell
cd Network_Base_nrf54
west build -b nrf54l15dk/nrf54l15/cpuapp/ns -d build --sysbuild .
west flash -d build
```

After flashing, verify each tag with a BLE scanner. It should advertise as `DSA` with three manufacturer-data bytes:

- Byte 0: tag ID.
- Byte 1: radio/storage status bits.
- Byte 2: network status. Bits `0..3` are contact data level, bits `4..6` are battery level, bit `7` is inactivity/eco mode.

Archive every production release with the git commit, SDK version, board target, build command, generated hex/bin, and a short hardware acceptance log.

## Part 3: Operation And Data Collection

When a tag boots, it initializes LED handling, self-report storage, contact storage, battery sampling, Bluetooth, and motion detection. It then advertises as `DSA` and scans for known devices. Nearby known `DSA` devices are evaluated as contacts; records contain contact ID, 24-bit uptime seconds, and RSSI magnitude.

Expected tag behavior:

- Status LED blinks briefly at the configured interval, default 20 s.
- Self-report LED lights for the configured self-report duration when a self-report is recorded.
- On tag hardware, the ADXL367 motion sensor enables inactivity-based energy conservation. After the configured timeout, default 900 s, the tag switches to low-activity radio timing and suspends the status LED. Motion restores the stored radio/LED behavior.
- On DK hardware, no motion sensor is present, so inactivity detection is disabled gracefully.

To collect data, connect the base station DK to the PC and start the logger.

For UART logging:

```powershell
cd Network_Python_RRT+UART
python dsa_logger.py --transport uart --port COM11 --baud 115200
```

For RTT logging:

```powershell
cd Network_Python_RRT+UART
python dsa_logger.py --transport rtt --rtt-device nRF54L15_M33
```

Reset the base station. It should print:

```text
Network base ready. Press button0 to Start connecting. Press button1 to stop connecting
```

Press button0 to start scanning and readout. The base station connects to `DSA` beacons whose advertised data level is at least `CONFIG_DSA_READOUT_LEVEL`, negotiates NUS throughput, sends the start command `st`, receives the export, and disconnects after the beacon sends `finished`. Press button1 to stop after the current transfer finishes.

Base station LEDs:

- LED0: program running.
- LED1: scanning active.
- LED2: connected to a beacon.

The logger writes `dsa_YYYYMMDD_HHMM.log` in the current directory. Parsed lines include PC timestamp, beacon ID, current timer, contact count, voltage, contact ID/timer/RSSI records, and self-report times. Keep these log files as production records.

A successful export consumes stored contact and self-report data on the beacon. If a transfer is interrupted, already acknowledged entries from a partial flash block can be resent because checkpointing is intentionally deferred to reduce flash writes.

Runtime configuration is possible through BLE command advertisements named `DST`/`DSZ`; normal `DSA` advertisements are treated as contact beacons. Command manufacturer data starts with a target byte, followed by one or more 3-byte parameter records: parameter ID plus big-endian 16-bit value. Target `0xff` broadcasts to all tags; otherwise the target must match the tag ID.

Supported persistent runtime parameters:

- LED: enable/disable status LED, blink interval, reset LED defaults.
- Network: RSSI threshold magnitude, contact tracking enable/disable, reset network defaults.
- Radio: high/low activity mode, advertising intervals, scan intervals/windows, reset radio defaults.
- Motion: inactivity detection enable/disable, inactivity timeout, reset motion defaults.

Before and during production operation, keep these settings under control: tag ID table, RSSI threshold, base readout level, data-level thresholds, battery threshold policy, and whether tag logging remains enabled.
