"""Serial helpers for DK B: Base raw output (decoded with dsa_logger),
Scan_New_Tags lines, and the Commander (Control with UART commands)."""

from __future__ import annotations

import threading
import time

from repo import dsa_logger


class SerialCapture:
    """Reads a VCOM port in the background."""

    def __init__(self, port: str, baud: int = 115200):
        import serial

        self._serial = serial.Serial(port, baud, timeout=0.1)
        self._data = bytearray()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        while not self._stop.is_set():
            chunk = self._serial.read(4096)
            if chunk:
                with self._lock:
                    self._data += chunk

    def take(self) -> bytes:
        with self._lock:
            data = bytes(self._data)
            self._data.clear()
        return data

    def write(self, data: bytes) -> None:
        self._serial.write(data)

    def close(self) -> None:
        self._stop.set()
        self._thread.join(1.0)
        self._serial.close()


class BaseLink:
    """DK B running the Base (raw output): decodes everything it forwards."""

    def __init__(self, port: str):
        self.capture = SerialCapture(port)
        self.parser = dsa_logger().MessageParser()
        self.messages = []

    def poll(self) -> None:
        self.messages += list(self.parser.feed(self.capture.take()))

    def wait_finished(self, beacon_id: int, timeout: float = 180.0) -> list:
        """Messages of one transfer from `beacon_id`, up to 'finished'."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll()
            for i, m in enumerate(self.messages):
                if m.beacon_id == str(beacon_id) and "Transfer complete. Disconnecting" in m.output_lines:
                    transfer = [x for x in self.messages[: i + 1] if x.beacon_id == str(beacon_id)]
                    self.messages = self.messages[i + 1:]
                    return transfer
            time.sleep(0.1)
        raise TimeoutError(f"no complete transfer from beacon {beacon_id} within {timeout}s")

    def close(self) -> None:
        self.capture.close()


class Commander:
    """DK B running Network_Control_nrf54 with CONFIG_DSA_CONTROL_UART_COMMANDS.

    UART protocol (specified in 1_Specification/H_hil_two_dk.md, implemented by the
    firmware addition): one command per line,
        CMD <target> <param> <value> [<duration_ms>]\\n   (decimal or 0x hex)
    advertises the triple as a "DSZ" command for duration_ms (default 3000)
    and answers "OK" (or "ERR <reason>").
    """

    def __init__(self, port: str):
        self.capture = SerialCapture(port)

    def send(self, target: int, parameter: int, value: int, duration_ms: int = 3000, timeout: float = 10.0) -> None:
        self.capture.take()
        self.capture.write(f"CMD {target} {parameter} {value} {duration_ms}\n".encode())
        deadline = time.monotonic() + timeout
        text = ""
        while time.monotonic() < deadline:
            text += self.capture.take().decode("utf-8", errors="replace")
            if "OK" in text:
                time.sleep(duration_ms / 1000 + 0.5)
                return
            if "ERR" in text:
                raise RuntimeError(f"commander rejected command: {text.strip()}")
            time.sleep(0.05)
        raise TimeoutError(f"commander did not answer: {text!r}")

    def close(self) -> None:
        self.capture.close()


class ScannerLink:
    """DK B running Scan_New_Tags: 'Device,<name>,<address>' lines."""

    def __init__(self, port: str):
        self.capture = SerialCapture(port)
        self.text = ""

    def wait_device(self, name: str, timeout: float = 30.0) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.text += self.capture.take().decode("ascii", errors="ignore")
            for line in self.text.splitlines():
                if line.startswith(f"Device,{name},"):
                    return line.strip()
            time.sleep(0.1)
        raise TimeoutError(f"scanner did not report {name}")

    def close(self) -> None:
        self.capture.close()
