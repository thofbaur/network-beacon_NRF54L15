/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>

#include "radio.h"
#include "network.h"
#include "led.h"
#include "self_report.h"
#include "battery_voltage.h"
#include "development_validation.h"
#include "motion.h"
#include "eco_log.h"

int main(void)
{
    int err;


	printk("Starting DSA Network Beacon\n");
	led_init();
	err = self_report_init();
	if (err) {
		printk("Self-report subsystem initialization failed (err %d)\n",
		       err);
		return err;
	}

	err = eco_log_init();
	if (err) {
		printk("Eco log initialization failed (err %d)\n", err);
	}

	err = network_init();
	if (err) {
		printk("Network subsystem initialization failed (err %d)\n", err);
		return err;
	}
	err = battery_voltage_init();
	if (err) {
		printk("Battery voltage initialization failed (err %d)\n", err);
	}

	/* Initialize the Bluetooth Subsystem */
	err = radio_init();
	if (err) {
		printk("Radio initialization failed (err %d)\n", err);
		return err;
	}

	err = motion_init();
	if (err) {
		printk("Motion subsystem initialization failed (err %d)\n", err);
	}

	/* Start advertising and scanning*/
	err = radio_start();
	if (err) {
		printk("Radio start failed (err %d)\n", err);
		return err;
	}

#if defined(CONFIG_DSA_DEV_SYNTHETIC_CONTACTS)
	development_validation_fill_random_contacts(
		CONFIG_DSA_DEV_SYNTHETIC_CONTACT_COUNT);
#endif

#if defined(CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORTS)
	development_validation_fill_self_reports(
		CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORT_COUNT);
#endif

	k_sleep(K_FOREVER);
	return 0;
}
