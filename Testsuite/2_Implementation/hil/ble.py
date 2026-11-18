"""BLE central on the computer's Bluetooth adapter (bleak; Windows, macOS,
Linux) that plays the Base's role in a readout - with deliberate faults.

Readout protocol (Network_Beacon_nrf54/src/nus.c):
  1. connect, enable notifications on the NUS TX characteristic
  2. write "st" to the NUS RX characteristic
  3. beacon notifies: TIME_CONTACTS_VOLTAGE, then DATA / SELF_REPORT / ECO_LOG
     batches - each batch waits for an ack {0xAC, flag, count} written to RX
     (2 s timeout) - then CONTROL "finished"; the beacon disconnects.
Notifications are decoded with dsa_logger.parse_payload, so the tests use the
same decoder as production.

macOS hides device addresses from applications, so beacons are identified
by the ID byte of their manufacturer data (ADV_POS_ID), never by address.
"""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field
from typing import Callable

import frames
from repo import dsa_logger

NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # central -> beacon (write)
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # beacon -> central (notify)


@dataclass
class Advert:
    name: str | None
    mfg: bytes  # [id, radio status, network status]
    rssi: int
    device: object
    seen_at: float

    @property
    def beacon_id(self) -> int:
        return self.mfg[0]

    @property
    def radio_status(self) -> int:
        return self.mfg[1]

    @property
    def network_status(self) -> int:
        return self.mfg[2]


def manufacturer_bytes(adv) -> bytes | None:
    """The beacon puts [id, radio, network] directly into the manufacturer
    data field; bleak splits the first two bytes off as 'company id'."""
    for company_id, payload in adv.manufacturer_data.items():
        return company_id.to_bytes(2, "little") + bytes(payload)
    return None


async def _scan_for(beacon_id: int, timeout: float, names=("DSA", "DST", "DSL")) -> Advert:
    from bleak import BleakScanner

    found: list[Advert] = []
    event = asyncio.Event()

    def callback(device, adv):
        mfg = manufacturer_bytes(adv)
        if mfg and len(mfg) >= 3 and mfg[0] == beacon_id and adv.local_name in names:
            found.append(Advert(adv.local_name, mfg, adv.rssi, device, time.monotonic()))
            event.set()

    async with BleakScanner(callback):
        try:
            await asyncio.wait_for(event.wait(), timeout)
        except asyncio.TimeoutError:
            raise TimeoutError(f"beacon {beacon_id} not seen within {timeout}s") from None
    return found[-1]


def find_beacon(beacon_id: int, timeout: float = 20.0) -> Advert:
    return asyncio.run(_scan_for(beacon_id, timeout))


async def _collect_adverts(beacon_id: int, seconds: float) -> list[Advert]:
    from bleak import BleakScanner

    adverts: list[Advert] = []

    def callback(device, adv):
        mfg = manufacturer_bytes(adv)
        if mfg and len(mfg) >= 3 and mfg[0] == beacon_id:
            adverts.append(Advert(adv.local_name, mfg, adv.rssi, device, time.monotonic()))

    async with BleakScanner(callback):
        await asyncio.sleep(seconds)
    return adverts


def collect_adverts(beacon_id: int, seconds: float) -> list[Advert]:
    return asyncio.run(_collect_adverts(beacon_id, seconds))


# Ack policy: called per batch (flag, count, index of this flag's batch);
# returns the ack bytes to send, or None to withhold the ack.
AckPolicy = Callable[[int, int, int], bytes | None]


def ack_all(flag: int, count: int, index: int) -> bytes | None:
    return frames.ack(flag, count)


@dataclass
class Readout:
    tcv: tuple[int, int, int] | None = None  # (uptime, contact count, voltage)
    contacts: list[tuple[int, int, int]] = field(default_factory=list)  # (id2, timer, rssi)
    self_reports: list[int] = field(default_factory=list)
    eco_sessions: list[tuple[int, int]] = field(default_factory=list)
    finished: bool = False
    disconnected_by_peer: bool = False
    notifications: list[bytes] = field(default_factory=list)
    mtu: int = 0
    duration_s: float = 0.0
    undecodable: list[bytes] = field(default_factory=list)


def _decode_into(result: Readout, data: bytes) -> None:
    log = dsa_logger()
    lines = log.parse_payload(data[0], data[1:], b"ID\x00" + data)
    if not lines:
        if data[:1] != bytes([frames.FLAG_CONTROL]):
            result.undecodable.append(data)
        return
    flag = data[0]
    if flag == frames.FLAG_TIME_CONTACTS_VOLTAGE:
        values = [int(line.split(": ")[1]) for line in lines]
        result.tcv = (values[0], values[1], values[2])
    elif flag == frames.FLAG_DATA:
        for line in lines:  # "ID2: a, Timer: t, RSSI: -r"
            parts = [p.split(": ")[1] for p in line.split(", ")]
            result.contacts.append((int(parts[0]), int(parts[1]), -int(parts[2])))
    elif flag == frames.FLAG_SELF_REPORT:
        result.self_reports += [int(line.split(": ")[1]) for line in lines]
    elif flag == frames.FLAG_ECO_LOG:
        for line in lines:
            enter, leave = (int(p.split(": ")[1]) for p in line.split(", "))
            result.eco_sessions.append((enter, leave))
    elif flag == frames.FLAG_CONTROL:
        result.finished = True


async def _readout(
    advert: Advert,
    ack_policy: AckPolicy,
    disconnect_after_batches: int | None,
    send_start: bool,
    enable_notify_first: bool,
    timeout: float,
    extra_writes: list[bytes],
) -> Readout:
    from bleak import BleakClient

    result = Readout()
    disconnected = asyncio.Event()
    queue: asyncio.Queue[bytes] = asyncio.Queue()
    batch_index: dict[int, int] = {}
    start = time.monotonic()

    def on_disconnect(_client):
        result.disconnected_by_peer = True
        disconnected.set()

    async with BleakClient(advert.device, disconnected_callback=on_disconnect) as client:
        result.mtu = getattr(client, "mtu_size", 0)

        def on_notify(_char, data: bytearray):
            queue.put_nowait(bytes(data))

        if not enable_notify_first:
            await client.write_gatt_char(NUS_RX, b"st", response=True)
        await client.start_notify(NUS_TX, on_notify)
        for payload in extra_writes:
            await client.write_gatt_char(NUS_RX, payload, response=True)
        if send_start:
            await client.write_gatt_char(NUS_RX, b"st", response=True)

        batches = 0
        deadline = time.monotonic() + timeout
        while not result.finished and time.monotonic() < deadline and not disconnected.is_set():
            try:
                data = await asyncio.wait_for(queue.get(), 0.5)
            except asyncio.TimeoutError:
                continue
            result.notifications.append(data)
            _decode_into(result, data)
            flag = data[0]
            if flag in (frames.FLAG_DATA, frames.FLAG_SELF_REPORT, frames.FLAG_ECO_LOG) and len(data) >= 2:
                index = batch_index.get(flag, 0)
                batch_index[flag] = index + 1
                ack = ack_policy(flag, data[1], index)
                if ack is not None:
                    await client.write_gatt_char(NUS_RX, ack, response=True)
                batches += 1
                if disconnect_after_batches is not None and batches >= disconnect_after_batches:
                    break
        if result.finished:
            try:
                await asyncio.wait_for(disconnected.wait(), 5.0)
            except asyncio.TimeoutError:
                pass
    result.duration_s = time.monotonic() - start
    return result


def readout(
    advert: Advert,
    ack_policy: AckPolicy = ack_all,
    disconnect_after_batches: int | None = None,
    send_start: bool = True,
    enable_notify_first: bool = True,
    timeout: float = 120.0,
    extra_writes: list[bytes] | None = None,
) -> Readout:
    return asyncio.run(
        _readout(advert, ack_policy, disconnect_after_batches, send_start, enable_notify_first, timeout,
                 extra_writes or [])
    )


async def _idle_connection(advert: Advert, seconds: float) -> float | None:
    """Connect, enable notifications, send nothing. Returns seconds until the
    beacon disconnected, or None if it did not within `seconds`."""
    from bleak import BleakClient

    disconnected = asyncio.Event()
    start = time.monotonic()
    async with BleakClient(advert.device, disconnected_callback=lambda c: disconnected.set()) as client:
        await client.start_notify(NUS_TX, lambda c, d: None)
        try:
            await asyncio.wait_for(disconnected.wait(), seconds)
            return time.monotonic() - start
        except asyncio.TimeoutError:
            return None


def idle_connection(advert: Advert, seconds: float) -> float | None:
    return asyncio.run(_idle_connection(advert, seconds))
