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
