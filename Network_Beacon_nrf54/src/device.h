#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/util.h>

/* Bit layout of the advertised radio/storage status byte (ADV_POS_RADIO_STATUS
 * in common_include.h). Bits 0/1/5 (RADIO_STATUS_*, RADIO_STATUS_MOTION_UNAVAILABLE)
 * live in common_include.h since they're set directly, not derived from the
 * storage_faults/storage_full aggregates below. Bits 6-7 are reserved.
 */
#define STORAGE_STATUS_STORAGE_FULL	BIT(2)
#define STORAGE_STATUS_PARAM_ERROR	BIT(3)
#define STORAGE_STATUS_STORAGE_ERROR	BIT(4)

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
#define STORAGE_FAULT_ECO_LOG_INIT	BIT(13)
#define STORAGE_FAULT_ECO_LOG_READ	BIT(14)
#define STORAGE_FAULT_ECO_LOG_WRITE	BIT(15)
#define STORAGE_FAULT_ECO_LOG_DELETE	BIT(16)
#define STORAGE_FAULT_ECO_LOG_META	BIT(17)
#define STORAGE_FAULT_MOTION_PARAMS	BIT(18)

#define STORAGE_FAULT_PARAM_MASK \
	(STORAGE_FAULT_LED_PARAMS | STORAGE_FAULT_NETWORK_PARAMS | \
	 STORAGE_FAULT_RADIO_PARAMS | STORAGE_FAULT_MOTION_PARAMS)
/* Any I/O or metadata fault across the three flash-backed domains: distinct
 * record/metadata faults aren't worth separate advertised bits, since both
 * mean the same thing to someone who can only reach the tag over BLE - the
 * storage subsystem is unhealthy.
 */
#define STORAGE_FAULT_STORAGE_MASK \
	(STORAGE_FAULT_CONTACT_INIT | STORAGE_FAULT_CONTACT_READ | \
	 STORAGE_FAULT_CONTACT_WRITE | STORAGE_FAULT_CONTACT_DELETE | \
	 STORAGE_FAULT_CONTACT_META | \
	 STORAGE_FAULT_SELF_REPORT_INIT | STORAGE_FAULT_SELF_REPORT_READ | \
	 STORAGE_FAULT_SELF_REPORT_WRITE | STORAGE_FAULT_SELF_REPORT_DELETE | \
	 STORAGE_FAULT_SELF_REPORT_META | \
	 STORAGE_FAULT_ECO_LOG_INIT | STORAGE_FAULT_ECO_LOG_READ | \
	 STORAGE_FAULT_ECO_LOG_WRITE | STORAGE_FAULT_ECO_LOG_DELETE | \
	 STORAGE_FAULT_ECO_LOG_META)

/* Domain bits for device_set_storage_full(), distinct from the fault bits
 * above: "full" isn't an I/O error, it means the ring is healthy but out of
 * room and has started discarding its oldest un-exported entries.
 */
#define STORAGE_FULL_CONTACT		BIT(0)
#define STORAGE_FULL_SELF_REPORT	BIT(1)
#define STORAGE_FULL_ECO_LOG		BIT(2)

uint8_t get_device_id(void);
uint8_t lookup_device_id(const bt_addr_le_t *addr);

uint8_t device_get_radio_status(void);
void device_set_radio_status(uint8_t status);
void device_set_radio_status_bit(uint8_t mask, bool active);
void device_set_storage_fault(uint32_t fault, bool active);
uint32_t device_get_storage_faults(void);
void device_set_storage_full(uint32_t domain, bool active);
uint32_t device_get_storage_full(void);

uint8_t device_get_network_status(void);
void device_set_network_status(uint8_t status);
void device_set_network_status_bits(uint8_t mask, uint8_t status);
