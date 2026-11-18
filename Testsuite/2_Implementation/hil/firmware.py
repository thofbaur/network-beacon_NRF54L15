"""Build and flash the firmware images the build checks and hardware tests
need, with `west`.

The nRF Connect SDK toolchain environment is applied automatically from the
toolchain's own environment.json (found in $DSA_NCS_TOOLCHAIN, C:/ncs/toolchains,
~/ncs/toolchains or /opt/nordic/ncs/toolchains), so no special shell is
needed. ZEPHYR_BASE defaults to the newest SDK found next to it
(C:/ncs/v*/zephyr, ~/ncs/v*/zephyr, /opt/nordic/ncs/v*/zephyr).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from repo import BASE, BEACON, BUILD_OUTPUT, CONFIGS, CONTROL, FIRMWARE_TARGET, REPO, SCAN_TAGS

DK = "nrf54l15dk/nrf54l15/cpuapp/ns"
TAG = "nrf54l15tag/nrf54l15/cpuapp/ns"
BUILD_ROOT = BUILD_OUTPUT / "west"


class FirmwareError(RuntimeError):
    pass


@dataclass(frozen=True)
class Image:
    """A west build configuration."""

    name: str
    app: Path
    board: str = DK
    confs: tuple[Path, ...] = ()
    defines: tuple[str, ...] = ()  # extra -D<KEY>=<VALUE> CMake/Kconfig definitions

    @property
    def build_dir(self) -> Path:
        return BUILD_ROOT / self.name


NCS_ROOTS = [Path("C:/ncs"), Path.home() / "ncs", Path("/opt/nordic/ncs")]


def toolchain_dir() -> Path | None:
    explicit = os.environ.get("DSA_NCS_TOOLCHAIN")
    if explicit:
        return Path(explicit)
    for root in NCS_ROOTS:
        for env_json in sorted((root / "toolchains").glob("*/environment.json")):
            return env_json.parent
    return None


def _env() -> dict[str, str]:
    """Process environment with the NCS toolchain applied."""
    env = dict(os.environ)
    tc = toolchain_dir()
    if tc is not None and (tc / "environment.json").exists():
        for var in json.loads((tc / "environment.json").read_text())["env_vars"]:
            if var["type"] == "string":
                env[var["key"]] = var["value"]
                continue
            paths = os.pathsep.join(str(tc / p) for p in var["values"])
            if var.get("existing_value_treatment") == "prepend_to" and env.get(var["key"]):
                env[var["key"]] = paths + os.pathsep + env[var["key"]]
            else:
                env[var["key"]] = paths
    env.setdefault("ZEPHYR_BASE", str(zephyr_base()))
    return env


def west() -> str:
    path = shutil.which("west", path=_env().get("PATH"))
    if not path:
        raise FirmwareError("'west' not found: install the nRF Connect SDK toolchain or set DSA_NCS_TOOLCHAIN")
    return path


def zephyr_base() -> Path:
    env = os.environ.get("ZEPHYR_BASE")
    if env:
        return Path(env)
    for root in NCS_ROOTS:
        for candidate in sorted(root.glob("v*/zephyr"), reverse=True):
            if candidate.is_dir():
                return candidate
    raise FirmwareError("ZEPHYR_BASE not set and no nRF Connect SDK found in C:/ncs, ~/ncs or /opt/nordic/ncs")


def build(image: Image, pristine: bool = False) -> Path:
    cmd = [west(), "build", "-b", image.board, "-d", str(image.build_dir), "--sysbuild"]
    if pristine:
        cmd += ["-p", "always"]
    cmd += [str(image.app), "--"]
    if image.confs:
        cmd.append("-DEXTRA_CONF_FILE=" + ";".join(str(c) for c in image.confs))
    cmd += [f"-D{d}" for d in image.defines]
    result = subprocess.run(cmd, cwd=REPO, env=_env(), capture_output=True, text=True)
    if result.returncode != 0:
        raise FirmwareError(f"build {image.name} failed:\n{' '.join(cmd)}\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return hex_file(image)


def hex_file(image: Image) -> Path:
    for candidate in (image.build_dir / "merged.hex", image.build_dir / image.app.name / "zephyr" / "zephyr.hex"):
        if candidate.exists():
            return candidate
    raise FirmwareError(f"no hex file in {image.build_dir}")


def flash(image: Image, jlink_serial: int, erase: bool = False) -> None:
    cmd = [west(), "flash", "-d", str(image.build_dir), "--dev-id", str(jlink_serial)]
    if erase:
        cmd.append("--erase")
    result = subprocess.run(cmd, cwd=REPO, env=_env(), capture_output=True, text=True)
    if result.returncode != 0:
        raise FirmwareError(f"flash {image.name} failed:\n{result.stdout[-3000:]}\n{result.stderr[-3000:]}")


def dot_config(build_dir: Path, app_name: str | None = None) -> dict[str, str]:
    """Parse the application's .config of a (sysbuild) build directory."""
    candidates = []
    if app_name:
        candidates.append(build_dir / app_name / "zephyr" / ".config")
    candidates += [build_dir / "zephyr" / ".config"]
    candidates += sorted(build_dir.glob("*/zephyr/.config"))
    for path in candidates:
        if path.exists() and "_sysbuild" not in str(path) and "ipc_radio" not in str(path):
            values = {}
            for line in path.read_text(encoding="utf-8").splitlines():
                m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
                if m:
                    values[m.group(1)] = m.group(2)
                m = re.match(r"^# (CONFIG_\w+) is not set$", line)
                if m:
                    values[m.group(1)] = "n"
            return values
    raise FirmwareError(f"no application .config below {build_dir}")


# ---- images used by the suite ----------------------------------------------
DEV_CONF = BEACON / "dsa_dev.conf"


def beacon_release(board: str = DK) -> Image:
    return Image("beacon_release" + ("_tag" if board == TAG else ""), BEACON, board)


def beacon_dev() -> Image:
    return Image("beacon_dev", BEACON, DK, (DEV_CONF,))


def beacon_hil(extra: tuple[Path, ...] = (), defines: tuple[str, ...] = (), suffix: str = "") -> Image:
    return Image(f"beacon_hil{suffix}", BEACON, DK, (DEV_CONF, CONFIGS / "hil.conf", *extra), defines)


def beacon_partner(location_tag: bool = False) -> Image:
    confs = (CONFIGS / "beacon_as_location_tag.conf",) if location_tag else ()
    return Image("beacon_partner" + ("_dsl" if location_tag else ""), BEACON, DK, confs)


def base_hil(target_id: int) -> Image:
    return Image(f"base_hil_{target_id}", BASE, DK, (CONFIGS / "base_hil.conf",),
                 (f"CONFIG_DSA_TARGET_BEACON_ID={target_id}",))


def base_release() -> Image:
    return Image("base_release", BASE, DK)


def control_release() -> Image:
    return Image("control_release", CONTROL, DK)


def control_commander() -> Image:
    return Image("control_commander", CONTROL, DK, (CONFIGS / "control_uart_commands.conf",))


def scan_new_tags() -> Image:
    return Image("scan_new_tags", SCAN_TAGS, DK)


def nordic_lbs() -> Image:
    return Image("peripheral_lbs", zephyr_base().parent / "nrf" / "samples" / "bluetooth" / "peripheral_lbs", DK)


def f1_target() -> Image:
    """F1 ztest app on the beacon's own partition layout (pm_static.yml is
    passed to sysbuild, which ignores it when set inside the app)."""
    return Image("f1_flash_ring_store_target", FIRMWARE_TARGET / "flash_ring_store_target", DK,
                 defines=(f"PM_STATIC_YML_FILE={(BEACON / 'pm_static.yml').as_posix()}",))
