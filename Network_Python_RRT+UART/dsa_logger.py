#!/usr/bin/env python3
"""Log line-oriented data from UART or SEGGER RTT."""

from __future__ import annotations

import argparse
import re
import sys
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable, Optional


DEFAULT_UART_PORT = "COM11"
DEFAULT_UART_BAUD = 115200
DEFAULT_RTT_DEVICE = "nRF54L15_M33"
DEFAULT_RTT_CHANNEL = 0
DEFAULT_RTT_INTERFACE = "SWD"
DEFAULT_RTT_SPEED = 4000
READ_CHUNK_SIZE = 1024
IDLE_SLEEP_SECONDS = 0.01
DSA_DATA_SET_LEN = 5
DSA_ECO_LOG_ENTRY_LEN = 6
DSA_SELF_REPORT_ENTRY_LEN = 3
# Upper bound on a single raw NUS frame's flag+payload region, mirroring
# Network_Base_nrf54's CONFIG_BT_L2CAP_TX_MTU (the base truncates/forwards
# each NUS notification verbatim at this size). Used to reject an implausible
# eco-log entry count immediately instead of stalling until enough bytes
# accumulate to test a frame size that can never be valid - e.g. if the
# beacon is still running firmware from before the count byte was added.
MAX_NUS_RAW_PAYLOAD_LEN = 247
NETWORK_BASE_SUBSTRING = "network_base:"
DEFAULT_OMIT_NETWORK_BASE_LINES = True
DEFAULT_OMIT_TIMESTAMPED_LINES = True
TIMESTAMPED_LINE_PATTERN = re.compile(r"\[\d{2}:\d{2}:\d{2}\.\d{3},\d{3}\]")
MESSAGE_PREFIX = b"ID"
MESSAGE_TERMINATOR = b"\r\n"
DSA_NUS_FLAGS = {
    0x01: "DSA_NUS_FLAG_TIME",
    0x02: "DSA_NUS_FLAG_DATA",
    0x03: "DSA_NUS_FLAG_VOLTAGE",
    0x04: "DSA_NUS_FLAG_CONTROL",
    0x05: "DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE",
    0x06: "DSA_NUS_FLAG_SELF_REPORT",
    0x07: "DSA_NUS_FLAG_ECO_LOG",
}
DSA_NUS_FLAG_TIME = 0x01
DSA_NUS_FLAG_DATA = 0x02
DSA_NUS_FLAG_VOLTAGE = 0x03
DSA_NUS_FLAG_CONTROL = 0x04
DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE = 0x05
DSA_NUS_FLAG_SELF_REPORT = 0x06
DSA_NUS_FLAG_ECO_LOG = 0x07
DSA_NUS_PAYLOAD_LENGTHS = {
    DSA_NUS_FLAG_TIME: 4,
    DSA_NUS_FLAG_VOLTAGE: 2,
    DSA_NUS_FLAG_CONTROL: 8,
    DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE: 9,
}


class InputSource(ABC):
    """Byte-oriented data source."""

    @abstractmethod
    def open(self) -> None:
        """Open the input source."""

    @abstractmethod
    def read(self) -> bytes:
        """Read available data. Returns an empty byte string when idle."""

    @abstractmethod
    def close(self) -> None:
        """Close the input source."""


class UartInput(InputSource):
    def __init__(self, port: str, baud: int) -> None:
        self.port = port
        self.baud = baud
        self._serial = None

    def open(self) -> None:
        try:
            import serial
        except ImportError as exc:
            raise RuntimeError("pyserial is required for UART input. Install requirements.txt.") from exc

        self._serial = serial.Serial(self.port, self.baud, timeout=0.1)

    def read(self) -> bytes:
        if self._serial is None:
            raise RuntimeError("UART input is not open.")

        waiting = self._serial.in_waiting
        size = waiting if waiting > 0 else 1
        return self._serial.read(size)

    def close(self) -> None:
        if self._serial is not None:
            self._serial.close()
            self._serial = None


class RttInput(InputSource):
    def __init__(self, device: str, channel: int, interface: str, speed: int) -> None:
        self.device = device
        self.channel = channel
        self.interface = interface
        self.speed = speed
        self._jlink = None

    def open(self) -> None:
        try:
            import pylink
        except ImportError as exc:
            raise RuntimeError("pylink-square is required for RTT input. Install requirements.txt.") from exc

        interface_name = self.interface.upper()
        try:
            interface = getattr(pylink.enums.JLinkInterfaces, interface_name)
        except AttributeError as exc:
            valid = ", ".join(name for name in dir(pylink.enums.JLinkInterfaces) if name.isupper())
            raise RuntimeError(f"Unknown J-Link interface '{self.interface}'. Valid values include: {valid}") from exc

        self._jlink = pylink.JLink()
        try:
            self._jlink.open()
            self._jlink.set_tif(interface)
            self._jlink.connect(self.device, speed=self.speed)
            self._jlink.rtt_start()
        except pylink.errors.JLinkException as exc:
            self.close()
            hint = ""
            if "Unsupported device selected" in str(exc):
                hint = " Try --rtt-device nRF54L15_M33 for the Cortex-M33 application core."
            raise RuntimeError(
                "Could not open RTT connection. "
                f"Device={self.device}, interface={interface_name}, speed={self.speed}, channel={self.channel}. "
                f"J-Link error: {exc}.{hint}"
            ) from exc

    def read(self) -> bytes:
        if self._jlink is None:
            raise RuntimeError("RTT input is not open.")

        data = self._jlink.rtt_read(self.channel, READ_CHUNK_SIZE)
        if isinstance(data, bytes):
            return data
        return bytes(data)

    def close(self) -> None:
        if self._jlink is not None:
            try:
                self._jlink.rtt_stop()
            finally:
                self._jlink.close()
                self._jlink = None


@dataclass(frozen=True)
class ParsedMessage:
    beacon_id: str
    flag_value: int
    flag_name: str
    payload: bytes
    output_lines: tuple[str, ...]


class MessageParser:
    """Converts raw bytes into structured DSA messages."""

    def __init__(
        self,
        encoding: str = "utf-8",
        exclude_substrings: Iterable[str] = (),
        omit_network_base_lines: bool = DEFAULT_OMIT_NETWORK_BASE_LINES,
        omit_timestamped_lines: bool = DEFAULT_OMIT_TIMESTAMPED_LINES,
    ) -> None:
        self.encoding = encoding
        self.exclude_substrings = tuple(exclude_substrings)
        self.omit_network_base_lines = omit_network_base_lines
        self.omit_timestamped_lines = omit_timestamped_lines
        self._buffer = bytearray()

    def feed(self, data: bytes) -> Iterable[ParsedMessage]:
        self._buffer.extend(data)

        while True:
            prefix_index = self._buffer.find(MESSAGE_PREFIX)
            if prefix_index < 0:
                self._keep_possible_split_prefix()
                break

            if prefix_index > 0:
                discarded = bytes(self._buffer[:prefix_index])
                del self._buffer[:prefix_index]
                if not self._can_discard(discarded):
                    continue

            parse_result = self._parse_next_message()
            if parse_result is None:
                break
            parsed_message, should_emit = parse_result
            if should_emit:
                yield parsed_message

    def flush(self) -> Optional[ParsedMessage]:
        self._buffer.clear()
        return None

    def _should_emit(self, line: str) -> bool:
        if self.omit_network_base_lines and NETWORK_BASE_SUBSTRING in line:
            return False
        if any(excluded in line for excluded in self.exclude_substrings):
            return False
        if self.omit_timestamped_lines and TIMESTAMPED_LINE_PATTERN.search(line):
            return False
        return True

    def _parse_next_message(self) -> Optional[tuple[ParsedMessage, bool]]:
        frame_header_size = len(MESSAGE_PREFIX) + 2
        if len(self._buffer) < frame_header_size:
            return None

        beacon_id = format_beacon_id(self._buffer[2])
        flag_value = self._buffer[3]
        if flag_value == DSA_NUS_FLAG_DATA:
            return self._parse_data_message(beacon_id, frame_header_size)
        if flag_value == DSA_NUS_FLAG_ECO_LOG:
            return self._parse_eco_log_message(beacon_id, frame_header_size)
        if flag_value == DSA_NUS_FLAG_SELF_REPORT:
            return self._parse_self_report_message(beacon_id, frame_header_size)

        payload_len = DSA_NUS_PAYLOAD_LENGTHS.get(flag_value)
        if payload_len is None:
            del self._buffer[:frame_header_size]
            return ParsedMessage(beacon_id, flag_value, f"UNKNOWN_FLAG_0x{flag_value:02X}", b"", ()), False

        frame_size = frame_header_size + payload_len
        total_size = frame_size + len(MESSAGE_TERMINATOR)
        if len(self._buffer) < total_size:
            return None
        if self._buffer[frame_size:total_size] != MESSAGE_TERMINATOR:
            self._discard_invalid_frame()
            return ParsedMessage(beacon_id, flag_value, DSA_NUS_FLAGS[flag_value], b"", ()), False

        raw_message = bytes(self._buffer[:frame_size])
        payload = bytes(self._buffer[frame_header_size:frame_size])
        del self._buffer[:total_size]

        output_lines = parse_payload(flag_value, payload, raw_message)
        return ParsedMessage(beacon_id, flag_value, DSA_NUS_FLAGS[flag_value], payload, output_lines), bool(output_lines)

    def _parse_data_message(self, beacon_id: str, frame_header_size: int) -> Optional[tuple[ParsedMessage, bool]]:
        contact_count_size = 1
        if len(self._buffer) < frame_header_size + contact_count_size:
            return None

        contact_count = self._buffer[frame_header_size]
        payload_len = contact_count_size + (contact_count * DSA_DATA_SET_LEN)
        frame_size = frame_header_size + payload_len
        total_size = frame_size + len(MESSAGE_TERMINATOR)
        if len(self._buffer) < total_size:
            return None
        if self._buffer[frame_size:total_size] != MESSAGE_TERMINATOR:
            self._discard_invalid_frame()
            return ParsedMessage(beacon_id, DSA_NUS_FLAG_DATA, DSA_NUS_FLAGS[DSA_NUS_FLAG_DATA], b"", ()), False

        raw_message = bytes(self._buffer[:frame_size])
        payload = bytes(self._buffer[frame_header_size:frame_size])
        del self._buffer[:total_size]

        output_lines = parse_payload(DSA_NUS_FLAG_DATA, payload, raw_message)
        return (
            ParsedMessage(beacon_id, DSA_NUS_FLAG_DATA, DSA_NUS_FLAGS[DSA_NUS_FLAG_DATA], payload, output_lines),
            bool(output_lines),
        )

    def _parse_eco_log_message(self, beacon_id: str, frame_header_size: int) -> Optional[tuple[ParsedMessage, bool]]:
        entry_count_size = 1
        if len(self._buffer) < frame_header_size + entry_count_size:
            return None

        entry_count = self._buffer[frame_header_size]
        payload_len = entry_count_size + (entry_count * DSA_ECO_LOG_ENTRY_LEN)
        flag_size = 1
        if flag_size + payload_len > MAX_NUS_RAW_PAYLOAD_LEN:
            # Implausible entry count for a single NUS notification - most
            # likely a stale beacon still sending the pre-count-byte format,
            # where this byte is actually the first entry's timer high byte.
            # Resync now instead of waiting for a frame size that can never
            # complete.
            self._discard_invalid_frame()
            return ParsedMessage(beacon_id, DSA_NUS_FLAG_ECO_LOG, DSA_NUS_FLAGS[DSA_NUS_FLAG_ECO_LOG], b"", ()), False

        frame_size = frame_header_size + payload_len
        total_size = frame_size + len(MESSAGE_TERMINATOR)
        if len(self._buffer) < total_size:
            return None
        if self._buffer[frame_size:total_size] != MESSAGE_TERMINATOR:
            self._discard_invalid_frame()
            return ParsedMessage(beacon_id, DSA_NUS_FLAG_ECO_LOG, DSA_NUS_FLAGS[DSA_NUS_FLAG_ECO_LOG], b"", ()), False

        raw_message = bytes(self._buffer[:frame_size])
        payload = bytes(self._buffer[frame_header_size:frame_size])
        del self._buffer[:total_size]

        output_lines = parse_payload(DSA_NUS_FLAG_ECO_LOG, payload, raw_message)
        return (
            ParsedMessage(beacon_id, DSA_NUS_FLAG_ECO_LOG, DSA_NUS_FLAGS[DSA_NUS_FLAG_ECO_LOG], payload, output_lines),
            bool(output_lines),
        )

    def _parse_self_report_message(self, beacon_id: str, frame_header_size: int) -> Optional[tuple[ParsedMessage, bool]]:
        report_count_size = 1
        if len(self._buffer) < frame_header_size + report_count_size:
            return None

        report_count = self._buffer[frame_header_size]
        payload_len = report_count_size + (report_count * DSA_SELF_REPORT_ENTRY_LEN)
        flag_size = 1
        if flag_size + payload_len > MAX_NUS_RAW_PAYLOAD_LEN:
            # Implausible report count for a single NUS notification - most
            # likely a stale beacon still sending the pre-count-byte format,
            # where this byte is actually the first report's timer high byte.
            # Resync now instead of waiting for a frame size that can never
            # complete.
            self._discard_invalid_frame()
            return ParsedMessage(beacon_id, DSA_NUS_FLAG_SELF_REPORT, DSA_NUS_FLAGS[DSA_NUS_FLAG_SELF_REPORT], b"", ()), False

        frame_size = frame_header_size + payload_len
        total_size = frame_size + len(MESSAGE_TERMINATOR)
        if len(self._buffer) < total_size:
            return None
        if self._buffer[frame_size:total_size] != MESSAGE_TERMINATOR:
            self._discard_invalid_frame()
            return ParsedMessage(beacon_id, DSA_NUS_FLAG_SELF_REPORT, DSA_NUS_FLAGS[DSA_NUS_FLAG_SELF_REPORT], b"", ()), False

        raw_message = bytes(self._buffer[:frame_size])
        payload = bytes(self._buffer[frame_header_size:frame_size])
        del self._buffer[:total_size]

        output_lines = parse_payload(DSA_NUS_FLAG_SELF_REPORT, payload, raw_message)
        return (
            ParsedMessage(beacon_id, DSA_NUS_FLAG_SELF_REPORT, DSA_NUS_FLAGS[DSA_NUS_FLAG_SELF_REPORT], payload, output_lines),
            bool(output_lines),
        )

    def _discard_invalid_frame(self) -> None:
        next_prefix_index = self._buffer.find(MESSAGE_PREFIX, 1)
        if next_prefix_index < 0:
            self._keep_possible_split_prefix()
        else:
            del self._buffer[:next_prefix_index]

    def _keep_possible_split_prefix(self) -> None:
        keep_bytes = len(MESSAGE_PREFIX) - 1
        if keep_bytes <= 0:
            self._buffer.clear()
        elif len(self._buffer) > keep_bytes:
            del self._buffer[:-keep_bytes]

    def _can_discard(self, discarded: bytes) -> bool:
        text = discarded.decode(self.encoding, errors="replace")
        return self._should_emit(text) or not text.strip()


class LogOutput:
    """Timestamped parsed-message output."""

    def __init__(self, directory: Path) -> None:
        self.directory = directory
        self.path = directory / f"dsa_{datetime.now().strftime('%Y%m%d_%H%M')}.log"
        self._file = None

    def open(self) -> None:
        self._file = self.path.open("a", encoding="utf-8", newline="\n")

    def write_message(self, message: ParsedMessage) -> None:
        if self._file is None:
            raise RuntimeError("Log output is not open.")

        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        for line in message.output_lines:
            self._file.write(f"{timestamp},ID:{message.beacon_id},{line}\n")
        self._file.flush()

    def close(self) -> None:
        if self._file is not None:
            self._file.close()
            self._file = None


def format_beacon_id(beacon_id_byte: int) -> str:
    if ord("0") <= beacon_id_byte <= ord("9"):
        return chr(beacon_id_byte)
    return str(beacon_id_byte)


def parse_payload(flag_value: int, payload: bytes, raw_message: bytes) -> tuple[str, ...]:
    if not validate_payload(flag_value, payload):
        return ()
    if flag_value == DSA_NUS_FLAG_TIME:
        return (f"Current Time: {uint32_be_decode(payload)}",)
    if flag_value == DSA_NUS_FLAG_DATA:
        return parse_data_payload(payload)
    if flag_value == DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE:
        return (
            f"Current Timer: {uint32_be_decode(payload[0:4])}",
            f"Contact Count: {uint24_be_decode(payload[4:7])}",
            f"Voltage: {uint16_be_decode(payload[7:9])}",
        )
    if flag_value == DSA_NUS_FLAG_ECO_LOG:
        return parse_eco_log_payload(payload)
    if flag_value == DSA_NUS_FLAG_SELF_REPORT:
        return parse_self_report_payload(payload)
    return ()


def validate_payload(flag_value: int, payload: bytes) -> bool:
    if flag_value == DSA_NUS_FLAG_DATA:
        if len(payload) < 1:
            return False
        contact_count = payload[0]
        return len(payload[1:]) == contact_count * DSA_DATA_SET_LEN

    if flag_value == DSA_NUS_FLAG_ECO_LOG:
        if len(payload) < 1:
            return False
        entry_count = payload[0]
        return len(payload[1:]) == entry_count * DSA_ECO_LOG_ENTRY_LEN

    if flag_value == DSA_NUS_FLAG_SELF_REPORT:
        if len(payload) < 1:
            return False
        report_count = payload[0]
        return len(payload[1:]) == report_count * DSA_SELF_REPORT_ENTRY_LEN

    expected_len = DSA_NUS_PAYLOAD_LENGTHS.get(flag_value)
    return expected_len is not None and len(payload) == expected_len


def parse_data_payload(payload: bytes) -> tuple[str, ...]:
    contact_count = payload[0]
    contact_payload = payload[1:]
    lines = []
    for contact_index in range(contact_count):
        offset = contact_index * DSA_DATA_SET_LEN
        data_set = contact_payload[offset : offset + DSA_DATA_SET_LEN]
        id2 = data_set[0]
        timer = uint24_be_decode(data_set[1:4])
        rssi = data_set[4]
        lines.append(f"ID2: {id2}, Timer: {timer}, RSSI: -{rssi}")
    return tuple(lines)


def parse_eco_log_payload(payload: bytes) -> tuple[str, ...]:
    entry_count = payload[0]
    entry_payload = payload[1:]
    lines = []
    for entry_index in range(entry_count):
        offset = entry_index * DSA_ECO_LOG_ENTRY_LEN
        entry = entry_payload[offset : offset + DSA_ECO_LOG_ENTRY_LEN]
        enter_time = uint24_be_decode(entry[0:3])
        leave_time = uint24_be_decode(entry[3:6])
        lines.append(f"Eco Session Enter: {enter_time}, Leave: {leave_time}")
    return tuple(lines)


def parse_self_report_payload(payload: bytes) -> tuple[str, ...]:
    report_count = payload[0]
    report_payload = payload[1:]
    lines = []
    for report_index in range(report_count):
        offset = report_index * DSA_SELF_REPORT_ENTRY_LEN
        entry = report_payload[offset : offset + DSA_SELF_REPORT_ENTRY_LEN]
        timer = uint24_be_decode(entry)
        lines.append(f"Self-report time: {timer}")
    return tuple(lines)


def uint24_be_decode(data: bytes) -> int:
    return (data[0] << 16) | (data[1] << 8) | data[2]


def uint32_be_decode(data: bytes) -> int:
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]


def uint16_be_decode(data: bytes) -> int:
    return (data[0] << 8) | data[1]


def build_input_source(args: argparse.Namespace) -> InputSource:
    if args.transport == "uart":
        return UartInput(args.port, args.baud)

    return RttInput(args.rtt_device, args.rtt_channel, args.rtt_interface, args.rtt_speed)


def run_logger(input_source: InputSource, parser: MessageParser, output: LogOutput) -> None:
    input_source.open()
    output.open()

    try:
        print(f"Logging to {output.path}")
        while True:
            data = input_source.read()
            if not data:
                time.sleep(IDLE_SLEEP_SECONDS)
                continue

            for message in parser.feed(data):
                output.write_message(message)
    except KeyboardInterrupt:
        trailing_message = parser.flush()
        if trailing_message is not None:
            output.write_message(trailing_message)
        print("\nStopped.")
    finally:
        output.close()
        input_source.close()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Log NRF54L15dk data from UART or SEGGER RTT.")
    parser.add_argument("--transport", choices=("uart", "rtt"), default="uart")
    parser.add_argument("--port", default=DEFAULT_UART_PORT, help=f"UART port. Default: {DEFAULT_UART_PORT}")
    parser.add_argument("--baud", type=int, default=DEFAULT_UART_BAUD, help=f"UART baud rate. Default: {DEFAULT_UART_BAUD}")
    parser.add_argument("--rtt-device", default=DEFAULT_RTT_DEVICE, help=f"RTT target device. Default: {DEFAULT_RTT_DEVICE}")
    parser.add_argument("--rtt-channel", type=int, default=DEFAULT_RTT_CHANNEL, help=f"RTT up channel. Default: {DEFAULT_RTT_CHANNEL}")
    parser.add_argument("--rtt-interface", default=DEFAULT_RTT_INTERFACE, help=f"J-Link interface. Default: {DEFAULT_RTT_INTERFACE}")
    parser.add_argument("--rtt-speed", type=int, default=DEFAULT_RTT_SPEED, help=f"J-Link speed value. Default: {DEFAULT_RTT_SPEED}")
    parser.add_argument("--encoding", default="utf-8", help="Input text encoding. Default: utf-8")
    parser.add_argument(
        "--exclude",
        action="append",
        default=[],
        help="Additional substring to omit from the log. Can be passed multiple times.",
    )
    parser.add_argument(
        "--omit-network-base",
        action=argparse.BooleanOptionalAction,
        default=DEFAULT_OMIT_NETWORK_BASE_LINES,
        help="Omit lines containing network_base:. Default: enabled",
    )
    parser.add_argument(
        "--omit-timestamped-lines",
        action=argparse.BooleanOptionalAction,
        default=DEFAULT_OMIT_TIMESTAMPED_LINES,
        help="Omit lines containing timestamps like [00:03:24.911,418]. Default: enabled",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    input_source = build_input_source(args)
    message_parser = MessageParser(
        args.encoding,
        args.exclude,
        args.omit_network_base,
        args.omit_timestamped_lines,
    )
    output = LogOutput(Path.cwd())

    try:
        run_logger(input_source, message_parser, output)
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
