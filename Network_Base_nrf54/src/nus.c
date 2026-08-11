#include "nus.h"

#include <errno.h>
#include <string.h>

#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/nus.h>
#include <bluetooth/services/nus_client.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "common_include.h"
#include "message_parser.h"
#include "output.h"

LOG_MODULE_DECLARE(network_base);

#define NUS_WRITE_TIMEOUT K_MSEC(150)
#define NUS_START_COMMAND "st"
#define NUS_RX_IDLE_TIMEOUT K_SECONDS(10)
#define NUS_RAW_PREFIX_LEN 3
#define NUS_RAW_SUFFIX_LEN 2
#define NUS_CONNECT_STATUS_LEN 1

static struct bt_nus_client nus_client;
static nus_finished_cb_t finished_cb;
static struct k_work_delayable rx_idle_timeout_work;
static uint8_t current_beacon_id;

K_SEM_DEFINE(nus_write_sem, 0, 1);

static void rx_idle_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("No NUS data received before timeout; terminating connection");

	if (finished_cb) {
		finished_cb();
	}
}

static void rx_idle_timeout_restart(void)
{
	k_work_reschedule(&rx_idle_timeout_work, NUS_RX_IDLE_TIMEOUT);
}

static void rx_idle_timeout_stop(void)
{
	(void)k_work_cancel_delayable(&rx_idle_timeout_work);
}

static void parser_finished(void)
{
	rx_idle_timeout_stop();

	if (finished_cb) {
		finished_cb();
	}
}

static void data_sent(struct bt_nus_client *nus, uint8_t err,
		      const uint8_t *const data, uint16_t len)
{
	ARG_UNUSED(nus);
	ARG_UNUSED(data);
	ARG_UNUSED(len);

	if (err) {
		LOG_WRN("NUS send ATT error 0x%02x", err);
	}

	k_sem_give(&nus_write_sem);
}

static uint8_t data_received(struct bt_nus_client *nus,
			     const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(nus);

	rx_idle_timeout_restart();

#if defined(DSA_OUTPUT_FORMAT_RAW)
	uint8_t raw_data[NUS_RAW_PREFIX_LEN + CONFIG_BT_L2CAP_TX_MTU +
			 NUS_RAW_SUFFIX_LEN] = {
		'I', 'D', current_beacon_id
	};
	size_t raw_len = MIN((size_t)len, CONFIG_BT_L2CAP_TX_MTU);
	size_t raw_total_len = NUS_RAW_PREFIX_LEN + raw_len + NUS_RAW_SUFFIX_LEN;

	memcpy(&raw_data[NUS_RAW_PREFIX_LEN], data, raw_len);
	raw_data[NUS_RAW_PREFIX_LEN + raw_len] = '\r';
	raw_data[NUS_RAW_PREFIX_LEN + raw_len + 1] = '\n';
	output_data(raw_data, raw_total_len);
#else
	message_parser_feed(current_beacon_id, data, len);
#endif
	return BT_GATT_ITER_CONTINUE;
}

static int send_start_command(void)
{
	int err;
	static const uint8_t start_command[] = NUS_START_COMMAND;

	err = bt_nus_client_send(&nus_client, start_command,
				 sizeof(start_command) - 1);
	if (err) {
		LOG_WRN("Failed to send start command (err %d)", err);
		return err;
	}

	err = k_sem_take(&nus_write_sem, NUS_WRITE_TIMEOUT);
	if (err) {
		LOG_WRN("NUS start command send timeout");
		return err;
	}

	LOG_INF("Sent NUS start command");
	return 0;
}

static void discovery_complete(struct bt_gatt_dm *dm, void *context)
{
	struct bt_nus_client *nus = context;
	int err;

	LOG_INF("NUS service discovery completed");

	err = bt_nus_handles_assign(dm, nus);
	if (err) {
		LOG_WRN("NUS handle assignment failed (err %d)", err);
		goto release;
	}

	err = bt_nus_subscribe_receive(nus);
	if (err) {
		LOG_WRN("NUS subscribe failed (err %d)", err);
		goto release;
	}

	(void)send_start_command();

release:
	bt_gatt_dm_data_release(dm);
}

static void discovery_service_not_found(struct bt_conn *conn, void *context)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(context);

	LOG_WRN("NUS service not found");
}

static void discovery_error(struct bt_conn *conn, int err, void *context)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(context);

	LOG_WRN("NUS discovery error (err %d)", err);
}

static struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_complete,
	.service_not_found = discovery_service_not_found,
	.error_found = discovery_error,
};

int nus_init(nus_finished_cb_t cb)
{
	int err;
	struct bt_nus_client_init_param init = {
		.cb = {
			.received = data_received,
			.sent = data_sent,
		},
	};

	finished_cb = cb;
	k_work_init_delayable(&rx_idle_timeout_work, rx_idle_timeout_handler);
	message_parser_init(parser_finished);

	err = bt_nus_client_init(&nus_client, &init);
	if (err) {
		LOG_ERR("NUS client init failed (err %d)", err);
		return err;
	}

	LOG_INF("NUS client initialized");
	return 0;
}

static void send_connect_status(uint8_t beacon_id, uint8_t status)
{
#if defined(DSA_OUTPUT_FORMAT_RAW)
	uint8_t raw_data[NUS_RAW_PREFIX_LEN + 1 + NUS_CONNECT_STATUS_LEN +
			 NUS_RAW_SUFFIX_LEN] = {
		'I', 'D', beacon_id, DSA_NUS_FLAG_CONNECT_STATUS, status,
		'\r', '\n'
	};

	output_data(raw_data, sizeof(raw_data));
#else
	output_messagef("ID: %u, Status: %u", beacon_id, status);
#endif
}

void nus_on_connected(struct bt_conn *conn, uint8_t beacon_id, uint8_t status)
{
	int err;

	current_beacon_id = beacon_id;
	message_parser_reset();
	rx_idle_timeout_restart();

	send_connect_status(beacon_id, status);

	err = bt_gatt_dm_start(conn, BT_UUID_NUS_SERVICE, &discovery_cb,
			       &nus_client);
	if (err) {
		LOG_WRN("NUS discovery start failed (err %d)", err);
	}
}

void nus_on_disconnected(void)
{
	rx_idle_timeout_stop();
	message_parser_reset();
}
