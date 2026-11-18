# Class E – Build checks

E1/E2 run west builds and are opt-in (`pytest --run-builds 2_Implementation/tests/build`).
The nRF Connect SDK toolchain is found automatically (`C:/ncs`, `~/ncs`,
`/opt/nordic/ncs` or `$DSA_NCS_TOOLCHAIN`). Build directories:
`Testsuite/.build/west/<image>`. E3's static part always runs.

**Inputs of a build case:** application directory, board target, extra
configuration files (`EXTRA_CONF_FILE`) and extra Kconfig/CMake
definitions, passed to `west build -b <board> -d <dir> --sysbuild <app> -- …`.

---

### E1 – Beacon builds in every configuration

| | |
|---|---|
| Requirements | nRF Connect SDK v3.3.0 toolchain, west |
| Automated | ✅ per pull request (`--run-builds`) |
| Implementation | `2_Implementation/tests/build/test_e1_e2_build_matrix.py::test_e1_beacon_builds` |

| Case | Input | Expected result |
|---|---|---|
| `beacon_release` | `Network_Beacon_nrf54`, `nrf54l15dk/nrf54l15/cpuapp/ns`, no extra conf | Build succeeds, `merged.hex` produced. |
| `beacon_release_tag` | Same app, `nrf54l15tag/nrf54l15/cpuapp/ns` | Build succeeds. |
| `beacon_dev` | DK, `dsa_dev.conf` | Build succeeds. |
| `beacon_hil` | DK, `dsa_dev.conf` + `2_Implementation/configs/hil.conf` | Build succeeds. |
| `beacon_hil_flash` | DK, `dsa_dev.conf` + `hil.conf` + `hil_flash.conf` | Build succeeds. |

---

### E2 – Base, Control and Scan_New_Tags build

| | |
|---|---|
| Requirements | as E1 |
| Automated | ✅ per pull request (`--run-builds`) |
| Implementation | `2_Implementation/tests/build/test_e1_e2_build_matrix.py` |

| Case | Input | Expected result |
|---|---|---|
| `base_release` | `Network_Base_nrf54`, DK | Build succeeds. |
| `base_hil_254` | Base, DK, `2_Implementation/configs/base_hil.conf`, `-DCONFIG_DSA_TARGET_BEACON_ID=254` | Build succeeds. |
| `control_release` | `Network_Control_nrf54`, DK | Build succeeds. |
| `scan_new_tags` | `Scan_New_Tags`, DK | Build succeeds. |
| `test_e2_control_commander_builds` | Control, DK, `2_Implementation/configs/control_uart_commands.conf` | Build succeeds. Skipped with `PREREQUISITE MISSING`. |

---

### E3 – Release configuration guard

| | |
|---|---|
| What is tested | A production build cannot contain development/test options and identifies its version. |
| Requirements | Python, pytest; optional `--release-build-dir <dir>` of a real release build |
| Automated | ✅ every commit (static part); with a build directory before release |
| Implementation | `2_Implementation/tests/build/test_e3_release_guard.py` |

| Case | Input | Expected result |
|---|---|---|
| `test_e3_release_conf_files_disable_dev_options[<symbol>]` | `CONFIG_*=` lines of `prj.conf`, `dsa.conf`, `dsa_runtime.conf`; symbols: synthetic contacts/self-reports, dev validation, eco LED indicator, PRINTK, LOG, CONSOLE, UART_CONSOLE, RTT_CONSOLE | None is `y`. |
| `test_e3_release_build_does_not_pull_dev_conf` | `EXTRA_CONF_FILE` list in `Network_Beacon_nrf54/CMakeLists.txt` | Only `dsa.conf`, `dsa_runtime.conf`. |
| `test_e3_no_forced_device_id_in_release_conf` | Release conf files | No `CONFIG_DSA_DEV_FORCE_ID`. |
| `test_e3_release_build_config` | `<dir>/Network_Beacon_nrf54/zephyr/.config` of the release build | The symbols above not `y`; `CONFIG_BT_DEVICE_NAME="DSA"`. Skipped without `--release-build-dir`. **Fails today on `.build/west/beacon_release`: `CONFIG_PRINTK=y` (selected by `CONFIG_NCS_BOOT_BANNER`) – finding 9.** |
| `test_e3_firmware_version_is_embedded` | `Network_Beacon_nrf54/VERSION` | Defines `VERSION_MAJOR/MINOR`, `PATCHLEVEL`. Skipped with `PREREQUISITE MISSING`. |
