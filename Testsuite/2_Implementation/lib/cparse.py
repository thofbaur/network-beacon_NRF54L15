"""Small parsers for the C sources the static tests inspect."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from repo import read_text

_DEFINE_RE = re.compile(r"^\s*#define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$", re.M)


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def parse_defines(path: Path) -> dict[str, int]:
    """Evaluate integer #defines (hex/dec literals, BIT(n), +, references)."""
    raw: dict[str, str] = {}
    for m in _DEFINE_RE.finditer(read_text(path)):
        raw[m.group(1)] = m.group(2).strip()

    values: dict[str, int] = {}

    def evaluate(name: str, depth: int = 0) -> int | None:
        if name in values:
            return values[name]
        if depth > 20 or name not in raw:
            return None
        expr = raw[name]
        expr = re.sub(r"\bBIT\((\d+)\)", lambda m: str(1 << int(m.group(1))), expr)
        expr = re.sub(r"(\d+)[uU]\b", r"\1", expr)
        for ref in re.findall(r"\b[A-Za-z_]\w*\b", expr):
            ref_value = evaluate(ref, depth + 1)
            if ref_value is None:
                return None
            expr = re.sub(rf"\b{ref}\b", str(ref_value), expr)
        if not re.fullmatch(r"[\s()0-9a-fA-Fx+\-*/<>|&]+", expr):
            return None
        try:
            value = int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - digits/operators only
        except Exception:  # pragma: no cover - unexpected expression shapes
            return None
        values[name] = value
        return value

    for name in raw:
        evaluate(name)
    return values


@dataclass(frozen=True)
class KnownDevice:
    addr_type: str
    addr: tuple[int, ...]  # val[0]..val[5] as written in the table
    device_id: int
    comment: str
    line: int


_ENTRY_RE = re.compile(
    r"\.type\s*=\s*(BT_ADDR_LE_\w+)\s*,\s*"
    r"\.a\s*=\s*\{\s*\.val\s*=\s*\{([^}]*)\}\s*\}\s*(?://\s*([^\n]*))?"
    r".*?\.id\s*=\s*(\d+)",
    re.S,
)


def parse_known_device_table(path: Path) -> list[KnownDevice]:
    text = read_text(path)
    devices = []
    for m in _ENTRY_RE.finditer(text):
        values = tuple(int(v.strip(), 16) for v in m.group(2).split(",") if v.strip())
        devices.append(
            KnownDevice(
                addr_type=m.group(1),
                addr=values,
                device_id=int(m.group(4)),
                comment=(m.group(3) or "").strip(),
                line=text.count("\n", 0, m.start()) + 1,
            )
        )
    return devices


def strip_c_comments(text: str) -> str:
    return _strip_comments(text)
