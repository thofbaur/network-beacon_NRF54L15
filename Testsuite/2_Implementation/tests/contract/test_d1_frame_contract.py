"""D1 - the Base (message_parser.c, compiled for the host) and the logger
(dsa_logger.py) decode the same NUS notifications to the same values.

The fixture set is generated, not hand-picked: every flag x edge values,
every beacon ID 0-255, and maximum-size batches. Both outputs are
normalised to tuples such as ("contact", id2, timer, rssi) before
comparison, because the two implementations print different text.
DSA_NUS_FLAG_CONNECT_STATUS is synthesised by the Base itself and never
passes through message_parser.c, so it is excluded. See 1_Specification/D_contract.md.
"""

from __future__ import annotations

import re
import subprocess

import pytest

import frames
from findings import finding
from repo import dsa_logger, firmware_unit_build

E16 = [0, 1, 0xFFFE, 0xFFFF]
E24 = [0, 1, 0xFFFFFE, 0xFFFFFF]
E32 = [0, 1, 0xFFFFFFFE, 0xFFFFFFFF]


def notifications() -> list[tuple[str, int, bytes]]:
    cases: list[tuple[str, int, bytes]] = []
    for v in E32:
        cases.append((f"time-{v}", 1, frames.time(v)))
    for u in E32[::3]:
        for c in E24:
            for mv in E16:
                cases.append((f"tcv-{u}-{c}-{mv}", 2, frames.time_contacts_voltage(u, c, mv)))
    for t in E24:
        for r in (0, 1, 128, 255):
            cases.append((f"contact-t{t}-r{r}", 3, frames.data([(7, t, r)])))
    cases.append(("contact-max-batch", 3, frames.data([(i, i * 997, i) for i in range(48)])))
    cases.append(("contact-empty-batch", 3, frames.data([])))
    for beacon_id in range(256):
        cases.append((f"id1-{beacon_id}", beacon_id, frames.data([(beacon_id, beacon_id, 60)])))
    for t in E24:
        cases.append((f"self-{t}", 4, frames.self_reports([t, 0xFFFFFF - t])))
    cases.append(("self-max-batch", 4, frames.self_reports(list(range(80)))))
    for a, b in [(0, 0), (1, 2), (0xFFFFFE, 0xFFFFFF)]:
        cases.append((f"eco-{a}-{b}", 5, frames.eco_log([(a, b)])))
    cases.append(("eco-max-batch", 5, frames.eco_log([(i, i + 1) for i in range(40)])))
    cases.append(("finished", 6, frames.finished()))
    cases.append(("voltage", 6, frames.voltage(2950)))
    return cases


LOGGER_PATTERNS = [
    (re.compile(r"^Current Time: (\d+)$"), "time"),
    (re.compile(r"^Current Timer: (\d+)$"), "timer"),
    (re.compile(r"^Contact Count: (\d+)$"), "count"),
    (re.compile(r"^Voltage: (\d+)$"), "voltage"),
    (re.compile(r"^ID2: (\d+), Timer: (\d+), RSSI: -(\d+)$"), "contact"),
    (re.compile(r"^Self-report time: (\d+)$"), "self"),
    (re.compile(r"^Eco Session Enter: (\d+), Leave: (\d+)$"), "eco"),
    (re.compile(r"^Transfer complete\. Disconnecting$"), "finished"),
]

BASE_PATTERNS = [
    (re.compile(r"^ID: (\d+) Current Timer:(\d+)$"), lambda m: [("time", int(m[2]))]),
    (
        re.compile(r"^ID: (\d+) Current Timer: (\d+) Contact Count: (\d+) Voltage: (\d+)$"),
        lambda m: [("timer", int(m[2])), ("count", int(m[3])), ("voltage", int(m[4]))],
    ),
    (
        re.compile(r"^ID: (\d+) Contact-ID: (\d+) Timer: (\d+) RSSI: -(\d+)$"),
        lambda m: [("contact", int(m[2]), int(m[3]), int(m[4]))],
    ),
    (re.compile(r"^ID: (\d+) VOLTAGE: (\d+)$"), lambda m: [("voltage", int(m[2]))]),
    (re.compile(r"^ID: (\d+)  Self-report time: (\d+)$"), lambda m: [("self", int(m[2]))]),
    (
        re.compile(r"^ID: (\d+) Eco session enter: (\d+) leave: (\d+)$"),
        lambda m: [("eco", int(m[2]), int(m[3]))],
    ),
    (re.compile(r"^ID: (\d+) Transfer complete\. Disconnecting$"), lambda m: [("finished",)]),
]


def normalise_logger(beacon_id: int, notification: bytes) -> list[tuple]:
    log = dsa_logger()
    out: list[tuple] = []
    for msg in log.MessageParser().feed(frames.raw_frame(beacon_id, notification)):
        assert msg.beacon_id == str(beacon_id)
        for line in msg.output_lines:
            for pattern, kind in LOGGER_PATTERNS:
                m = pattern.match(line)
                if m:
                    out.append((kind, *map(int, m.groups())))
                    break
            else:
                out.append(("unparsed-logger", line))
    return out


def normalise_base(beacon_id: int, lines: list[str]) -> list[tuple]:
    out: list[tuple] = []
    for line in lines:
        for pattern, convert in BASE_PATTERNS:
            m = pattern.match(line)
            if m:
                assert int(m[1]) == beacon_id, f"Base printed ID {m[1]} for beacon {beacon_id}"
                out.extend(convert(m))
                break
        else:
            out.append(("unparsed-base", line))
    return out


@pytest.fixture(scope="module")
def base_outputs() -> dict[str, list[str]]:
    build = firmware_unit_build()
    try:
        exe = build.build_all(["d1_base_parser_dump"])["d1_base_parser_dump"]
    except build.BuildError as exc:
        pytest.fail(f"Base parser dump tool did not build:\n{exc}")
    cases = notifications()
    stdin = "".join(f"{beacon_id} {n.hex()}\n" for _, beacon_id, n in cases)
    result = subprocess.run([str(exe)], input=stdin, capture_output=True, text=True, check=True)
    blocks: dict[int, list[str]] = {}
    current = -1
    for line in result.stdout.splitlines():
        if line.startswith("#"):
            current = int(line[1:])
            blocks[current] = []
        elif line != "FINISHED":
            blocks[current].append(line)
    return {name: blocks.get(i, []) for i, (name, _, _) in enumerate(cases)}


CASES = [c for c in notifications() if c[0] != "voltage"]


@pytest.mark.parametrize("name, beacon_id, notification", CASES, ids=[c[0] for c in CASES])
def test_d1_base_and_logger_agree(base_outputs, name, beacon_id, notification):
    assert normalise_base(beacon_id, base_outputs[name]) == normalise_logger(beacon_id, notification)


@finding(5)
def test_d1_voltage_flag_decoded_by_both(base_outputs):
    """DSA_NUS_FLAG_VOLTAGE (0x03) is defined in common_include.h and decoded
    by the Base; the beacon does not send it today."""
    base = normalise_base(6, base_outputs["voltage"])
    logger = normalise_logger(6, frames.voltage(2950))
    assert base == [("voltage", 2950)]
    assert logger == base, "dsa_logger.py drops DSA_NUS_FLAG_VOLTAGE packets"
