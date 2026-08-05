#
# Copyright (c) 2018 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(Network_Base_nrf54)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../shared)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
  src/led.c
  src/radio.c
  src/nus.c
  src/message_parser.c
  src/output.c
)

if(CONFIG_DSA_OUTPUT STREQUAL "UART")
  target_sources(app PRIVATE src/uart_output.c)
  target_compile_definitions(app PRIVATE DSA_OUTPUT_UART)
elseif(CONFIG_DSA_OUTPUT STREQUAL "RTT")
  target_sources(app PRIVATE src/rtt_output.c)
  target_compile_definitions(app PRIVATE DSA_OUTPUT_RTT)
else()
  message(FATAL_ERROR "Unsupported CONFIG_DSA_OUTPUT value: ${CONFIG_DSA_OUTPUT}. Use UART or RTT.")
endif()

if(CONFIG_DSA_OUTPUT_FORMAT_PARSED)
  target_compile_definitions(app PRIVATE DSA_OUTPUT_FORMAT_PARSED)
elseif(CONFIG_DSA_OUTPUT_FORMAT_RAW)
  target_compile_definitions(app PRIVATE DSA_OUTPUT_FORMAT_RAW)
endif()
# NORDIC SDK APP END
