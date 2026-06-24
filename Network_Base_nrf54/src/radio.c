#include "radio.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#include "common_include.h"
#include "led.h"

LOG_MODULE_DECLARE(network_base);

#define DSA_ADV_NAME "DSA"
#define DSA_MANUFACTURER_PAYLOAD_LEN 3

struct dsa_adv {
	bool name_match;
	bool manufacturer_match;
	uint8_t id;
	uint8_t radio_status;
	uint8_t network_status;
};

static struct bt_conn *default_conn;
static struct radio_callbacks radio_cb;
static bool scanning_requested;
static bool stop_after_finished_requested;
static bool connect_in_progress;
static bool nus_ready_notified;
static uint8_t pending_beacon_id;
static uint8_t connected_beacon_id;

static bool parse_advertising_data(struct bt_data *data, void *user_data)
{
	struct dsa_adv *adv = user_data;

	switch (data->type) {
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED:
		if ((data->data_len == strlen(DSA_ADV_NAME)) &&
		    (memcmp(data->data, DSA_ADV_NAME, data->data_len) == 0)) {
			adv->name_match = true;
		}
		break;

	case BT_DATA_MANUFACTURER_DATA:
		if (data->data_len == DSA_MANUFACTURER_PAYLOAD_LEN) {
			adv->id = data->data[ADV_POS_ID];
			adv->radio_status = data->data[ADV_POS_RADIO_STATUS];
			adv->network_status = data->data[ADV_POS_NETWORK_STATUS];
			adv->manufacturer_match = true;
		}
		break;

	default:
		break;
	}

	return true;
}

static bool should_connect_to_adv(const struct dsa_adv *adv)
{
	uint8_t readout_level =
		(adv->network_status & DATA_LEVEL_MASK) >> P_SHIFT_STATUS_DATA;

	return adv->name_match && adv->manufacturer_match &&
	       (readout_level >= READOUT_LEVEL);
}

static void scan_recv(const struct bt_le_scan_recv_info *info,
		      struct net_buf_simple *ad)
{
	int err;
	struct dsa_adv adv = { 0 };
	char addr[BT_ADDR_LE_STR_LEN];
	struct net_buf_simple ad_copy = *ad;

	if (default_conn || connect_in_progress || stop_after_finished_requested) {
		return;
	}

	bt_data_parse(&ad_copy, parse_advertising_data, &adv);

	if (adv.name_match && adv.manufacturer_match) {
		printk("DSA beacon detected: id=%u\n", adv.id);
	}

	if (!should_connect_to_adv(&adv)) {
		return;
	}

	bt_addr_le_to_str(info->addr, addr, sizeof(addr));
	LOG_INF("DSA beacon matched: addr=%s id=%u radio=0x%02x network=0x%02x",
		addr, adv.id, adv.radio_status, adv.network_status);

	err = bt_le_scan_stop();
	if (err && err != -EALREADY) {
		LOG_WRN("Failed to stop scan before connect (err %d)", err);
		return;
	}

	led_set_scanning(false);
	connect_in_progress = true;
	pending_beacon_id = adv.id;

	err = bt_conn_le_create(info->addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT, &default_conn);
	if (err) {
		LOG_WRN("Failed to create connection (err %d)", err);
		default_conn = NULL;
		connect_in_progress = false;

		if (!stop_after_finished_requested) {
			(void)radio_start_scanning();
		}
	}
}

static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv,
};

static void notify_ready_for_nus(struct bt_conn *conn)
{
	if ((default_conn != conn) || nus_ready_notified) {
		return;
	}

	nus_ready_notified = true;

	if (radio_cb.connected) {
		radio_cb.connected(conn, connected_beacon_id);
	}
}

static void exchange_func(struct bt_conn *conn, uint8_t err,
			  struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(params);

	if (err) {
		LOG_WRN("MTU exchange failed (err %u)", err);
	} else {
		LOG_INF("MTU exchange complete");
	}

	notify_ready_for_nus(conn);
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	static struct bt_gatt_exchange_params exchange_params;
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	connect_in_progress = false;

	if (conn_err) {
		LOG_WRN("Connection failed: %s err 0x%02x %s", addr, conn_err,
			bt_hci_err_to_str(conn_err));

		if (default_conn == conn) {
			bt_conn_unref(default_conn);
			default_conn = NULL;
		}

		if (!stop_after_finished_requested) {
			(void)radio_start_scanning();
		}

		return;
	}

	if (!default_conn) {
		default_conn = bt_conn_ref(conn);
	}

	LOG_INF("Connected: %s", addr);
	printk("Connected: %s\n", addr);
	led_set_connected(true);
	connected_beacon_id = pending_beacon_id;
	nus_ready_notified = false;

	exchange_params.func = exchange_func;
	err = bt_gatt_exchange_mtu(conn, &exchange_params);
	if (err) {
		LOG_WRN("MTU exchange request failed (err %d)", err);
		notify_ready_for_nus(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s reason 0x%02x %s", addr, reason,
		bt_hci_err_to_str(reason));
	printk("Disconnected: %s\n", addr);

	if (default_conn == conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	connect_in_progress = false;
	nus_ready_notified = false;
	led_set_connected(false);

	if (radio_cb.disconnected) {
		radio_cb.disconnected();
	}

	if (stop_after_finished_requested) {
		scanning_requested = false;
		led_set_scanning(false);
		return;
	}

	if (scanning_requested) {
		printk("Restarting scan after disconnect\n");
		(void)radio_start_scanning();
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

int radio_init(const struct radio_callbacks *callbacks)
{
	int err;

	if (callbacks) {
		radio_cb = *callbacks;
	}

	bt_le_scan_cb_register(&scan_callbacks);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return err;
	}

	LOG_INF("Bluetooth initialized");

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		err = settings_load();
		if (err) {
			LOG_WRN("Settings load failed (err %d)", err);
		}
	}

	return 0;
}

int radio_start_scanning(void)
{
	int err;

	stop_after_finished_requested = false;
	scanning_requested = true;

	if (default_conn || connect_in_progress) {
		LOG_INF("Scan requested; waiting for current connection state");
		return 0;
	}

	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, NULL);
	if (err == -EALREADY) {
		led_set_scanning(true);
		printk("Scanning already active\n");
		return 0;
	}
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	led_set_scanning(true);
	LOG_INF("Scan started");
	printk("Scanning started\n");
	return 0;
}

int radio_stop_scanning(void)
{
	int err;

	scanning_requested = false;
	err = bt_le_scan_stop();
	if (err && err != -EALREADY) {
		LOG_ERR("Failed to stop scanning (err %d)", err);
		return err;
	}

	led_set_scanning(false);
	LOG_INF("Scan stopped");
	printk("Scanning stopped\n");
	return 0;
}

void radio_request_stop_after_finished(void)
{
	stop_after_finished_requested = true;
	scanning_requested = false;

	if (!default_conn && !connect_in_progress) {
		(void)radio_stop_scanning();
		LOG_INF("Stop requested with no active connection");
		return;
	}

	(void)bt_le_scan_stop();
	led_set_scanning(false);
	LOG_INF("Stop requested; waiting for current transfer to finish");
	printk("Scanning stopped; waiting for current transfer to finish\n");
}

void radio_transfer_finished(void)
{
	int err;

	LOG_INF("Transfer finished");

	if (!default_conn) {
		if (stop_after_finished_requested) {
			(void)radio_stop_scanning();
		}
		return;
	}

	err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err) {
		LOG_WRN("Disconnect request failed (err %d)", err);
	}
}
