"""Board description for the hardware tests (0_Environment/boards.toml)."""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class Board:
    jlink_serial: int
    beacon_id: int
    vcom: str | None = None


@dataclass(frozen=True)
class Boards:
    dk_a: Board
    dk_b: Board | None
    tag_ids: list[int] = field(default_factory=list)
    ble_adapter: str | None = None


def load_boards(path: Path) -> Boards:
    data = tomllib.loads(path.read_text(encoding="utf-8"))

    def board(section: str) -> Board | None:
        if section not in data:
            return None
        s = data[section]
        return Board(int(s["jlink_serial"]), int(s.get("beacon_id", 0xFF)), s.get("vcom") or None)

    dk_a = board("dk_a")
    if dk_a is None:
        raise ValueError(f"{path}: [dk_a] section is required")
    return Boards(
        dk_a=dk_a,
        dk_b=board("dk_b"),
        tag_ids=[int(i) for i in data.get("tags", {}).get("beacon_ids", [])],
        ble_adapter=data.get("ble", {}).get("adapter") or None,
    )
