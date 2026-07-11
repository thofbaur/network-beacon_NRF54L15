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

int main(void)
{
    int err;


	printk("Starting DSA Network Beacon\n");
	led_init();
	self_report_init();
	network_init();

	/* Initialize the Bluetooth Subsystem */
	err = radio_init();
	if (err) {
		printk("Radio initialization failed (err %d)\n", err);
		return err;
	}

	/* Start advertising and scanning*/
	err = radio_start();
	if (err) {
		printk("Radio start failed (err %d)\n", err);
		return err;
	}

	/* TODO Remove: seed synthetic contacts for development testing. */
	network_dev_fill_random_contacts(100);

	k_sleep(K_FOREVER);
	return 0;
}
