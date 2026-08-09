# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

list(APPEND EXTRA_CONF_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/dsa.conf"
    "${CMAKE_CURRENT_SOURCE_DIR}/dsa_runtime.conf"
)

# The onboard ADXL367 (motion.c) only exists on the nrf54l15tag board. Its
# interrupt GPIO isn't wired by the board's own devicetree, so add it here
# explicitly rather than relying on boards/<board>.overlay auto-discovery,
# which Zephyr caches at first configure and won't re-check afterward.
if(BOARD STREQUAL "nrf54l15tag/nrf54l15/cpuapp/ns" OR
   BOARD STREQUAL "nrf54l15tag/nrf54l15/cpuapp")
    list(APPEND EXTRA_DTC_OVERLAY_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/boards/nrf54l15tag_nrf54l15_cpuapp_ns.overlay"
    )
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(scan_adv)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

target_sources(app PRIVATE
	src/battery_voltage.c
	src/device.c
	src/eco_log.c
	src/eco_log_storage.c
	src/flash_ring_store.c
	src/led.c
	src/main.c
	src/motion.c
	src/network.c
	src/network_storage.c
	src/nus.c
	src/param_storage.c
	src/radio.c
	src/radio_ids.c
	src/ram_log_ring.c
	src/self_report.c
	src/self_report_storage.c
	src/storage_work_queue.c
)

target_sources_ifdef(CONFIG_DSA_DEV_VALIDATION app PRIVATE
	src/development_validation.c
)

zephyr_library_include_directories(${ZEPHYR_BASE}/samples/bluetooth)
