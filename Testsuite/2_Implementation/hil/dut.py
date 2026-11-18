"""Debugger access to a development kit over its on-board J-Link: reset,
halt, and the RTT console (printk output of hil.conf builds)."""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field

DEVICE = "nRF54L15_M33"


@dataclass
class Dut:
    jlink_serial: int
    _jlink: object = None
    _text: str = ""
    lines: list[str] = field(default_factory=list)

    def connect(self) -> "Dut":
        import pylink

        jlink = pylink.JLink()
        jlink.open(serial_no=self.jlink_serial)
        jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
        jlink.connect(DEVICE, speed=4000)
        jlink.rtt_start()
        self._jlink = jlink
        return self

    def close(self) -> None:
        if self._jlink is not None:
            try:
                self._jlink.rtt_stop()
            finally:
                self._jlink.close()
                self._jlink = None

    def reset(self, clear_log: bool = True) -> None:
        """Hardware reset; the firmware runs again from boot."""
        if clear_log:
            self.clear()
        self._jlink.reset(halt=False)
        self._jlink.rtt_start()

    def poll(self) -> None:
        data = self._jlink.rtt_read(0, 4096)
        if data:
            self._text += bytes(data).decode("utf-8", errors="replace")
            *complete, self._text = self._text.split("\n")
            self.lines += [line.rstrip("\r") for line in complete]

    def clear(self) -> None:
        self.poll()
        self.lines.clear()

    def wait_for(self, pattern: str, timeout: float = 10.0) -> re.Match:
        regex = re.compile(pattern)
        deadline = time.monotonic() + timeout
        checked = 0
        while time.monotonic() < deadline:
            self.poll()
            for line in self.lines[checked:]:
                m = regex.search(line)
                if m:
                    return m
            checked = len(self.lines)
            time.sleep(0.05)
        raise TimeoutError(f"no RTT line matching {pattern!r} within {timeout}s; last lines: {self.lines[-10:]}")

    def collect(self, seconds: float) -> list[str]:
        start = len(self.lines)
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.poll()
            time.sleep(0.05)
        return self.lines[start:]
