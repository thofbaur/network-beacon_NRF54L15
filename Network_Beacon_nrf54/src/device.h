#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/util.h>

#define STORAGE_STATUS_PARAM_ERROR	BIT(3)
#define STORAGE_STATUS_CONTACT_ERROR	BIT(4)
#define STORAGE_STATUS_META_ERROR	BIT(5)

#define STORAGE_FAULT_LED_PARAMS		BIT(0)
#define STORAGE_FAULT_NETWORK_PARAMS	BIT(1)
#define STORAGE_FAULT_RADIO_PARAMS	BIT(2)
#define STORAGE_FAULT_CONTACT_INIT	BIT(3)
#define STORAGE_FAULT_CONTACT_READ	BIT(4)
#define STORAGE_FAULT_CONTACT_WRITE	BIT(5)
#define STORAGE_FAULT_CONTACT_DELETE	BIT(6)
#define STORAGE_FAULT_CONTACT_META	BIT(7)
#define STORAGE_FAULT_SELF_REPORT_INIT	BIT(8)
#define STORAGE_FAULT_SELF_REPORT_READ	BIT(9)
#define STORAGE_FAULT_SELF_REPORT_WRITE	BIT(10)
#define STORAGE_FAULT_SELF_REPORT_DELETE BIT(11)
#define STORAGE_FAULT_SELF_REPORT_META	BIT(12)

#define STORAGE_FAULT_PARAM_MASK \
	(STORAGE_FAULT_LED_PARAMS | STORAGE_FAULT_NETWORK_PARAMS | \
	 STORAGE_FAULT_RADIO_PARAMS)
#define STORAGE_FAULT_CONTACT_MASK \
	(STORAGE_FAULT_CONTACT_INIT | STORAGE_FAULT_CONTACT_READ | \
	 STORAGE_FAULT_CONTACT_WRITE | STORAGE_FAULT_CONTACT_DELETE | \
	 STORAGE_FAULT_SELF_REPORT_INIT | STORAGE_FAULT_SELF_REPORT_READ | \
	 STORAGE_FAULT_SELF_REPORT_WRITE | STORAGE_FAULT_SELF_REPORT_DELETE)

uint8_t get_device_id(void);
uint8_t lookup_device_id(const bt_addr_le_t *addr);

uint8_t device_get_radio_status(void);
void device_set_radio_status(uint8_t status);
void device_set_radio_status_bit(uint8_t mask, bool active);
void device_set_storage_fault(uint32_t fault, bool active);
uint32_t device_get_storage_faults(void);

uint8_t device_get_network_status(void);
void device_set_network_status(uint8_t status);
void device_set_network_status_bits(uint8_t mask, uint8_t status);
