#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include "radio_ids.h"
#include "device.h"
#include "radio.h"

struct device_status {
    atomic_t radio;
    atomic_t network;
};

static struct device_status status_device;
static atomic_t storage_faults;

uint8_t lookup_device_id(const bt_addr_le_t *addr)
{
    for (size_t i = 0; i < known_device_table_len; i++) {
        if (bt_addr_le_cmp(addr, &known_device_table[i].addr) == 0) {
            return known_device_table[i].id;
        }
    }

    return 0xff; // unknown / unassigned
}

uint8_t get_device_id()
{
    static uint8_t device_id = 0xff;

    bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
    size_t count = CONFIG_BT_ID_MAX;

    // Retrieve all addresses registered for the Bluetooth stack
    bt_id_get(addrs, &count);

	if (count > 0) {
		device_id = lookup_device_id(&addrs[0]);
	} else {
		printk("No Bluetooth identity address available, using unknown device id\n");
	}
	return device_id;
}

uint8_t device_get_radio_status(void)
{
    return (uint8_t)atomic_get(&status_device.radio);
}

void device_set_radio_status(uint8_t status)
{
    atomic_set(&status_device.radio, status);
}

void device_set_radio_status_bit(uint8_t mask, bool active)
{
    if (active) {
        atomic_or(&status_device.radio, mask);
    } else {
        atomic_and(&status_device.radio, (atomic_val_t)~mask);
    }
}

void device_set_storage_fault(uint32_t fault, bool active)
{
	atomic_val_t faults;
	uint8_t previous = device_get_radio_status();

	if (active) {
		atomic_or(&storage_faults, fault);
	} else {
		atomic_and(&storage_faults, (atomic_val_t)~fault);
	}

	faults = atomic_get(&storage_faults);
	device_set_radio_status_bit(STORAGE_STATUS_PARAM_ERROR,
				    (faults & STORAGE_FAULT_PARAM_MASK) != 0);
	device_set_radio_status_bit(STORAGE_STATUS_CONTACT_ERROR,
				    (faults & STORAGE_FAULT_CONTACT_MASK) != 0);
	device_set_radio_status_bit(STORAGE_STATUS_META_ERROR,
				    (faults & (STORAGE_FAULT_CONTACT_META |
					       STORAGE_FAULT_SELF_REPORT_META |
					       STORAGE_FAULT_ECO_LOG_META)) != 0);

	if (device_get_radio_status() != previous) {
		radio_schedule_status_update();
	}
}

uint32_t device_get_storage_faults(void)
{
	return (uint32_t)atomic_get(&storage_faults);
}

uint8_t device_get_network_status(void)
{
    return (uint8_t)atomic_get(&status_device.network);
}

void device_set_network_status(uint8_t status)
{
    atomic_set(&status_device.network, status);
}

void device_set_network_status_bits(uint8_t mask, uint8_t status)
{
	uint8_t previous;
	uint8_t updated;

	previous = device_get_network_status();
	updated = (previous & ~mask) | (status & mask);
	atomic_set(&status_device.network, updated);

	if (updated != previous) {
		radio_schedule_status_update();
	}
}
