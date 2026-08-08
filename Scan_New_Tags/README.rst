.. _scan_new_tags:

Scan New Tags
#############

.. contents::
   :local:
   :depth: 2

This application continuously scans for Bluetooth LE advertisements. It never
establishes a connection. Whenever the advertised device name matches one of
the names configured in :file:`src/main.c`, the device's Bluetooth LE address
is sent out over UART.

Requirements
************

The application is built for the nRF54L15 development kit
(``nrf54l15dk/nrf54l15/cpuapp/ns``).

Configuration
*************

The list of device names to look for is a plain C array, ``target_names``, at
the top of :file:`src/main.c`. Edit it and reflash to change which devices
are reported.

UART
****

Matching addresses are sent as ASCII strings (for example
``AA:BB:CC:DD:EE:FF\r\n``) over the kit's on-board UART (routed through the
SEGGER J-Link VCOM) at 115200 baud, 8N1. This UART carries only address
reports; console output is disabled and log messages are sent over RTT
instead, so the UART link stays clean.

Building and running
*********************

.. code-block:: console

   west build -p -b nrf54l15dk/nrf54l15/cpuapp/ns
   west flash

Testing
=======

1. Connect the kit to your computer with a USB cable.
#. Open a serial terminal on the kit's VCOM port at 115200 baud, 8N1.
#. Optionally, connect to the kit's RTT console to view log messages.
#. Reset the kit and observe the log "Scanning for advertisements".
#. Start advertising from another device using one of the names configured
   in ``target_names``.
#. Observe that the device's address is printed on the serial terminal.
