#include "nus.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/nus.h>
#include <bluetooth/services/nus_client.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "common_include.h"

LOG_MODULE_DECLARE(network_base);

#define NUS_WRITE_TIMEOUT K_MSEC(150)
#define NUS_START_COMMAND "st"

#define DSA_TIME_LEN 4
#define DSA_DATA_SET_LEN 5
#define DSA_VOLTAGE_LEN 2
#define DSA_CONTROL_LEN 8
#define DSA_TIME_CONTACT_VOLTAGE_LEN 8
#define NUS_RX_IDLE_TIMEOUT K_SECONDS(10)

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

static size_t expected_len_for_flag(uint8_t flag)
{
	switch (flag) {
	case DSA_NUS_FLAG_TIME:
		return DSA_TIME_LEN;
	case DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE:
		return DSA_TIME_CONTACT_VOLTAGE_LEN;
	case DSA_NUS_FLAG_DATA:
		return DSA_DATA_SET_LEN;
	case DSA_NUS_FLAG_VOLTAGE:
		return DSA_VOLTAGE_LEN;
	case DSA_NUS_FLAG_CONTROL:
		return DSA_CONTROL_LEN;
	default:
		return 0;
	}
}

static bool is_known_flag(uint8_t byte)
{
	return expected_len_for_flag(byte) != 0;
}

static void print_bytes(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		printk("%02x%s", data[i], (i + 1 == len) ? "" : " ");
	}
}

static uint32_t uint32_be_decode(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8)|data[3];
}

static uint32_t uint24_be_decode(const uint8_t *data)
{
	return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
}

static uint32_t uint16_be_decode(const uint8_t *data)
{
	return ((uint32_t)data[0] << 8) | data[1];
}

static void handle_time_package(const uint8_t *data)
{
	uint32_t timer = uint32_be_decode(data);

	printk("ID:%u Current Timer:%u\n", current_beacon_id, timer);
}

static void handle_time_contact_voltage_package(const uint8_t *data)
{
	uint32_t timer = uint32_be_decode(data);
	uint16_t contact_count = uint16_be_decode(&data[4]);
	uint16_t voltage = uint16_be_decode(&data[6]);
	printk("ID:%u Current Timer:%u\n", current_beacon_id, timer);
	printk("ID:%u Contact Count:%u\n", current_beacon_id, contact_count);
	printk("ID:%u Voltage:%u (not implemented yet)\n", current_beacon_id, voltage);

}


static void handle_data_package(const uint8_t *data)
{
	uint8_t contact_id = data[0];
	uint32_t timer = uint24_be_decode(&data[1]);
	uint8_t negative_rssi = data[4];

	printk("ID:%u Contact-ID:%u Timer:%u RSSI:-%u\n",
	       current_beacon_id, contact_id, timer, negative_rssi);
}

static void handle_data_block(const uint8_t *data, size_t len)
{
	size_t set_count = len / DSA_DATA_SET_LEN;

	for (size_t i = 0; i < set_count; i++) {
		handle_data_package(&data[i * DSA_DATA_SET_LEN]);
	}
}

static void handle_voltage_package(const uint8_t *data)
{
	uint16_t voltage = uint16_be_decode(data);

	printk("ID:%u VOLTAGE:%u\n", current_beacon_id, voltage);
}

static void handle_control_package(const uint8_t *data)
{
	
	if (memcmp(data, "finished", DSA_CONTROL_LEN) == 0) {
		printk("ID:%u Transfer complete. Disconnecting", current_beacon_id);
		printk("\n");
	
		LOG_INF("Received finished control package");
		rx_idle_timeout_stop();

		if (finished_cb) {
			finished_cb();
		}
	}
}

static void handle_default_package(const uint8_t *data, size_t len)
{
	printk("DEFAULT placeholder: uint8=");

	for (size_t i = 0; i < len; i++) {
		printk("%u%s", data[i], (i + 1 == len) ? "" : " ");
	}

	printk("\n");
}

static void handle_complete_package(uint8_t flag, const uint8_t *data, size_t len)
{
	switch (flag) {
	case DSA_NUS_FLAG_TIME:
		if (len != DSA_TIME_LEN) {
			LOG_WRN("Invalid TIME package length %u", (unsigned int)len);
			break;
		}
		handle_time_package(data);
		break;
	case DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE:
		if (len != DSA_TIME_CONTACT_VOLTAGE_LEN) {
			LOG_WRN("Invalid TIME+CONTACT+VOLTAGE package length %u", (unsigned int)len);
			break;
		}	
		handle_time_contact_voltage_package(data);
		break;
	case DSA_NUS_FLAG_DATA:
		if ((len == 0) || ((len % DSA_DATA_SET_LEN) != 0)) {
			LOG_WRN("Invalid DATA block length %u", (unsigned int)len);
			break;
		}
		handle_data_block(data, len);
		break;
	case DSA_NUS_FLAG_VOLTAGE:
		if (len != DSA_VOLTAGE_LEN) {
			LOG_WRN("Invalid VOLTAGE package length %u", (unsigned int)len);
			break;
		}
		handle_voltage_package(data);
		break;
	case DSA_NUS_FLAG_CONTROL:
		if (len != DSA_CONTROL_LEN) {
			LOG_WRN("Invalid CONTROL package length %u", (unsigned int)len);
			break;
		}
		handle_control_package(data);
		break;
	default:
		handle_default_package(data, len);
		break;
	}
}

static void parser_reset(void)
{
	/* Stateless parser: each NUS notification carries one complete package. */
}

static void parser_feed_package(const uint8_t *data, uint16_t len)
{
	uint8_t flag;

	if (len < 1) {
		LOG_WRN("Ignoring empty NUS package");
		return;
	}

	flag = data[0];
	if (!is_known_flag(flag)) {
		LOG_INF("No known NUS flag matched; using default data package");
		handle_default_package(data, len);
		return;
	}

	handle_complete_package(flag, &data[1], len - 1);
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
	parser_feed_package(data, len);
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
	parser_reset();

	err = bt_nus_client_init(&nus_client, &init);
	if (err) {
		LOG_ERR("NUS client init failed (err %d)", err);
		return err;
	}

	LOG_INF("NUS client initialized");
	return 0;
}

void nus_on_connected(struct bt_conn *conn, uint8_t beacon_id)
{
	int err;

	current_beacon_id = beacon_id;
	parser_reset();
	rx_idle_timeout_restart();

	err = bt_gatt_dm_start(conn, BT_UUID_NUS_SERVICE, &discovery_cb,
			       &nus_client);
	if (err) {
		LOG_WRN("NUS discovery start failed (err %d)", err);
	}
}

void nus_on_disconnected(void)
{
	rx_idle_timeout_stop();
	parser_reset();
}
