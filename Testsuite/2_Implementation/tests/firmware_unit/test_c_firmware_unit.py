"""C1-C11 - firmware unit tests on the PC.

The test logic lives in 2_Implementation/firmware_unit/tests/test_c*.c; this module runs
each C case in its own process and reports it as a pytest item. See
1_Specification/C_firmware_unit.md.
"""

from __future__ import annotations

import subprocess

import pytest


def test_c(c_case, c_env):
    target, case, exe = c_case
    if target == "BUILD":
        pytest.fail(f"firmware unit tests did not build:\n{case}")
    result = subprocess.run([exe, "--run", case], capture_output=True, text=True, timeout=600, env=c_env)
    details = "\n".join(line.strip() for line in result.stdout.splitlines() if line.startswith("    "))
    assert result.returncode == 0, details or result.stdout + result.stderr
