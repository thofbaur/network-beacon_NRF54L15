/*
 * Bluetooth host API shim. bt_le_scan_start() captures the scan callback so
 * tests can inject raw advertising payloads into the firmware's real parser
 * (shim_bt_deliver_adv()); advertising calls record their parameters and
 * advertised data for assertions.
 */
#ifndef SHIM_ZEPHYR_BLUETOOTH_BLUETOOTH_H
#define SHIM_ZEPHYR_BLUETOOTH_BLUETOOTH_H

#include "shim_common.h"
#include <zephyr/bluetooth/addr.h>
#include <zephyr/sys/util.h>

#define BT_DATA_NAME_SHORTENED 0x08
#define BT_DATA_NAME_COMPLETE 0x09
#define BT_DATA_MANUFACTURER_DATA 0xff

struct bt_data {
	uint8_t type;
	uint8_t data_len;
	const uint8_t *data;
};

#define BT_DATA(_type, _data, _data_len)                                      \
	{ .type = (_type), .data_len = (_data_len),                           \
	  .data = (const uint8_t *)(_data) }

struct net_buf_simple {
	uint8_t *data;
	uint16_t len;
	uint16_t size;
	uint8_t *__buf;
};

void net_buf_simple_clone(const struct net_buf_simple *original,
			  struct net_buf_simple *clone);
void bt_data_parse(struct net_buf_simple *ad,
		   bool (*func)(struct bt_data *data, void *user_data),
		   void *user_data);

typedef void bt_le_scan_cb_t(const bt_addr_le_t *addr, int8_t rssi,
			     uint8_t adv_type, struct net_buf_simple *buf);

#define BT_LE_SCAN_TYPE_PASSIVE 0x00
#define BT_LE_SCAN_TYPE_ACTIVE 0x01
#define BT_LE_SCAN_OPT_FILTER_DUPLICATE BIT(0)
#define BT_LE_SCAN_OPT_FILTER_ACCEPT_LIST BIT(1)

struct bt_le_scan_param {
	uint8_t type;
	uint32_t options;
	uint16_t interval;
	uint16_t window;
	uint16_t timeout;
	uint16_t interval_coded;
	uint16_t window_coded;
};

#define BT_LE_ADV_OPT_NONE 0
#define BT_LE_ADV_OPT_CONN BIT(0)
#define BT_LE_ADV_OPT_USE_IDENTITY BIT(2)

struct bt_le_adv_param {
	uint8_t id;
	uint8_t sid;
	uint8_t secondary_max_skip;
	uint32_t options;
	uint32_t interval_min;
	uint32_t interval_max;
	const bt_addr_le_t *peer;
};

typedef void (*bt_ready_cb_t)(int err);

int bt_enable(bt_ready_cb_t cb);
int bt_id_get(bt_addr_le_t *addrs, size_t *count);
int bt_le_scan_start(const struct bt_le_scan_param *param, bt_le_scan_cb_t cb);
int bt_le_scan_stop(void);
int bt_le_adv_start(const struct bt_le_adv_param *param,
		    const struct bt_data *ad, size_t ad_len,
		    const struct bt_data *sd, size_t sd_len);
int bt_le_adv_stop(void);
int bt_le_adv_update_data(const struct bt_data *ad, size_t ad_len,
			  const struct bt_data *sd, size_t sd_len);
int bt_le_filter_accept_list_add(const bt_addr_le_t *addr);

#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/conn.h>

/* ---- test control ------------------------------------------------------ */
struct shim_bt_state {
	bt_le_scan_cb_t *scan_cb;
	struct bt_le_scan_param scan_param;
	bool scanning;
	int scan_start_calls;
	int scan_stop_calls;
	struct bt_le_adv_param adv_param;
	bool advertising;
	int adv_start_calls;
	int adv_update_calls;
	uint8_t adv_name[32];
	uint8_t adv_name_len;
	uint8_t adv_mfg[32];
	uint8_t adv_mfg_len;
	int accept_list_adds;
	/* error injection, returned by the next calls */
	int scan_start_err;
	int scan_stop_err;
	int adv_start_err;
	int adv_stop_err;
	int adv_update_err;
	int accept_list_err;
	int enable_err;
	bt_addr_le_t identity;
	size_t identity_count;
};

extern struct shim_bt_state shim_bt;

void shim_bt_reset(void);
/* Deliver one advertising report (raw AD structures) to the captured scan
 * callback. Returns false when no scan callback has been registered.
 */
bool shim_bt_deliver_adv(const bt_addr_le_t *addr, int8_t rssi,
			 const uint8_t *ad, uint16_t ad_len);
/* Build "len,type,data" AD structures: name + manufacturer data. */
uint16_t shim_bt_build_adv(uint8_t *out, size_t out_size, const char *name,
			   const uint8_t *mfg, uint8_t mfg_len);

#endif /* SHIM_ZEPHYR_BLUETOOTH_BLUETOOTH_H */
