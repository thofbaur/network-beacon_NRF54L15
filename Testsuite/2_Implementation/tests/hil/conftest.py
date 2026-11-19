"""Session fixtures for the hardware tests (classes F, G, H).

The `lab` fixture owns both development kits: it builds the needed images,
reprograms a board only when its role changes, resets DK A and gives access
to RTT, the BLE central and DK B's UART. Board data comes from
0_Environment/boards.toml (copy 0_Environment/boards.example.toml).
"""

from __future__ import annotations

import contextlib
import time
from pathlib import Path

import pytest

import ble
import firmware
from boards import Boards, load_boards
from dut import Dut
from repo import PREREQ, kconfig_has_symbol, BEACON, CONTROL


class Lab:
    def __init__(self, boards: Boards):
        self.boards = boards
        self.current: dict[int, str] = {}
        self._dut_a: Dut | None = None

    # ---- programming ----------------------------------------------------
    def ensure(self, serial: int, image: firmware.Image, erase: bool = False) -> None:
        if self.current.get(serial) == image.name and not erase:
            return
        if serial == self.boards.dk_a.jlink_serial:
            self._close_a()
        firmware.build(image)
        firmware.flash(image, serial, erase=erase)
        self.current[serial] = image.name
        time.sleep(3.0)

    def ensure_a(self, image: firmware.Image, erase: bool = False) -> None:
        self.ensure(self.boards.dk_a.jlink_serial, image, erase)

    def ensure_b(self, image: firmware.Image) -> None:
        if self.boards.dk_b is None:
            pytest.skip("DK B not configured in boards.toml")
        self.ensure(self.boards.dk_b.jlink_serial, image)

    # ---- DK A -------------------------------------------------------------
    @property
    def dut_a(self) -> Dut:
        if self._dut_a is None:
            self._dut_a = Dut(self.boards.dk_a.jlink_serial).connect()
        return self._dut_a

    def _close_a(self) -> None:
        if self._dut_a is not None:
            self._dut_a.close()
            self._dut_a = None

    def reset_a(self, wait_for: str | None = r"Filled network contact buffer", timeout: float = 30.0) -> None:
        """Reset DK A; a hil build refills its synthetic data at boot."""
        self.dut_a.reset()
        if wait_for:
            self.dut_a.wait_for(wait_for, timeout)
        time.sleep(1.0)

    def advert_a(self, timeout: float = 20.0) -> ble.Advert:
        require_ble()
        return ble.find_beacon(self.boards.dk_a.beacon_id, timeout)

    # ---- DK B -------------------------------------------------------------
    @contextlib.contextmanager
    def dk_b_uart(self, cls):
        if self.boards.dk_b is None or not self.boards.dk_b.vcom:
            pytest.skip("DK B vcom not configured in boards.toml")
        link = cls(self.boards.dk_b.vcom)
        try:
            yield link
        finally:
            link.close()

    def close(self) -> None:
        self._close_a()


def require_ble() -> None:
    problem = ble.adapter_problem()
    if problem:
        pytest.skip(problem)


@pytest.fixture(autouse=True)
def _skip_ble_without_adapter(request):
    """Skip `ble` tests before any board is programmed when the computer has
    no Bluetooth adapter (unmarked tests are caught in Lab.advert_a)."""
    if request.node.get_closest_marker("ble") and request.config.getoption("--hil"):
        request.getfixturevalue("boards")  # selects the configured adapter first
        require_ble()


@pytest.fixture(scope="session")
def boards(pytestconfig) -> Boards:
    path = Path(pytestconfig.getoption("--boards"))
    if not path.exists():
        pytest.skip(f"{path} not found: copy 0_Environment/boards.example.toml to 0_Environment/boards.toml")
    loaded = load_boards(path)
    ble.use_adapter(loaded.ble_adapter)
    return loaded


@pytest.fixture(scope="session")
def lab(boards):
    try:
        firmware.west()
        firmware.zephyr_base()
    except firmware.FirmwareError as exc:
        pytest.skip(str(exc))
    lab = Lab(boards)
    yield lab
    lab.close()


@pytest.fixture
def hil_beacon(lab) -> Lab:
    """DK A runs the hil beacon build, freshly reset (500 contacts in RAM)."""
    lab.ensure_a(firmware.beacon_hil())
    lab.reset_a()
    return lab


@pytest.fixture(scope="session")
def require_force_id():
    if not kconfig_has_symbol(BEACON / "Kconfig", "DSA_DEV_FORCE_ID"):
        pytest.skip(f"{PREREQ}: CONFIG_DSA_DEV_FORCE_ID in Network_Beacon_nrf54/Kconfig")


@pytest.fixture(scope="session")
def require_commander():
    if not kconfig_has_symbol(CONTROL / "Kconfig", "DSA_CONTROL_UART_COMMANDS"):
        pytest.skip(f"{PREREQ}: CONFIG_DSA_CONTROL_UART_COMMANDS in Network_Control_nrf54")
