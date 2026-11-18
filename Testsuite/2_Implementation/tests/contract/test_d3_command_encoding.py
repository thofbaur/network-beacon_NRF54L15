"""D3 - the Control tool encodes commands exactly as the beacon decodes them.

Checks Network_Control_nrf54/src/main.c statically: HI/LO byte order
(beacon: sys_get_be16), device name (beacon: "DSZ" -> command), target byte,
that every parameter it uses exists in common_include.h, and that the
values it sends are accepted by the beacon. The behaviour of the beacon on
these bytes is tested in C9 (send_command() builds the same layout).
See 1_Specification/D_contract.md.
"""

from __future__ import annotations

import re

import pytest

from cparse import parse_defines, parse_known_device_table, strip_c_comments
from repo import BEACON_SRC, COMMON_INCLUDE, CONTROL, PREREQ, RADIO_IDS_C, kconfig_has_symbol, read_text

CONTROL_MAIN = CONTROL / "src" / "main.c"

# Values the beacon accepts per parameter (from led.c/network.c/radio.c/motion.c).
BOOLEAN_PARAMS = {"P_MAIN_LED_ACTIVE", "P_MAIN_FIREWORK_ACTIVE", "P_TRACKING_ACTIVE", "P_MOTION_ACTIVE", "P_SET_RAD_ACTIVE"}
NONZERO_PARAMS = {"P_MAIN_LED_INTERVAL_S", "P_ECO_SCAN_PERIOD_S", "P_MOTION_INACTIVITY_TIMEOUT_S"}
BYTE_PARAMS = {"P_RSSI_NETWORK", "P_RSSI_LOCATION"}
ADV_MS_PARAMS = {"P_ADV_INTERVAL_MS", "P_ADV_INTERVAL_ECO_MS"}
SCAN_MS_PARAMS = {"P_SCAN_INTERVAL_MS", "P_SCAN_WINDOW_MS", "P_ECO_SCAN_WINDOW_MS"}


@pytest.fixture(scope="module")
def control_source() -> str:
    return read_text(CONTROL_MAIN)


def macro_body(source: str, name: str) -> str:
    m = re.search(rf"#define\s+{name}\(v\)\s+(.+)", source)
    assert m, f"macro {name}(v) not found in Control main.c"
    return m.group(1).replace(" ", "")


def active_commands(source: str) -> list[tuple[str, str, str]]:
    m = re.search(r"static const uint8_t mfg_data\[\]\s*=\s*\{(.*?)\};", source, re.S)
    assert m, "mfg_data[] not found in Control main.c"
    body = strip_c_comments(m.group(1))
    return re.findall(r"(P_\w+)\s*,\s*HI\(([^)]+)\)\s*,\s*LO\(([^)]+)\)", body)


def test_d3_value_byte_order_is_big_endian(control_source):
    hi = macro_body(control_source, "HI")
    lo = macro_body(control_source, "LO")
    assert ">>8" in hi and "0XFF" in hi.upper(), f"HI(v) = {hi}"
    assert ">>" not in lo and "0XFF" in lo.upper(), f"LO(v) = {lo}"
    assert "sys_get_be16(&data[offset + 1])" in read_text(BEACON_SRC / "radio.c")


def test_d3_triples_are_parameter_hi_lo(control_source):
    for param, hi_value, lo_value in active_commands(control_source):
        assert hi_value == lo_value, f"{param}: HI({hi_value}) and LO({lo_value}) encode different values"


def test_d3_device_name_triggers_command_handling(control_source):
    assert re.search(r'#define\s+DEVICE_NAME\s+"DSZ"', control_source)
    assert '{ .name = "DSZ", \t.action = ACTION_DSZ }' in read_text(BEACON_SRC / "radio.c").replace("    ", "")


def test_d3_target_is_known_device_or_broadcast(control_source):
    m = re.search(r"#define\s+CMD_TARGET\s+(0x[0-9a-fA-F]+|\d+)", control_source)
    assert m
    target = int(m.group(1), 0)
    ids = {d.device_id for d in parse_known_device_table(RADIO_IDS_C)}
    assert target == 0xFF or target in ids, f"CMD_TARGET {target} is neither 0xFF nor a known beacon ID"


def test_d3_parameters_exist_in_protocol_header(control_source):
    header = parse_defines(COMMON_INCLUDE)
    used = re.findall(r"\b(P_[A-Z_]+)\b", strip_c_comments(control_source))
    unknown = sorted({p for p in used if p not in header})
    assert not unknown, f"Control uses parameters not in common_include.h: {unknown}"


def test_d3_active_command_values_are_accepted_by_beacon(control_source):
    problems = []
    for param, value_text, _ in active_commands(control_source):
        value = int(value_text, 0)
        if param in BOOLEAN_PARAMS and value not in (0, 1):
            problems.append(f"{param}={value} (beacon accepts 0/1)")
        if param in NONZERO_PARAMS and value == 0:
            problems.append(f"{param}=0 (beacon rejects 0)")
        if param in BYTE_PARAMS and value > 255:
            problems.append(f"{param}={value} (beacon accepts 0-255)")
        if param in ADV_MS_PARAMS and not 0x20 <= value * 8 // 5 <= 0x4000:
            problems.append(f"{param}={value} ms (beacon accepts 20-10240 ms)")
        if param in SCAN_MS_PARAMS and not 4 <= value * 8 // 5 <= 0x4000:
            problems.append(f"{param}={value} ms (beacon accepts 3-10240 ms)")
    assert not problems, "; ".join(problems)


def test_d3_commander_uart_option_exists():
    """The hardware command tests (H1-H3, H10) drive Control over UART."""
    if not kconfig_has_symbol(CONTROL / "Kconfig", "DSA_CONTROL_UART_COMMANDS"):
        pytest.skip(f"{PREREQ}: CONFIG_DSA_CONTROL_UART_COMMANDS in Network_Control_nrf54")
    assert "DSA_CONTROL_UART_COMMANDS" in read_text(CONTROL_MAIN)
