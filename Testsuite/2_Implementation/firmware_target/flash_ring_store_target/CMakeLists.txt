# F1 - flash_ring_store.c against the real NVS on the nRF54L15 (DK A).
# Uses the beacon's own partition layout so the test runs on exactly the
# partitions the product uses.
cmake_minimum_required(VERSION 3.20.0)

set(BEACON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../Network_Beacon_nrf54)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flash_ring_store_target)

target_include_directories(app PRIVATE ${BEACON_DIR}/src ${BEACON_DIR}/../shared)
target_sources(app PRIVATE
	src/main.c
	${BEACON_DIR}/src/flash_ring_store.c
)
