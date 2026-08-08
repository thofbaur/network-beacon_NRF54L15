# Scan New Tags — HowTo

## What it does

This tool finds the BLE addresses of new/unlabeled DSA tags and turns them
into ready-to-paste entries for
`Network_Beacon_nrf54/src/radio_ids.c`. It has three parts:

- **Firmware** (`src/main.c`): runs on an nRF54L15 DK. It continuously
  scans for BLE advertisements (never connects), checks the advertised
  device name against a configured list (`target_names`, default
  `"Nordic_LBS"`), and for every match prints `Device,<name>,<address>\r\n`
  over the DK's on-board UART. Console/log output is routed to RTT instead,
  so the UART line carries nothing but address reports.
- **Logger** (`read_tags.py`): runs on the PC, reads those lines from the
  serial port, and appends each newly seen device to a timestamped CSV file
  (`tag_adresses_<timestamp>.csv`) with columns `name, addr1..addr6`.
  Duplicate addresses seen again in the same session are ignored.
- **ID generator** (`generate_ids.py`): reads every `tag_adresses_*.csv`
  file in this directory (so it can combine multiple logging sessions),
  drops duplicate addresses across all of them, and writes `ids.c`
  containing one `known_device_table`-style block per unique tag, with
  address bytes reversed and `.id` values assigned sequentially.

## Install

Firmware (needs the Nordic nRF Connect SDK/Zephyr toolchain and `west`,
already required for the other firmware in this repo):

```powershell
cd Scan_New_Tags
west build -p -b nrf54l15dk/nrf54l15/cpuapp/ns
west flash
```

Logger (needs Python 3):

```powershell
cd Scan_New_Tags
python -m pip install pyserial
```

## Use

1. Edit `target_names` in `src/main.c` to the advertised name(s) of the tags
   you want to discover, then rebuild and reflash (see above).
2. Connect the DK to the PC via USB.
3. Run the logger, passing the DK's VCOM port with `-p`/`--port` (check
   Device Manager or VS Code/nRF Connect for the actual port; defaults to
   `COM11` if omitted):

   ```powershell
   python read_tags.py -p COM11
   ```

4. Power on / bring the target tags into range. Each new match prints to the
   console and is appended to the CSV as it's found. Stop with Ctrl+C.
5. Once you're done logging (across one or more sessions/CSV files), generate
   the `ids.c` blocks. `--start-id` sets the ID of the first entry (default
   `1`); pick a value that doesn't collide with IDs already used in
   `radio_ids.c`:

   ```powershell
   python generate_ids.py --start-id 5
   ```

6. Copy the generated blocks from `ids.c` into the `known_device_table`
   array in `Network_Beacon_nrf54/src/radio_ids.c`, per
   `Production_HowTo.md`. Replace the CSV-derived `// <name>` comments with
   something identifying the physical tag if useful.

Optionally, open an RTT console to the DK to see log messages
(`Bluetooth initialized`, `Scanning for advertisements`, `Match: ...`).
