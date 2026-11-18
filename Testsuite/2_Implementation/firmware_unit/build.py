#!/usr/bin/env python3
"""Build (and optionally run) the host-compiled firmware unit tests.

The unmodified firmware sources are compiled against the Zephyr host shim in
shim/ and the fakes in fakes/. Kconfig values are generated from the real
project configuration so the tests exercise the production configuration:

    Network_Beacon_nrf54/Kconfig            (defaults)
    Network_Beacon_nrf54/dsa.conf           (project values)
    Network_Beacon_nrf54/dsa_runtime.conf   (runtime defaults)
    Network_Beacon_nrf54/pm_static.yml      (partition offsets/sizes)

Compiler selection (first match wins): $CC, gcc, clang, cc, then
"python -m ziglang cc" (pip install ziglang) - so the tests build on
Windows, macOS and Linux without a system compiler.

Usage:
    python build.py                 build all targets
    python build.py --run           build and run all targets
    python build.py --target c1_flash_ring_store --run
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]  # Testsuite/2_Implementation/firmware_unit -> repo
BEACON = REPO / "Network_Beacon_nrf54"
BEACON_SRC = BEACON / "src"
BASE_SRC = REPO / "Network_Base_nrf54" / "src"
SHARED = REPO / "shared"
BUILD_DIR = REPO / "Testsuite" / ".build" / "firmware_unit"  # regenerable, git-ignored
EXE_SUFFIX = ".exe" if os.name == "nt" else ""

# Hooks the tests use to inject data; they only add code paths that exist in
# dsa_dev.conf builds, they do not change production code paths.
TEST_ONLY_CONFIG = {
    "CONFIG_DSA_DEV_SYNTHETIC_CONTACTS": "1",
    "CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORTS": "1",
    "CONFIG_BT_ID_MAX": "1",
    "CONFIG_BT_L2CAP_TX_MTU": "247",
}

COMMON_SOURCES = [
    HERE / "minitest.c",
    HERE / "shim/src/shim_kernel.c",
    HERE / "shim/src/shim_drivers.c",
    HERE / "shim/src/shim_bt.c",
    HERE / "fakes/fake_nvs.c",
    HERE / "fakes/fake_settings.c",
    HERE / "fakes/fake_peers.c",
]
BEACON_INCLUDES = [HERE / "shim/include", HERE / "fakes", BEACON_SRC, SHARED]
BASE_INCLUDES = [HERE / "shim/include", HERE / "fakes", BASE_SRC, SHARED]


def beacon(*names: str) -> list[Path]:
    return [BEACON_SRC / n for n in names]


def fake(*names: str) -> list[Path]:
    return [HERE / "fakes" / n for n in names]


@dataclass
class Target:
    name: str
    test: Path
    sources: list[Path]
    includes: list[Path] = field(default_factory=lambda: list(BEACON_INCLUDES))
    defines: list[str] = field(default_factory=list)

    @property
    def exe(self) -> Path:
        return BUILD_DIR / f"{self.name}{EXE_SUFFIX}"


TARGETS = [
    Target("c1_flash_ring_store", HERE / "tests/test_c1_flash_ring_store.c",
           beacon("flash_ring_store.c", "network_storage.c", "device.c", "radio_ids.c")
           + fake("fake_radio_status.c")),
    Target("c2_ram_log_ring", HERE / "tests/test_c2_ram_log_ring.c",
           beacon("ram_log_ring.c", "storage_work_queue.c", "device.c", "radio_ids.c")
           + fake("fake_radio_status.c")),
    Target("c3_network", HERE / "tests/test_c3_network.c",
           beacon("network.c", "network_storage.c", "flash_ring_store.c", "param_storage.c",
                  "device.c", "radio_ids.c", "storage_work_queue.c")
           + fake("fake_radio_status.c")),
    Target("c4_self_report_eco_log", HERE / "tests/test_c4_self_report_eco_log.c",
           beacon("self_report.c", "self_report_storage.c", "eco_log.c", "eco_log_storage.c",
                  "ram_log_ring.c", "flash_ring_store.c", "storage_work_queue.c",
                  "device.c", "radio_ids.c")
           + fake("fake_radio_status.c", "fake_led_api.c")),
    Target("c5_param_storage", HERE / "tests/test_c5_param_storage.c",
           beacon("param_storage.c")),
    Target("c6_device", HERE / "tests/test_c6_device.c",
           beacon("device.c", "radio_ids.c") + fake("fake_radio_status.c")),
    Target("c7_base_message_parser", HERE / "tests/test_c7_base_message_parser.c",
           [BASE_SRC / "message_parser.c"] + fake("fake_output.c"),
           includes=list(BASE_INCLUDES)),
    Target("c8_motion", HERE / "tests/test_c8_motion.c",
           beacon("motion.c", "device.c", "radio_ids.c", "param_storage.c")
           + fake("fake_radio_status.c", "fake_led_api.c")),
    Target("c9_radio", HERE / "tests/test_c9_radio.c",
           beacon("radio.c", "device.c", "radio_ids.c", "param_storage.c",
                  "storage_work_queue.c")
           + fake("fake_led_api.c", "fake_network_api.c", "fake_motion_api.c",
                  "fake_eco_log_api.c", "fake_nus_api.c")),
    Target("c10_led", HERE / "tests/test_c10_led.c",
           beacon("led.c", "device.c", "radio_ids.c", "param_storage.c")
           + fake("fake_radio_status.c")),
    Target("c11_battery", HERE / "tests/test_c11_battery.c",
           beacon("battery_voltage.c", "device.c", "radio_ids.c")
           + fake("fake_radio_status.c")),
    # Not a minitest suite: reads frames on stdin, prints the Base's decoded
    # output lines. Used by the cross-language contract test D1.
    Target("d1_base_parser_dump", HERE / "tools/d1_base_parser_dump.c",
           [BASE_SRC / "message_parser.c"] + fake("fake_output.c"),
           includes=list(BASE_INCLUDES)),
]


class BuildError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# configuration generation
# --------------------------------------------------------------------------
def parse_kconfig_defaults(path: Path) -> dict[str, tuple[str, str]]:
    """Return {CONFIG_NAME: (type, default)} for unconditional defaults."""
    symbols: dict[str, tuple[str, str]] = {}
    current = None
    ctype = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        m = re.match(r"^config\s+(\w+)", line)
        if m:
            current = f"CONFIG_{m.group(1)}"
            ctype = None
            continue
        if current is None:
            continue
        m = re.match(r"^(bool|int|hex|string)\b", line)
        if m:
            ctype = m.group(1)
            continue
        m = re.match(r"^default\s+(.+?)\s*$", line)
        if m and " if " not in f" {m.group(1)} " and current not in symbols and ctype:
            symbols[current] = (ctype, m.group(1))
    return symbols


def parse_conf(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(CONFIG_\w+)=(.*)$", line)
        if m:
            values[m.group(1)] = m.group(2).strip()
    return values


def parse_pm_static(path: Path) -> dict[str, tuple[int, int]]:
    """Minimal pm_static.yml reader: {partition: (address, size)}."""
    parts: dict[str, dict[str, int]] = {}
    current = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        m = re.match(r"^(\w+):\s*$", raw)
        if m:
            current = m.group(1)
            parts[current] = {}
            continue
        m = re.match(r"^\s+(address|size):\s*(0x[0-9a-fA-F]+|\d+)\s*$", raw)
        if m and current:
            parts[current][m.group(1)] = int(m.group(2), 0)
    return {k: (v["address"], v["size"]) for k, v in parts.items()
            if "address" in v and "size" in v}


def generate_config_header() -> Path:
    symbols = parse_kconfig_defaults(BEACON / "Kconfig")
    values = {name: default for name, (_t, default) in symbols.items()}
    types = {name: t for name, (t, _d) in symbols.items()}
    for conf in ("dsa.conf", "dsa_runtime.conf"):
        values.update(parse_conf(BEACON / conf))

    lines = [
        "/* Generated by build.py - do not edit. Production Kconfig values of",
        " * Network_Beacon_nrf54 (Kconfig defaults + dsa.conf + dsa_runtime.conf)",
        " * and partition layout from pm_static.yml.",
        " */",
        "#ifndef SHIM_TEST_CONFIG_H",
        "#define SHIM_TEST_CONFIG_H",
    ]
    for name in sorted(values):
        value = values[name]
        ctype = types.get(name)
        if value == "y":
            emitted = "1"
        elif value == "n":
            continue
        elif ctype == "string" or value.startswith('"'):
            emitted = value if value.startswith('"') else f'"{value}"'
        else:
            emitted = value
        lines += [f"#ifndef {name}", f"#define {name} {emitted}", "#endif"]
    for name, value in TEST_ONLY_CONFIG.items():
        lines += [f"#ifndef {name}", f"#define {name} {value}", "#endif"]
    for part, (address, size) in sorted(parse_pm_static(BEACON / "pm_static.yml").items()):
        lines.append(f"#define SHIM_PARTITION_OFFSET_{part} 0x{address:x}")
        lines.append(f"#define SHIM_PARTITION_SIZE_{part} 0x{size:x}")
    lines += ["#endif", ""]

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    header = BUILD_DIR / "test_config.h"
    text = "\n".join(lines)
    if not header.exists() or header.read_text(encoding="utf-8") != text:
        header.write_text(text, encoding="utf-8")
    return header


# --------------------------------------------------------------------------
# compiler
# --------------------------------------------------------------------------
def find_compiler() -> list[str]:
    env_cc = os.environ.get("CC")
    if env_cc:
        return env_cc.split()
    for name in ("gcc", "clang", "cc"):
        path = shutil.which(name)
        if path:
            return [path]
    try:
        import ziglang  # noqa: F401
    except ImportError:
        pass
    else:
        return [sys.executable, "-m", "ziglang", "cc"]
    raise BuildError(
        "No C compiler found. Install gcc/clang or run "
        "'pip install ziglang' (portable compiler, no admin rights needed)."
    )


@dataclass(frozen=True)
class CoverageToolchain:
    """A compiler that can build instrumented executables plus its tools.

    kind "gcc":  gcc --coverage, reported with gcov through gcovr.
    kind "llvm": clang -fprofile-instr-generate, reported with llvm-cov.
    """

    kind: str
    cc: str
    flags: tuple[str, ...]
    gcov: str = ""
    profdata: str = ""
    cov: str = ""


def _llvm_bin_candidates() -> list[Path]:
    dirs: list[Path] = []
    env = os.environ.get("DSA_LLVM_BIN")
    if env:
        dirs.append(Path(env))
    for name in ("clang", "llvm-cov"):
        found = shutil.which(name)
        if found:
            dirs.append(Path(found).parent)
    local = Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WinGet" / "Packages"
    dirs += sorted(local.glob("BrechtSanders.WinLibs*LLVM*/mingw64/bin"), reverse=True)
    dirs += [
        Path("C:/Program Files/LLVM/bin"),
        Path("/opt/homebrew/opt/llvm/bin"),
        Path("/usr/local/opt/llvm/bin"),
    ]
    dirs += sorted(Path("/usr/lib").glob("llvm-*/bin"), reverse=True)
    return dirs


LLVM_FLAGS = ("-fprofile-instr-generate", "-fcoverage-mapping")
GCC_FLAGS = ("--coverage",)
COVERAGE_BUILD_DIR = BUILD_DIR.parent / "firmware_unit_coverage"
_toolchain_cache: CoverageToolchain | None = None


def _links_with(cc: str, flags: tuple[str, ...]) -> bool:
    """Probe: can this compiler link an instrumented program? (zig and
    several clang distributions lack the coverage runtime library)."""
    probe_dir = BUILD_DIR.parent / "coverage_probe"
    probe_dir.mkdir(parents=True, exist_ok=True)
    source = probe_dir / "probe.c"
    source.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    result = subprocess.run([cc, *flags, str(source), "-o", str(probe_dir / f"probe{EXE_SUFFIX}")],
                            capture_output=True, text=True, cwd=probe_dir)
    return result.returncode == 0


def find_coverage_toolchain() -> CoverageToolchain:
    """Find a compiler that can build coverage-instrumented executables.

    Tried in order, each only if a probe program links:
      1. clang + llvm-profdata + llvm-cov (macOS Xcode/Homebrew, Linux, LLVM)
      2. gcc + gcov (Linux, WinLibs/MinGW on Windows), reported via gcovr
    Windows without Visual Studio: winget install BrechtSanders.WinLibs.POSIX.UCRT.LLVM
    (its gcc/gcov is used). DSA_LLVM_BIN can point to the tool directory.
    The bundled zig compiler has no coverage runtime and is never used here.
    """
    global _toolchain_cache
    if _toolchain_cache is not None:
        return _toolchain_cache
    exe = EXE_SUFFIX
    candidates = _llvm_bin_candidates()
    for directory in candidates:
        clang, profdata, cov = (directory / f"{n}{exe}" for n in ("clang", "llvm-profdata", "llvm-cov"))
        if clang.exists() and profdata.exists() and cov.exists() and _links_with(str(clang), LLVM_FLAGS):
            _toolchain_cache = CoverageToolchain("llvm", str(clang), LLVM_FLAGS,
                                                 profdata=str(profdata), cov=str(cov))
            return _toolchain_cache
    if sys.platform == "darwin":
        try:
            profdata = subprocess.run(["xcrun", "--find", "llvm-profdata"], capture_output=True, text=True, check=True)
            cov = subprocess.run(["xcrun", "--find", "llvm-cov"], capture_output=True, text=True, check=True)
            if _links_with("clang", LLVM_FLAGS):
                _toolchain_cache = CoverageToolchain("llvm", "clang", LLVM_FLAGS,
                                                     profdata=profdata.stdout.strip(), cov=cov.stdout.strip())
                return _toolchain_cache
        except (OSError, subprocess.CalledProcessError):
            pass
    gcc_dirs = candidates + [Path(p).parent for p in filter(None, [shutil.which("gcc")])]
    for directory in gcc_dirs:
        gcc, gcov = directory / f"gcc{exe}", directory / f"gcov{exe}"
        if gcc.exists() and gcov.exists() and _links_with(str(gcc), GCC_FLAGS):
            _toolchain_cache = CoverageToolchain("gcc", str(gcc), GCC_FLAGS, gcov=str(gcov))
            return _toolchain_cache
    raise BuildError(
        "Coverage needs clang with the profile runtime + llvm-cov, or gcc + gcov. "
        "Windows: winget install BrechtSanders.WinLibs.POSIX.UCRT.LLVM ; macOS: Xcode "
        "command line tools ; Linux: gcc or clang + llvm. DSA_LLVM_BIN can point to the tools."
    )


def newest_input_mtime(target: Target, header: Path) -> float:
    files = [target.test, header, *target.sources, *COMMON_SOURCES]
    for directory in (HERE / "shim", HERE / "fakes", HERE):
        files += list(directory.rglob("*.h"))
    for inc in target.includes:
        if inc.is_dir() and inc not in (HERE / "shim/include", HERE / "fakes"):
            files += list(inc.glob("*.h"))
    return max(f.stat().st_mtime for f in files if f.exists())


def build_target(target: Target, compiler: list[str], header: Path, force: bool = False,
                 out_dir: Path = BUILD_DIR, extra_flags: tuple[str, ...] = ()) -> Path:
    exe = out_dir / target.exe.name
    if not force and exe.exists() and exe.stat().st_mtime >= newest_input_mtime(target, header):
        return exe
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        *compiler,
        *extra_flags,
        "-std=gnu11",
        "-O0",
        "-g",
        "-Wall",
        "-Wno-unused-function",
        "-Wno-unused-variable",
        "-Wno-unused-but-set-variable",
        "-include", str(header),
    ]
    for inc in target.includes:
        cmd += ["-I", str(inc)]
    cmd += ["-I", str(HERE)]
    cmd += [f"-D{d}" for d in target.defines]
    cmd += [str(target.test), *map(str, target.sources), *map(str, COMMON_SOURCES)]
    cmd += ["-o", str(exe)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise BuildError(
            f"Build of {target.name} failed:\n{' '.join(cmd)}\n{result.stdout}\n{result.stderr}"
        )
    return exe


def build_all(names: list[str] | None = None, force: bool = False, coverage: bool = False) -> dict[str, Path]:
    """Build the targets. coverage=True builds an instrumented copy with
    clang into COVERAGE_BUILD_DIR (the normal build stays untouched)."""
    header = generate_config_header()
    if coverage:
        toolchain = find_coverage_toolchain()
        compiler = [toolchain.cc]
        out_dir, extra = COVERAGE_BUILD_DIR, toolchain.flags
    else:
        compiler = find_compiler()
        out_dir, extra = BUILD_DIR, ()
    selected = [t for t in TARGETS if names is None or t.name in names]
    if not selected:
        raise BuildError(f"Unknown target(s): {names}")
    results: dict[str, Path] = {}
    errors: list[str] = []
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        futures = {pool.submit(build_target, t, compiler, header, force, out_dir, extra): t for t in selected}
        for future, target in futures.items():
            try:
                results[target.name] = future.result()
            except BuildError as exc:
                errors.append(str(exc))
    if errors:
        raise BuildError("\n\n".join(errors))
    return results


def list_cases(exe: Path) -> list[str]:
    out = subprocess.run([str(exe), "--list"], capture_output=True, text=True, check=True)
    return [line.strip() for line in out.stdout.splitlines() if line.strip()]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", action="append", help="build only this target (repeatable)")
    parser.add_argument("--run", action="store_true", help="run the test executables after building")
    parser.add_argument("--force", action="store_true", help="rebuild even if up to date")
    args = parser.parse_args(argv)

    try:
        built = build_all(args.target, args.force)
    except BuildError as exc:
        print(exc, file=sys.stderr)
        return 2
    print(f"Built {len(built)} target(s) in {BUILD_DIR}")
    if not args.run:
        return 0

    # Every case runs in its own process so module statics start fresh, the
    # same way the pytest runner (2_Implementation/tests/firmware_unit/) executes them.
    failed: list[str] = []
    passed = 0
    for name, exe in sorted(built.items()):
        if name.startswith("d1_"):
            continue
        print(f"=== {name}")
        for case in list_cases(exe):
            result = subprocess.run([str(exe), "--run", case], capture_output=True, text=True)
            if result.returncode == 0:
                passed += 1
                print(f"PASS {case}")
            else:
                failed.append(case)
                print(f"FAIL {case}")
                for line in result.stdout.splitlines():
                    if line.startswith("    "):
                        print(line)
    print(f"\n{passed} passed, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
