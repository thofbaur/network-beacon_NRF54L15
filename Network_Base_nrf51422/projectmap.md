# Outline
This is intended to be the base station for the network beacon project based on NRF54L15.

The code shall run on an nrf51422 Development kit (standard overlay).
With pressing button0, the device shall enter scanning mode. It shall continuously scan for beacons based on nrf54l15. If a beacon with name DSA is found, it shall check the manufacturing data of the advertisment package. No company ID, pure payload. From the 3 bytes of data the relevant id, radiostatus and network status shall be extracted according to the defined position. If network status , cleared by the mask and rightshifted by the defined shift is larger or equal than the defined readout_level, a connection to the beacons shall be established.

Then the text "st" shall be sent via Nordic Uart Service.
It shall then receive data from the connected beacon. Data is split in packages, each package is preceded by a defined flag. For each flag a fixed data length (or multiples) are defined.
For the time flag, the data will be 4 bytes, uint32.
For the network data flag, the data will contain a recorded contact. it will be multiples of 5 bytes, each containing a single data set. First byte is id of contact, 2 to 4 are timer value uint24 in big endian format, 5 is negative RSSI.
For the voltage flag, the data will be one byte
For the control flag, the data will be eight byte.
Each data package shall be analyzed and written via UART to the debug console (specifics to be defined later).
Each write to UART should begin with "ID:" and value of the currently connected beacon.
For time, then should follow "Current Timer:" and then the value of the data.
For data the print should give: Contact-ID, Timer, RSSI.
After receiving the control flag and data package "finished", the beacon shall be disconnected.

Pressing button1 shall wait until the current connection send finished, then disconnect and stop scanning mode.

# LED
LED0 shall be active if programm is running.
LED1 shall be active if programm is in scanning mode
LED2 shall be active if a connection is established