/* TODO Remove: development-only synthetic network contact data filler. */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>

#include "network.h"

/* TODO Remove: fills the contact ring with random development-test entries. */
void network_dev_fill_random_contacts(uint16_t count)
{
	uint32_t uptime_s = (uint32_t)k_uptime_seconds();

	for (uint16_t i = 0; i < count; i++) {
		uint8_t id = sys_rand8_get();
		uint8_t rssi = 30U + (sys_rand8_get() % 70U);
		uint32_t age_s = sys_rand32_get() & 0x00ffffffU;

		network_dev_append_contact(id, uptime_s - age_s, rssi);
	}

	printk("TODO Remove: filled network contact buffer with %u random entries\n", count);
}
