#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>

#include <bluetooth/services/nus.h>

#include "nus.h"
#include "common_include.h"
#include "network.h"

#define NUS_ATT_NOTIFY_HEADER_LEN 3
#define NUS_MAX_PAYLOAD_LEN (CONFIG_BT_L2CAP_TX_MTU - NUS_ATT_NOTIFY_HEADER_LEN)
#define NUS_MAX_IN_FLIGHT 2
#define NUS_SEND_SLOT_WAIT_MS 10
#define NUS_SEND_SLOT_TIMEOUT_MS 1000
#define NUS_SEND_RETRY_WAIT_MS 10
#define NUS_SEND_RETRY_TIMEOUT_MS 1000
#define NUS_TRANSFER_STACK_SIZE 2048
#define NUS_TRANSFER_PRIORITY 5
/* TODO: make NUS idle timeout configurable for production tuning. */
#define NUS_IDLE_TIMEOUT_MS 20000

static struct bt_conn *current_conn;
static bool nus_notifications_enabled;
static bool disconnect_when_sent;
static bool transfer_active;
static atomic_t pending_nus_sends;
static struct bt_gatt_exchange_params mtu_exchange_params;
static struct bt_conn *transfer_conn;

static void transfer_work_handler(struct k_work *work);
static void nus_idle_timeout_handler(struct k_work *work);
static void disconnect_nus_connection(struct bt_conn *conn);

static K_WORK_DEFINE(transfer_work, transfer_work_handler);
static K_WORK_DELAYABLE_DEFINE(nus_idle_timeout_work, nus_idle_timeout_handler);
K_THREAD_STACK_DEFINE(transfer_stack, NUS_TRANSFER_STACK_SIZE);
static struct k_work_q transfer_work_q;

static void nus_idle_timeout_refresh(void)
{
	k_work_reschedule(&nus_idle_timeout_work, K_MSEC(NUS_IDLE_TIMEOUT_MS));
}

static void nus_idle_timeout_cancel(void)
{
	k_work_cancel_delayable(&nus_idle_timeout_work);
}

static void nus_idle_timeout_handler(struct k_work *work)
{
	struct bt_conn *conn;

	ARG_UNUSED(work);

	conn = current_conn;
	if (!conn) {
		return;
	}

	printk("NUS idle timeout, disconnecting\n");
	disconnect_nus_connection(conn);
}

static int nus_send_tracked(struct bt_conn *conn, const void *data, uint16_t len)
{
	int err;
	int64_t deadline;
	int64_t retry_deadline;

	if (!nus_notifications_enabled) {
		return -EINVAL;
	}

	deadline = k_uptime_get() + NUS_SEND_SLOT_TIMEOUT_MS;
	while (atomic_get(&pending_nus_sends) >= NUS_MAX_IN_FLIGHT) {
		if (current_conn != conn || !nus_notifications_enabled) {
			return -ECONNRESET;
		}

		if (k_uptime_get() >= deadline) {
			return -EAGAIN;
		}

		k_sleep(K_MSEC(NUS_SEND_SLOT_WAIT_MS));
	}

	atomic_inc(&pending_nus_sends);
	retry_deadline = k_uptime_get() + NUS_SEND_RETRY_TIMEOUT_MS;
	do {
		err = bt_nus_send(conn, data, len);
		if (!err) {
			return 0;
		}

		if (err != -EAGAIN) {
			break;
		}

		if (current_conn != conn || !nus_notifications_enabled) {
			err = -ECONNRESET;
			break;
		}

		k_sleep(K_MSEC(NUS_SEND_RETRY_WAIT_MS));
	} while (k_uptime_get() < retry_deadline);

	atomic_dec(&pending_nus_sends);
	return err;
}

static void disconnect_nus_connection(struct bt_conn *conn)
{
	if (bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN)) {
		printk("Failed to disconnect NUS connection\n");
	}
}

static void print_conn_info(struct bt_conn *conn, const char *prefix)
{
	struct bt_conn_info info;
	int err;

	err = bt_conn_get_info(conn, &info);
	if (err) {
		printk("%s: failed to read connection info (err %d)\n", prefix, err);
		return;
	}

	if (info.type != BT_CONN_TYPE_LE) {
		printk("%s: non-LE connection type %u\n", prefix, info.type);
		return;
	}

	printk("%s: role=%s interval=%u us latency=%u timeout=%u units\n",
	       prefix,
	       info.role == BT_CONN_ROLE_PERIPHERAL ? "peripheral" : "central",
	       info.le.interval_us, info.le.latency, info.le.timeout);
}

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t att_err,
			    struct bt_gatt_exchange_params *params)
{
	printk("MTU exchange %s", att_err ? "failed" : "successful");
	if (att_err) {
		printk(" (ATT err 0x%02x)", att_err);
	}
	printk(", negotiated MTU %u\n", bt_gatt_get_mtu(conn));
}

static void request_connection_params(struct bt_conn *conn)
{
	int err;
	const struct bt_conn_le_phy_param phy = {
		.pref_tx_phy = BT_GAP_LE_PHY_2M,
		.pref_rx_phy = BT_GAP_LE_PHY_2M,
	};
	const struct bt_conn_le_data_len_param data_len = {
		.tx_max_len = BT_GAP_DATA_LEN_MAX,
		.tx_max_time = BT_GAP_DATA_TIME_MAX,
	};
	const struct bt_le_conn_param conn_param = {
		.interval_min = BT_GAP_MS_TO_CONN_INTERVAL(7.5),
		.interval_max = BT_GAP_MS_TO_CONN_INTERVAL(15),
		.latency = 0,
		.timeout = BT_GAP_MS_TO_CONN_TIMEOUT(4000),
	};

	printk("Requesting maximum NUS TX connection parameters\n");
	printk("Requested PHY: TX=2M RX=2M\n");
	printk("Requested data length: tx_max_len=%u tx_max_time=%u\n",
	       data_len.tx_max_len, data_len.tx_max_time);
	printk("Requested connection interval: %u-%u units, latency=%u, timeout=%u units\n",
	       conn_param.interval_min, conn_param.interval_max,
	       conn_param.latency, conn_param.timeout);

	mtu_exchange_params.func = mtu_exchange_cb;
	err = bt_gatt_exchange_mtu(conn, &mtu_exchange_params);
	if (err) {
		printk("MTU exchange request failed (err %d)\n", err);
	} else {
		printk("MTU exchange request sent\n");
	}

	err = bt_conn_le_phy_update(conn, &phy);
	if (err) {
		printk("PHY update request failed (err %d)\n", err);
	} else {
		printk("PHY update request sent\n");
	}

	err = bt_conn_le_data_len_update(conn, &data_len);
	if (err) {
		printk("Data length update request failed (err %d)\n", err);
	} else {
		printk("Data length update request sent\n");
	}

	err = bt_conn_le_param_update(conn, &conn_param);
	if (err) {
		printk("Connection parameter update request failed (err %d)\n", err);
	} else {
		printk("Connection parameter update request sent\n");
	}
}

static int send_uptime(struct bt_conn *conn)
{
	int err;
	uint8_t buffer[1 + sizeof(uint32_t)];

	buffer[0] = DSA_NUS_FLAG_TIME;
	sys_put_be32((uint32_t)k_uptime_seconds(), &buffer[1]);

	err = nus_send_tracked(conn, buffer, sizeof(buffer));
	if (err) {
		printk("Failed to send NUS uptime response (err %d)\n", err);
	}

	return err;
}

static int send_uptime_contacts_voltage(struct bt_conn *conn)
{
	int err;
	uint8_t buffer[1 + sizeof(uint32_t)+sizeof(uint16_t)+sizeof(uint16_t)];

	buffer[0] = DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE;
	sys_put_be32((uint32_t)k_uptime_seconds(), &buffer[1]);
	sys_put_be32((uint16_t)network_get_contact_count(),&buffer[5]);
	err = nus_send_tracked(conn, buffer, sizeof(buffer));
	if (err) {
		printk("Failed to send NUS uptime response (err %d)\n", err);
	}

	return err;
}

static int send_finished(struct bt_conn *conn)
{
	int err;
	uint8_t buffer[1 + 8];

	buffer[0] = DSA_NUS_FLAG_CONTROL;
	
	strcpy(&buffer[1], "finished");


	err = nus_send_tracked(conn, buffer, sizeof(buffer));
	if (err) {
		printk("Failed to send NUS finished message (err %d)\n", err);
	}

	return err;

}



static int send_networkdata(struct bt_conn *conn)
{
	int err;
	uint16_t max_payload = bt_gatt_get_mtu(conn);
	uint16_t payload_len;
	uint16_t bytes_written = 0;
	uint16_t contact_payload_len;
	uint8_t buffer[NUS_MAX_PAYLOAD_LEN];
	
	if (max_payload > NUS_ATT_NOTIFY_HEADER_LEN) {
		max_payload -= NUS_ATT_NOTIFY_HEADER_LEN;
	} else {
		max_payload = 0;
	}

	printk("NUS max payload: %u bytes\n", max_payload);

	payload_len = MIN(max_payload, (uint16_t)sizeof(buffer));
	if (payload_len <= 1) {
		return -EMSGSIZE;
	}

	contact_payload_len = payload_len - 1;
	buffer[0] = DSA_NUS_FLAG_DATA;

	do {
		bytes_written = network_peek_contact(&buffer[1], contact_payload_len);
		if (bytes_written > 0) {
			err = nus_send_tracked(conn, buffer, bytes_written + 1);
			if (err) {
				printk("Failed to send NUS network data (err %d)\n", err);
				return err;
			}

			network_drop_contact_bytes(bytes_written);
		}
	} while (bytes_written > 0);

	return 0;
}


static void nus_received(struct bt_conn *conn, const uint8_t *const data, uint16_t len)
{
	nus_idle_timeout_refresh();

	printk("NUS RX len=%u first=%02x %02x %02x %02x\n", len,
	       len > 0 ? data[0] : 0,
	       len > 1 ? data[1] : 0,
	       len > 2 ? data[2] : 0,
	       len > 3 ? data[3] : 0);

	if (len == 2 && memcmp(data, "st", 2) == 0) {
		if (transfer_active) {
			printk("NUS transfer already active, ignoring start command\n");
			return;
		}
		if (!nus_notifications_enabled) {
			printk("NUS transfer requested before notifications are enabled\n");
			return;
		}

		transfer_active = true;
		transfer_conn = bt_conn_ref(conn);
		k_work_submit_to_queue(&transfer_work_q, &transfer_work);
	}
}

static void transfer_work_handler(struct k_work *work)
{
	int err;
	struct bt_conn *conn;

	ARG_UNUSED(work);

	conn = transfer_conn;
	transfer_conn = NULL;
	if (!conn) {
		transfer_active = false;
		return;
	}

	printk("Starting sending data\n");
	err = send_uptime(conn);
	if (err) {
		transfer_active = false;
		bt_conn_unref(conn);
		return;
	}
	printk("Sent time\n");

	
	err = send_uptime_contacts_voltage(conn);
	if (err) {
		transfer_active = false;
		bt_conn_unref(conn);
		return;
	}


	printk("Starting sending data\n");
	err = send_networkdata(conn);
	if (err) {
		transfer_active = false;
		bt_conn_unref(conn);
		return;
	}

	err = send_finished(conn);
	if (err) {
		printk("Failed to send NUS finished response (err %d)\n", err);
		transfer_active = false;
	} else {
		disconnect_when_sent = true;
		printk("Sent finished message");
	}

	bt_conn_unref(conn);
}

static void nus_sent(struct bt_conn *conn)
{
	nus_idle_timeout_refresh();

	if (atomic_get(&pending_nus_sends) > 0) {
		atomic_dec(&pending_nus_sends);
	}

	if (disconnect_when_sent && atomic_get(&pending_nus_sends) == 0) {
		disconnect_when_sent = false;
		disconnect_nus_connection(conn);
	}
}

static void nus_send_enabled(enum bt_nus_send_status status)
{
	nus_notifications_enabled = (status == BT_NUS_SEND_STATUS_ENABLED);
	printk("NUS TX notifications %s\n", nus_notifications_enabled ? "enabled" : "disabled");
}

static struct bt_nus_cb nus_cb = {
	.received = nus_received,
	.sent = nus_sent,
	.send_enabled = nus_send_enabled,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Connection event from %s\n", addr);

	if (err) {
		printk("Connection failed from %s (HCI err 0x%02x)\n", addr, err);
		return;
	}

	printk("Connected: %s\n", addr);
	print_conn_info(conn, "Initial connection parameters");

	if (current_conn) {
		printk("Replacing previous connection reference\n");
		bt_conn_unref(current_conn);
	}

	current_conn = bt_conn_ref(conn);
	nus_idle_timeout_refresh();
	request_connection_params(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("Disconnected: %s (reason 0x%02x)\n", addr, reason);

	if (current_conn == conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	nus_notifications_enabled = false;
	disconnect_when_sent = false;
	transfer_active = false;
	nus_idle_timeout_cancel();
	if (transfer_conn) {
		bt_conn_unref(transfer_conn);
		transfer_conn = NULL;
	}
	atomic_set(&pending_nus_sends, 0);
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);

	printk("Connection parameters updated: interval=%u units latency=%u timeout=%u units\n",
	       interval, latency, timeout);
}

static void le_phy_updated(struct bt_conn *conn, struct bt_conn_le_phy_info *param)
{
	ARG_UNUSED(conn);

	printk("PHY updated: TX=%u RX=%u\n", param->tx_phy, param->rx_phy);
}

static void le_data_len_updated(struct bt_conn *conn,
				struct bt_conn_le_data_len_info *info)
{
	ARG_UNUSED(conn);

	printk("Data length updated: tx_len=%u tx_time=%u rx_len=%u rx_time=%u\n",
	       info->tx_max_len, info->tx_max_time,
	       info->rx_max_len, info->rx_max_time);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
	.le_phy_updated = le_phy_updated,
	.le_data_len_updated = le_data_len_updated,
};

int nus_service_init(void)
{
	int err;

	err = bt_nus_init(&nus_cb);
	if (err) {
		printk("Failed to initialize NUS (err %d)\n", err);
		return err;
	}

	k_work_queue_start(&transfer_work_q, transfer_stack,
			   K_THREAD_STACK_SIZEOF(transfer_stack),
			   NUS_TRANSFER_PRIORITY, NULL);

	return 0;
}

bool nus_is_connected(void)
{
	return current_conn != NULL;
}
