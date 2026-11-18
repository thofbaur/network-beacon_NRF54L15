#include <stdio.h>

#include <zephyr/bluetooth/bluetooth.h>

struct shim_bt_state shim_bt;

void shim_bt_reset(void)
{
	memset(&shim_bt, 0, sizeof(shim_bt));
	shim_bt.identity_count = 1;
}

int bt_enable(bt_ready_cb_t cb)
{
	if (cb) {
		cb(shim_bt.enable_err);
	}
	return shim_bt.enable_err;
}

int bt_id_get(bt_addr_le_t *addrs, size_t *count)
{
	size_t n = MIN(*count, shim_bt.identity_count);

	for (size_t i = 0; i < n; i++) {
		addrs[i] = shim_bt.identity;
	}
	*count = n;
	return 0;
}

void net_buf_simple_clone(const struct net_buf_simple *original,
			  struct net_buf_simple *clone)
{
	*clone = *original;
}

/* Same parsing rules as Zephyr's bt_data_parse(): each AD structure is
 * <len><type><len-1 data bytes>; a zero length ends parsing, and a
 * structure running past the end of the buffer is treated as malformed.
 */
void bt_data_parse(struct net_buf_simple *ad,
		   bool (*func)(struct bt_data *data, void *user_data),
		   void *user_data)
{
	while (ad->len > 1) {
		struct bt_data data;
		uint8_t len = ad->data[0];

		if (len == 0) {
			return;
		}
		if (len > ad->len - 1) {
			return;
		}
		data.type = ad->data[1];
		data.data_len = len - 1;
		data.data = &ad->data[2];
		if (!func(&data, user_data)) {
			return;
		}
		ad->data += len + 1;
		ad->len -= len + 1;
	}
}

int bt_le_scan_start(const struct bt_le_scan_param *param, bt_le_scan_cb_t cb)
{
	shim_bt.scan_start_calls++;
	if (shim_bt.scan_start_err) {
		return shim_bt.scan_start_err;
	}
	shim_bt.scan_param = *param;
	shim_bt.scan_cb = cb;
	shim_bt.scanning = true;
	return 0;
}

int bt_le_scan_stop(void)
{
	shim_bt.scan_stop_calls++;
	if (shim_bt.scan_stop_err) {
		return shim_bt.scan_stop_err;
	}
	if (!shim_bt.scanning) {
		return -EALREADY;
	}
	shim_bt.scanning = false;
	return 0;
}

static void record_ad(const struct bt_data *ad, size_t ad_len)
{
	for (size_t i = 0; i < ad_len; i++) {
		if (ad[i].type == BT_DATA_NAME_COMPLETE) {
			shim_bt.adv_name_len =
				(uint8_t)MIN(ad[i].data_len, sizeof(shim_bt.adv_name));
			memcpy(shim_bt.adv_name, ad[i].data, shim_bt.adv_name_len);
		} else if (ad[i].type == BT_DATA_MANUFACTURER_DATA) {
			shim_bt.adv_mfg_len =
				(uint8_t)MIN(ad[i].data_len, sizeof(shim_bt.adv_mfg));
			memcpy(shim_bt.adv_mfg, ad[i].data, shim_bt.adv_mfg_len);
		}
	}
}

int bt_le_adv_start(const struct bt_le_adv_param *param,
		    const struct bt_data *ad, size_t ad_len,
		    const struct bt_data *sd, size_t sd_len)
{
	ARG_UNUSED(sd);
	ARG_UNUSED(sd_len);
	shim_bt.adv_start_calls++;
	if (shim_bt.adv_start_err) {
		return shim_bt.adv_start_err;
	}
	shim_bt.adv_param = *param;
	shim_bt.advertising = true;
	record_ad(ad, ad_len);
	return 0;
}

int bt_le_adv_stop(void)
{
	if (shim_bt.adv_stop_err) {
		return shim_bt.adv_stop_err;
	}
	shim_bt.advertising = false;
	return 0;
}

int bt_le_adv_update_data(const struct bt_data *ad, size_t ad_len,
			  const struct bt_data *sd, size_t sd_len)
{
	ARG_UNUSED(sd);
	ARG_UNUSED(sd_len);
	shim_bt.adv_update_calls++;
	if (shim_bt.adv_update_err) {
		return shim_bt.adv_update_err;
	}
	record_ad(ad, ad_len);
	return 0;
}

int bt_le_filter_accept_list_add(const bt_addr_le_t *addr)
{
	ARG_UNUSED(addr);
	if (shim_bt.accept_list_err) {
		return shim_bt.accept_list_err;
	}
	shim_bt.accept_list_adds++;
	return 0;
}

bool shim_bt_deliver_adv(const bt_addr_le_t *addr, int8_t rssi,
			 const uint8_t *ad, uint16_t ad_len)
{
	uint8_t copy[64];
	struct net_buf_simple buf;

	if (!shim_bt.scan_cb || ad_len > sizeof(copy)) {
		return false;
	}
	memcpy(copy, ad, ad_len);
	buf.data = copy;
	buf.len = ad_len;
	buf.size = sizeof(copy);
	buf.__buf = copy;
	shim_bt.scan_cb(addr, rssi, 0, &buf);
	return true;
}

uint16_t shim_bt_build_adv(uint8_t *out, size_t out_size, const char *name,
			   const uint8_t *mfg, uint8_t mfg_len)
{
	size_t pos = 0;

	if (name) {
		size_t name_len = strlen(name);

		if (pos + 2 + name_len > out_size) {
			return 0;
		}
		out[pos++] = (uint8_t)(name_len + 1);
		out[pos++] = BT_DATA_NAME_COMPLETE;
		memcpy(&out[pos], name, name_len);
		pos += name_len;
	}
	if (mfg) {
		if (pos + 2 + mfg_len > out_size) {
			return 0;
		}
		out[pos++] = (uint8_t)(mfg_len + 1);
		out[pos++] = BT_DATA_MANUFACTURER_DATA;
		memcpy(&out[pos], mfg, mfg_len);
		pos += mfg_len;
	}
	return (uint16_t)pos;
}
