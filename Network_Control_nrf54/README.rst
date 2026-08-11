Beacon Control
##############

Overview
********

Control application for the beacons in the Network_Beacon_nrf54 project.
It advertises a hardcoded command packet using the beacon command protocol
(device name ``"DSZ"`` plus manufacturer data
``[target, parameter, value_hi, value_lo, ...]``, see
``BeaconNRF54/shared/common_include.h``). Pressing Button 3 starts
advertising the command; pressing Button 4 stops it.

``src/main.c`` defines every beacon command parameter and lists them all,
commented out, in the ``mfg_data`` array. To change what gets sent, edit
that array: uncomment a parameter's line (and adjust its value) to include
it, comment out a line to drop it, and set ``CMD_TARGET`` to a specific
beacon id or leave it broadcasting to all. Multiple parameters may be
active at once - the beacon applies every (parameter, value) triple it
finds in the packet. Reflash after editing.

Requirements
************

* nRF54L15 DK (``nrf54l15dk/nrf54l15/cpuapp/ns``)

Building and Running
*********************

.. code-block:: console

   west build -b nrf54l15dk/nrf54l15/cpuapp/ns
   west flash
