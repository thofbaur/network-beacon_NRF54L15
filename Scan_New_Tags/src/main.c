/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief Continuously scans for BLE advertisements and reports the address
 *         of any device whose advertised name matches a configured list.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#include <zephyr/bluetooth/bluetooth.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(scan_new_tags);

/* Names to look for in advertising/scan response data. Edit this list and
 * reflash to change which devices are reported.
 */
static const char *const target_names[] = {
	"Nordic_LBS",
	//"DSA",
	//"DSZ",
	//"DST",
	//"DSL",
};

/* UART used to report matching addresses, at 115200 baud (set in the board
 * devicetree). Kept separate from logging, which goes out over RTT.
 */
#define REPORT_UART_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const report_uart = DEVICE_DT_GET(REPORT_UART_NODE);

static void uart_send_string(const char *str)
{
	for (; *str; str++) {
		uart_poll_out(report_uart, *str);
	}
}

#define MAX_NAME_LEN 32

struct name_match_data {
	bool matched;
	char name[MAX_NAME_LEN + 1];
};

static bool ad_parse_cb(struct bt_data *data, void *user_data)
{
	struct name_match_data *match = user_data;

	if (data->type != BT_DATA_NAME_COMPLETE && data->type != BT_DATA_NAME_SHORTENED) {
		return true;
	}

	for (size_t i = 0; i < ARRAY_SIZE(target_names); i++) {
		size_t name_len = strlen(target_names[i]);

		if (data->data_len == name_len &&
		    memcmp(data->data, target_names[i], name_len) == 0) {
			match->matched = true;
			memcpy(match->name, data->data, name_len);
			match->name[name_len] = '\0';
			return false;
		}
	}

	return true;
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	struct name_match_data match = { .matched = false };
	char addr_str[BT_ADDR_LE_STR_LEN];
	char msg[sizeof("Device,") - 1 + MAX_NAME_LEN + 1 + BT_ADDR_LE_STR_LEN + 2];

	bt_data_parse(buf, ad_parse_cb, &match);

	if (!match.matched) {
		return;
	}

	bt_addr_le_to_str(info->addr, addr_str, sizeof(addr_str));
	LOG_INF("Match: %s (%s)", match.name, addr_str);

	snprintk(msg, sizeof(msg), "Device,%s,%s\r\n", match.name, addr_str);
	uart_send_string(msg);
}

static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv_cb,
};

int main(void)
{
	int err;

	if (!device_is_ready(report_uart)) {
		LOG_ERR("Report UART device not ready");
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return 0;
	}
	LOG_INF("Bluetooth initialized");

	bt_le_scan_cb_register(&scan_callbacks);

	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE_CONTINUOUS, NULL);
	if (err) {
		LOG_ERR("Scan start failed (err %d)", err);
		return 0;
	}

	LOG_INF("Scanning for advertisements");

	return 0;
}
