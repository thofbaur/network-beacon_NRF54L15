"""Repository paths and helpers to import the project's scripts as modules."""

from __future__ import annotations

import importlib.util
import re
import subprocess
import sys
from pathlib import Path
from types import ModuleType

IMPLEMENTATION = Path(__file__).resolve().parents[1]  # Testsuite/2_Implementation
TESTSUITE = IMPLEMENTATION.parent
REPO = TESTSUITE.parent
ENVIRONMENT = TESTSUITE / "0_Environment"
SPECS = TESTSUITE / "1_Specification"
RESULTS = TESTSUITE / "3_Results"
BUILD_OUTPUT = TESTSUITE / ".build"  # regenerable, git-ignored

BEACON = REPO / "Network_Beacon_nrf54"
BEACON_SRC = BEACON / "src"
BASE = REPO / "Network_Base_nrf54"
BASE_SRC = BASE / "src"
CONTROL = REPO / "Network_Control_nrf54"
SCAN_TAGS = REPO / "Scan_New_Tags"
SHARED = REPO / "shared"
LOGGER_DIR = REPO / "Network_Logger"
COMMON_INCLUDE = SHARED / "common_include.h"
RADIO_IDS_C = BEACON_SRC / "radio_ids.c"
DECISIONS = BEACON / "DECISIONS.md"
FIRMWARE_UNIT = IMPLEMENTATION / "firmware_unit"
FIRMWARE_TARGET = IMPLEMENTATION / "firmware_target"
CONFIGS = IMPLEMENTATION / "configs"

PREREQ = "PREREQUISITE MISSING"


def load_module(path: Path, name: str | None = None) -> ModuleType:
    """Import a script by file path without running its __main__ block."""
    name = name or f"dsa_test_{path.stem}"
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def dsa_logger() -> ModuleType:
    return load_module(LOGGER_DIR / "dsa_logger.py", "dsa_logger")


def firmware_unit_build() -> ModuleType:
    return load_module(FIRMWARE_UNIT / "build.py", "dsa_firmware_unit_build")


def read_text(path: Path) -> str:
    """Read a text file with normalised line endings."""
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def git(*args: str) -> str:
    result = subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    return result.stdout


def latest_release_tag() -> str | None:
    try:
        tags = git("tag", "--list", "v*", "--sort=-v:refname").split()
    except RuntimeError:
        return None
    return tags[0] if tags else None


def kconfig_has_symbol(kconfig: Path, symbol: str) -> bool:
    """True when `config <symbol>` is defined in the Kconfig file."""
    if not kconfig.exists():
        return False
    return re.search(rf"^\s*config\s+{re.escape(symbol)}\b", read_text(kconfig), re.M) is not None
