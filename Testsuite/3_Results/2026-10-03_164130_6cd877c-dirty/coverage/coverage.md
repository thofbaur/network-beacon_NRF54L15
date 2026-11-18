# Coverage

Used to find untested branches, not as a target number (see
`1_Specification/Release_Gate.md`).

## Firmware (C, host tests)

| File | Lines | Branches | Functions |
|---|---|---|---|
| `Network_Base_nrf54/src/message_parser.c` | 91.7 % | 85.0 % | 100.0 % |
| `Network_Beacon_nrf54/src/battery_voltage.c` | 94.2 % | 87.5 % | 100.0 % |
| `Network_Beacon_nrf54/src/device.c` | 98.4 % | 94.4 % | 100.0 % |
| `Network_Beacon_nrf54/src/eco_log.c` | 82.6 % | 66.7 % | 80.0 % |
| `Network_Beacon_nrf54/src/eco_log_storage.c` | 83.3 % | – | 83.3 % |
| `Network_Beacon_nrf54/src/flash_ring_store.c` | 91.3 % | 74.0 % | 100.0 % |
| `Network_Beacon_nrf54/src/led.c` | 87.8 % | 70.7 % | 100.0 % |
| `Network_Beacon_nrf54/src/motion.c` | 75.7 % | 75.0 % | 83.3 % |
| `Network_Beacon_nrf54/src/network.c` | 83.6 % | 69.9 % | 95.7 % |
| `Network_Beacon_nrf54/src/network_storage.c` | 88.2 % | 100.0 % | 83.3 % |
| `Network_Beacon_nrf54/src/param_storage.c` | 91.0 % | 80.4 % | 100.0 % |
| `Network_Beacon_nrf54/src/radio.c` | 83.1 % | 70.0 % | 94.1 % |
| `Network_Beacon_nrf54/src/ram_log_ring.c` | 88.8 % | 74.1 % | 100.0 % |
| `Network_Beacon_nrf54/src/self_report.c` | 77.4 % | 70.8 % | 84.6 % |
| `Network_Beacon_nrf54/src/self_report_storage.c` | 83.3 % | – | 83.3 % |
| `Network_Beacon_nrf54/src/storage_work_queue.c` | 100.0 % | 100.0 % | 100.0 % |
| **Total** | **86.6 %** | **73.9 %** | **94.1 %** |

Not compiled into any host test (covered only by hardware tests F–H or not at all):

- `Network_Beacon_nrf54/src/development_validation.c`
- `Network_Beacon_nrf54/src/main.c`
- `Network_Beacon_nrf54/src/nus.c`
- `Network_Base_nrf54/src/led.c`
- `Network_Base_nrf54/src/main.c`
- `Network_Base_nrf54/src/nus.c`
- `Network_Base_nrf54/src/output.c`
- `Network_Base_nrf54/src/radio.c`
- `Network_Base_nrf54/src/rtt_output.c`
- `Network_Base_nrf54/src/uart_output.c`

Data only, no code to measure: `Network_Beacon_nrf54/src/radio_ids.c`

Line-by-line: `c_html/index.html`

## Python

| File | Statements | Branches | Coverage |
|---|---|---|---|
| `Network_Log+Postprocessing/dsa_logger.py` | 333/408 | 98/124 | 81.0 % |
| `Scan_New_Tags/generate_ids.py` | 48/49 | 19/20 | 97.1 % |
| `Scan_New_Tags/read_tags.py` | 49/54 | 9/10 | 90.6 % |
| **Total** | | | **83.6 %** |

Line-by-line: `python_html/index.html`
