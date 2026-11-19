"""Build and flash the firmware images the build checks and hardware tests
need, with `west`.

Everything is pinned to NCS_VERSION (the SDK the firmware is developed
with). The SDK is <root>/<NCS_VERSION>/zephyr and the toolchain is the
bundle registered for that version in <root>/toolchains/toolchains.json;
<root> is $DSA_NCS_ROOT or one of C:/ncs, ~/ncs, /opt/nordic/ncs. The
toolchain environment is applied from its own environment.json, so no
special shell is needed.

Overrides: DSA_NCS_VERSION, DSA_NCS_ROOT, DSA_NCS_TOOLCHAIN, ZEPHYR_BASE
(must point to an SDK of NCS_VERSION).
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

# The nRF Connect SDK version the firmware is developed and released with.
# Change it here (or set DSA_NCS_VERSION) when the firmware moves to a new SDK.
NCS_VERSION = os.environ.get("DSA_NCS_VERSION", "v3.3.0")


def _ncs_roots() -> list[Path]:
    explicit = os.environ.get("DSA_NCS_ROOT")
    return [Path(explicit)] if explicit else NCS_ROOTS


def sdk_version(zephyr: Path) -> str | None:
    """'v3.3.0' read from <sdk>/nrf/VERSION next to a zephyr folder."""
    version_file = zephyr.parent / "nrf" / "VERSION"
    if not version_file.exists():
        return None
    return "v" + version_file.read_text(encoding="utf-8").strip()


def toolchain_dir() -> Path | None:
    """The toolchain bundle that the NCS installer registered for NCS_VERSION
    (toolchains/toolchains.json)."""
    explicit = os.environ.get("DSA_NCS_TOOLCHAIN")
    if explicit:
        return Path(explicit)
    for root in _ncs_roots():
        index = root / "toolchains" / "toolchains.json"
        if not index.exists():
            continue
        for entry in json.loads(index.read_text(encoding="utf-8")):
            for tc in entry.get("toolchains", []):
                if NCS_VERSION in tc.get("ncs_versions", []):
                    candidate = root / "toolchains" / tc["identifier"]["bundle_id"]
                    if (candidate / "environment.json").exists():
                        return candidate
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
    env["ZEPHYR_BASE"] = str(zephyr_base())
    return env


def west() -> str:
    if toolchain_dir() is None:
        raise FirmwareError(
            f"no nRF Connect SDK toolchain for {NCS_VERSION} found in "
            f"{', '.join(p.as_posix() for p in _ncs_roots())}: install it with the nRF Connect "
            "toolchain manager, or set DSA_NCS_TOOLCHAIN")
    path = shutil.which("west", path=_env().get("PATH"))
    if not path:
        raise FirmwareError(f"'west' not found in the toolchain {toolchain_dir()}")
    return path


def zephyr_base() -> Path:
    """The Zephyr folder of NCS_VERSION. An explicit ZEPHYR_BASE (often set
    globally by the nRF Connect extension) is used only if it is that version."""
    env = os.environ.get("ZEPHYR_BASE")
    if env:
        found = sdk_version(Path(env))
        if found != NCS_VERSION:
            raise FirmwareError(
                f"ZEPHYR_BASE={env} is nRF Connect SDK {found or 'of unknown version'}, the firmware "
                f"uses {NCS_VERSION}: unset ZEPHYR_BASE or set DSA_NCS_VERSION")
        return Path(env)
    for root in _ncs_roots():
        candidate = root / NCS_VERSION / "zephyr"
        if candidate.is_dir():
            return candidate
    raise FirmwareError(
        f"nRF Connect SDK {NCS_VERSION} not found in {', '.join(p.as_posix() for p in _ncs_roots())}: "
        "install it, or set DSA_NCS_ROOT / ZEPHYR_BASE")


def build(image: Image, pristine: bool = False) -> Path:
    cmd = [west(), "build", "-b", image.board, "-d", str(image.build_dir), "--sysbuild"]
    if pristine:
        cmd += ["-p", "always"]
    cmd += [str(image.app), "--"]
    if image.confs:
        cmd.append("-DEXTRA_CONF_FILE=" + ";".join(str(c) for c in image.confs))
    cmd += [f"-D{d}" for d in image.defines]
    # Run from the app folder: west computes a relative source path, which
    # fails on Windows when the repository and the SDK are on different drives.
    result = subprocess.run(cmd, cwd=image.app, env=_env(), capture_output=True, text=True)
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
