#include "radio.h"

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "common_include.h"
#include "led.h"

LOG_MODULE_DECLARE(network_base);

#define DSA_MANUFACTURER_PAYLOAD_LEN 3
#define DSA_ATT_PAYLOAD_OVERHEAD 3U
#define DSA_CONN_INTERVAL_MIN 6U
#define DSA_CONN_INTERVAL_MAX 12U
#define DSA_CONN_LATENCY 0U
#define DSA_CONN_TIMEOUT 400U
#define DSA_FINISH_DISCONNECT_DELAY K_MSEC(50)

static const char * const dsa_allowed_adv_names[] = {
	"DSA",
	"DSL",
};

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
static bool transfer_finish_pending;
static uint8_t pending_beacon_id;
static uint8_t pending_beacon_status;
static uint8_t connected_beacon_id;
static uint8_t connected_beacon_status;
static uint8_t readout_level_threshold = CONFIG_DSA_READOUT_LEVEL;
static const struct bt_le_conn_param dsa_conn_param =
	BT_LE_CONN_PARAM_INIT(DSA_CONN_INTERVAL_MIN, DSA_CONN_INTERVAL_MAX,
			      DSA_CONN_LATENCY, DSA_CONN_TIMEOUT);
static struct k_work_delayable finish_disconnect_work;

static int start_scanning(void);

static const char *phy_to_str(uint8_t phy)
{
	switch (phy) {
	case BT_GAP_LE_PHY_1M:
		return "1M";
	case BT_GAP_LE_PHY_2M:
		return "2M";
	case BT_GAP_LE_PHY_CODED:
		return "coded";
	default:
		return "unknown";
	}
}

static void log_conn_throughput_state(struct bt_conn *conn, const char *stage)
{
	struct bt_conn_info info;
	uint16_t att_mtu;
	uint16_t att_payload;
	int err;

	err = bt_conn_get_info(conn, &info);
	if (err) {
		LOG_WRN("%s: failed to read connection info (err %d)", stage, err);
		return;
	}

	att_mtu = bt_gatt_get_mtu(conn);
	att_payload = att_mtu > DSA_ATT_PAYLOAD_OVERHEAD ?
		      att_mtu - DSA_ATT_PAYLOAD_OVERHEAD : 0U;

	LOG_INF("%s: ATT MTU=%u payload=%u cfg L2CAP_TX_MTU=%d ACL_TX_SIZE=%d ACL_RX_SIZE=%d",
		stage, att_mtu, att_payload,
		CONFIG_BT_L2CAP_TX_MTU, CONFIG_BT_BUF_ACL_TX_SIZE,
		CONFIG_BT_BUF_ACL_RX_SIZE);

	if (info.type != BT_CONN_TYPE_LE) {
		LOG_INF("%s: non-LE connection type=%u", stage, info.type);
		return;
	}

	LOG_INF("%s: role=%s interval=%u us latency=%u timeout=%u ms security=L%u",
		stage,
		info.role == BT_CONN_ROLE_CENTRAL ? "central" : "peripheral",
		info.le.interval_us, info.le.latency, info.le.timeout * 10U,
		info.security.level);

#if defined(CONFIG_BT_USER_PHY_UPDATE)
	if (info.le.phy) {
		LOG_INF("%s: PHY tx=%s(0x%02x) rx=%s(0x%02x)", stage,
			phy_to_str(info.le.phy->tx_phy), info.le.phy->tx_phy,
			phy_to_str(info.le.phy->rx_phy), info.le.phy->rx_phy);
	} else {
		LOG_INF("%s: PHY info unavailable", stage);
	}
#endif

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
	if (info.le.data_len) {
		LOG_INF("%s: data length tx=%u/%u us rx=%u/%u us", stage,
			info.le.data_len->tx_max_len,
			info.le.data_len->tx_max_time,
			info.le.data_len->rx_max_len,
			info.le.data_len->rx_max_time);
	} else {
		LOG_INF("%s: data length info unavailable", stage);
	}
#endif
}

static void request_link_throughput_updates(struct bt_conn *conn)
{
	int err;

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
	err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (err) {
		LOG_WRN("LE data length update request failed (err %d)", err);
	} else {
		LOG_INF("LE data length update requested: tx_len=%u tx_time=%u us",
			BT_GAP_DATA_LEN_MAX, BT_GAP_DATA_TIME_MAX);
	}
#else
	LOG_INF("LE data length update not requested; CONFIG_BT_USER_DATA_LEN_UPDATE=n");
#endif

#if defined(CONFIG_BT_USER_PHY_UPDATE)
	err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (err) {
		LOG_WRN("LE 2M PHY update request failed (err %d)", err);
	} else {
		LOG_INF("LE 2M PHY update requested");
	}
#else
	LOG_INF("LE PHY update not requested; CONFIG_BT_USER_PHY_UPDATE=n");
#endif
}

static void finish_disconnect_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	if (!default_conn) {
		transfer_finish_pending = false;
		if (stop_after_finished_requested) {
			(void)radio_stop_scanning();
		}
		return;
	}

	log_conn_throughput_state(default_conn, "Disconnect request");

	err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err) {
		LOG_WRN("Disconnect request failed (err %d)", err);
		transfer_finish_pending = false;
	}
}

static bool adv_name_allowed(const uint8_t *data, uint8_t len)
{
	for (size_t i = 0; i < ARRAY_SIZE(dsa_allowed_adv_names); i++) {
		const char *name = dsa_allowed_adv_names[i];

		if ((len == strlen(name)) && (memcmp(data, name, len) == 0)) {
			return true;
		}
	}

	return false;
}

static bool parse_advertising_data(struct bt_data *data, void *user_data)
{
	struct dsa_adv *adv = user_data;

	switch (data->type) {
	case BT_DATA_NAME_COMPLETE:
	case BT_DATA_NAME_SHORTENED:
		if (adv_name_allowed(data->data, data->data_len)) {
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
	       (readout_level >= readout_level_threshold);
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
		LOG_INF("DSA beacon detected: id=%u", adv.id);
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
	pending_beacon_status = adv.radio_status;

	LOG_INF("Creating connection with interval=%u-%u (%u-%u us) latency=%u timeout=%u ms",
		dsa_conn_param.interval_min, dsa_conn_param.interval_max,
		BT_CONN_INTERVAL_TO_US(dsa_conn_param.interval_min),
		BT_CONN_INTERVAL_TO_US(dsa_conn_param.interval_max),
		dsa_conn_param.latency, dsa_conn_param.timeout * 10U);

	err = bt_conn_le_create(info->addr, BT_CONN_LE_CREATE_CONN, &dsa_conn_param,
				&default_conn);
	if (err) {
		LOG_WRN("Failed to create connection (err %d)", err);
		default_conn = NULL;
		connect_in_progress = false;

		if (!stop_after_finished_requested) {
			(void)start_scanning();
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
	log_conn_throughput_state(conn, "NUS start");

	if (radio_cb.connected) {
		radio_cb.connected(conn, connected_beacon_id, connected_beacon_status);
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

	log_conn_throughput_state(conn, "MTU exchange callback");
	notify_ready_for_nus(conn);
}

static bool le_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	LOG_WRN("LE parameter request rejected: min=%u (%u us) max=%u (%u us) latency=%u timeout=%u ms",
		param->interval_min, BT_CONN_INTERVAL_TO_US(param->interval_min),
		param->interval_max, BT_CONN_INTERVAL_TO_US(param->interval_max),
		param->latency, param->timeout * 10U);
	log_conn_throughput_state(conn, "LE parameter request");

	return false;
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	LOG_INF("LE parameters updated: interval=%u (%u us) latency=%u timeout=%u ms",
		interval, BT_CONN_INTERVAL_TO_US(interval), latency,
		timeout * 10U);
	log_conn_throughput_state(conn, "LE parameter callback");
}

#if defined(CONFIG_BT_USER_PHY_UPDATE)
static void le_phy_updated(struct bt_conn *conn,
			   struct bt_conn_le_phy_info *param)
{
	LOG_INF("LE PHY updated: tx=%s(0x%02x) rx=%s(0x%02x)",
		phy_to_str(param->tx_phy), param->tx_phy,
		phy_to_str(param->rx_phy), param->rx_phy);
	log_conn_throughput_state(conn, "LE PHY callback");
}
#endif

#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
static void le_data_len_updated(struct bt_conn *conn,
				struct bt_conn_le_data_len_info *info)
{
	LOG_INF("LE data length updated: tx=%u/%u us rx=%u/%u us",
		info->tx_max_len, info->tx_max_time, info->rx_max_len,
		info->rx_max_time);
	log_conn_throughput_state(conn, "LE data length callback");
}
#endif

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
			(void)start_scanning();
		}

		return;
	}

	if (!default_conn) {
		default_conn = bt_conn_ref(conn);
	}

	LOG_INF("Connected: %s", addr);
	led_set_connected(true);
	connected_beacon_id = pending_beacon_id;
	connected_beacon_status = pending_beacon_status;
	nus_ready_notified = false;
	log_conn_throughput_state(conn, "Connected initial");
	request_link_throughput_updates(conn);

	exchange_params.func = exchange_func;
	LOG_INF("Requesting ATT MTU exchange");
	err = bt_gatt_exchange_mtu(conn, &exchange_params);
	if (err) {
		LOG_WRN("MTU exchange request failed (err %d)", err);
		log_conn_throughput_state(conn, "MTU exchange request failed");
		notify_ready_for_nus(conn);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s reason 0x%02x %s", addr, reason,
		bt_hci_err_to_str(reason));

	if (default_conn == conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}

	connect_in_progress = false;
	nus_ready_notified = false;
	transfer_finish_pending = false;
	(void)k_work_cancel_delayable(&finish_disconnect_work);
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
		LOG_INF("Restarting scan after disconnect");
		(void)start_scanning();
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_req = le_param_req,
	.le_param_updated = le_param_updated,
#if defined(CONFIG_BT_USER_PHY_UPDATE)
	.le_phy_updated = le_phy_updated,
#endif
#if defined(CONFIG_BT_USER_DATA_LEN_UPDATE)
	.le_data_len_updated = le_data_len_updated,
#endif
};

int radio_init(const struct radio_callbacks *callbacks)
{
	int err;

	if (callbacks) {
		radio_cb = *callbacks;
	}

	k_work_init_delayable(&finish_disconnect_work, finish_disconnect_handler);
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

static int start_scanning(void)
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
		LOG_INF("Scanning already active");
		return 0;
	}
	if (err) {
		LOG_ERR("Scanning failed to start (err %d)", err);
		return err;
	}

	led_set_scanning(true);
	LOG_INF("Scan started");
	return 0;
}

int radio_start_scanning_with_level(uint8_t min_data_level)
{
	readout_level_threshold = min_data_level;
	LOG_INF("Scan requested with minimum data level %u", min_data_level);
	return start_scanning();
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
}

void radio_transfer_finished(void)
{
	LOG_INF("Transfer finished");

	if (transfer_finish_pending) {
		LOG_INF("Disconnect already pending after transfer finished");
		return;
	}

	transfer_finish_pending = true;

	if (!default_conn) {
		transfer_finish_pending = false;
		if (stop_after_finished_requested) {
			(void)radio_stop_scanning();
		}
		return;
	}

	LOG_INF("Scheduling disconnect after transfer finished");
	k_work_schedule(&finish_disconnect_work, DSA_FINISH_DISCONNECT_DELAY);
}
