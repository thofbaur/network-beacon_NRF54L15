/* Development-only synthetic validation data generator. */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/printk.h>

#include "development_validation.h"
#include "network.h"
#include "self_report.h"

#define SYNTHETIC_CONTACT_TIME_MAX 0x00ffffffU

#if defined(CONFIG_DSA_DEV_SYNTHETIC_CONTACTS)
void development_validation_fill_random_contacts(uint16_t count)
{
	for (uint16_t i = 0; i < count; i++) {
		uint8_t id = (uint8_t)((i + 1U) & UINT8_MAX);
		uint32_t uptime_s = (i + 1U) & SYNTHETIC_CONTACT_TIME_MAX;
		uint8_t rssi = 30U + (sys_rand8_get() % 70U);

		network_development_validation_append_contact(
			id, uptime_s, rssi);
	}

	printk("Filled network contact buffer with %u synthetic entries\n",
	       count);
}
#endif

#if defined(CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORTS)
void development_validation_fill_self_reports(uint16_t count)
{
	uint32_t uptime_s = (uint32_t)k_uptime_seconds();

	for (uint16_t i = 0; i < count; i++) {
		uint32_t age_s = sys_rand32_get() & 0x00ffffffU;

		self_report_development_validation_append(uptime_s - age_s);
	}

	printk("Filled self-report buffer with %u synthetic entries\n", count);
}
#endif
