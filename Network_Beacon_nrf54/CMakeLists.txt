# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(scan_adv)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

target_sources(app PRIVATE
	src/battery_voltage.c
	src/device.c
	src/led.c
	src/main.c
	src/network.c
	src/network_storage.c
	src/nus.c
	src/param_storage.c
	src/radio.c
	src/radio_ids.c
	src/self_report.c
	src/self_report_storage.c
	src/storage_work_queue.c
)

target_sources_ifdef(CONFIG_DSA_DEV_SYNTHETIC_CONTACTS app PRIVATE
	src/network_dev.c
)

zephyr_library_include_directories(${ZEPHYR_BASE}/samples/bluetooth)
