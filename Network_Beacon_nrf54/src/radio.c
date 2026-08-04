#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
//#include <bluetooth/scan.h>
#include "common_include.h"
#include "radio_ids.h"
#include "network.h"
#include "param_storage.h"
#include "radio.h"
#include "nus.h"
#include "device.h"
#include "led.h"
#include "storage_work_queue.h"
/* Radio Parameters
 *
 */
#define CONNECTABLE_ADV_INTERVAL_MIN \
	BT_GAP_MS_TO_ADV_INTERVAL(CONFIG_DSA_ADV_INTERVAL_MIN_MS)
#define CONNECTABLE_ADV_INTERVAL_MAX \
	BT_GAP_MS_TO_ADV_INTERVAL(CONFIG_DSA_ADV_INTERVAL_MAX_MS)
#define SCAN_WINDOW \
	BT_GAP_MS_TO_SCAN_WINDOW(CONFIG_DSA_SCAN_WINDOW_MS)
#define SCAN_INTERVAL \
	BT_GAP_MS_TO_SCAN_INTERVAL(CONFIG_DSA_SCAN_INTERVAL_MS)
#define CONNECTABLE_ADV_INTERVAL_MIN_LOW_ACTIVITY \
	BT_GAP_MS_TO_ADV_INTERVAL( \
		CONFIG_DSA_LOW_ACTIVITY_ADV_INTERVAL_MIN_MS)
#define CONNECTABLE_ADV_INTERVAL_MAX_LOW_ACTIVITY \
	BT_GAP_MS_TO_ADV_INTERVAL( \
		CONFIG_DSA_LOW_ACTIVITY_ADV_INTERVAL_MAX_MS)
#define SCAN_WINDOW_LOW_ACTIVITY \
	BT_GAP_MS_TO_SCAN_WINDOW(CONFIG_DSA_LOW_ACTIVITY_SCAN_WINDOW_MS)
#define SCAN_INTERVAL_LOW_ACTIVITY \
	BT_GAP_MS_TO_SCAN_INTERVAL(CONFIG_DSA_LOW_ACTIVITY_SCAN_INTERVAL_MS)
#define HIGH_ACTIVITY				1
#define LOW_ACTIVITY				0
#define COMMAND_TARGET_BROADCAST	0xff
#define COMMAND_DATA_MAX_LEN		31
#define COMMAND_QUEUE_DEPTH		4
#define RADIO_PARAMS_STORAGE_KEY	"dsa/radio"
#define RADIO_PARAMS_STORED_SIZE	17U

#define SCAN_INTERVAL_MIN_UNITS		0x0004
#define SCAN_INTERVAL_MAX_UNITS		0x4000



static uint8_t mfg_data[] = { 0xff, 0x00, 0x00 };

static const struct bt_data ad[] = {
	//BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, strlen(CONFIG_BT_DEVICE_NAME)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

struct radio_params {
	uint16_t	adv_interval_min;
	uint16_t	adv_interval_max;
	uint16_t	adv_interval_min_lowactivity;
	uint16_t	adv_interval_max_lowactivity;
	uint16_t	scan_interval;
	uint16_t	scan_interval_lowactivity;
	uint16_t	scan_window;
	uint16_t	scan_window_lowactivity;
	uint8_t		mode;
};

static struct radio_params params_radio;
static struct radio_params command_old_params_radio;
static bool command_batch_active;

static struct bt_le_scan_param scan_params;
static struct bt_le_adv_param adv_params;
static bool advertising_initialized;

enum target_action {
    ACTION_NONE = 0,
    ACTION_DSA,
    ACTION_DST,
    ACTION_DSZ,
};

struct target_device {
    const char *name;
    enum target_action action;
};

static const struct target_device target_devices[] = {
    { .name = "DSA",    .action = ACTION_DSA },
    { .name = "DSZ", 	.action = ACTION_DSZ },
    { .name = "DST", 	.action = ACTION_DST },
};
struct parsed_advertisement {
	enum target_action action;
	bool name_found;
	bool manufacturer_found;
	uint8_t manufacturer_len;
	uint8_t manufacturer_data[COMMAND_DATA_MAX_LEN];
};

struct command_msg {
	uint8_t len;
	uint8_t data[COMMAND_DATA_MAX_LEN];
};

static void command_work_handler(struct k_work *work);
static void radio_status_update_handler(struct k_work *work);
static void radio_status_set_local(uint8_t mask, bool active);
static void adv_prepare_status_data(void);
static int adv_update_locked(void);
static bool adv_interval_valid(uint16_t interval);
static bool scan_interval_valid(uint16_t interval);
static int adv_ms_to_units(uint16_t milliseconds, uint16_t *units);
static int scan_ms_to_units(uint16_t milliseconds, uint16_t *units);
static bool radio_params_equal(const struct radio_params *a,
			       const struct radio_params *b);

K_MSGQ_DEFINE(command_msgq, sizeof(struct command_msg), COMMAND_QUEUE_DEPTH, 1);
static K_WORK_DEFINE(command_work, command_work_handler);
static K_WORK_DEFINE(radio_status_update_work, radio_status_update_handler);
static K_MUTEX_DEFINE(radio_operation_lock);

void set_ble_params(struct radio_params *params);
static uint8_t update_ble_params(struct bt_le_scan_param *scan_params, struct bt_le_adv_param *adv_params);
static int radio_params_validate(const struct radio_params *params);
static int scan_init(void);



void set_radio_params_init(void)
{
	params_radio.adv_interval_min 			= CONNECTABLE_ADV_INTERVAL_MIN;
	params_radio.adv_interval_max 			= CONNECTABLE_ADV_INTERVAL_MAX;
	params_radio.adv_interval_min_lowactivity 	= CONNECTABLE_ADV_INTERVAL_MIN_LOW_ACTIVITY;
	params_radio.adv_interval_max_lowactivity 	= CONNECTABLE_ADV_INTERVAL_MAX_LOW_ACTIVITY;
	params_radio.scan_interval 			= (uint16_t)SCAN_INTERVAL;
	params_radio.scan_interval_lowactivity 	= (uint16_t)SCAN_INTERVAL_LOW_ACTIVITY;
	params_radio.scan_window 			= (uint16_t)SCAN_WINDOW;
	params_radio.scan_window_lowactivity 	= (uint16_t)SCAN_WINDOW_LOW_ACTIVITY;
	params_radio.mode =
		IS_ENABLED(CONFIG_DSA_RADIO_DEFAULT_HIGH_ACTIVITY) ?
			HIGH_ACTIVITY : LOW_ACTIVITY;
}

static bool parse_advertisement_cb(struct bt_data *data, void *user_data)
{
	struct parsed_advertisement *parsed = user_data;

	if (data->type == BT_DATA_NAME_COMPLETE ||
	    data->type == BT_DATA_NAME_SHORTENED) {
		for (size_t i = 0; i < ARRAY_SIZE(target_devices); i++) {
			const char *target_name = target_devices[i].name;

			if (data->data_len == strlen(target_name) &&
			    memcmp(data->data, target_name, data->data_len) == 0) {
				parsed->name_found = true;
				parsed->action = target_devices[i].action;
				break;
			}
		}
	} else if (data->type == BT_DATA_MANUFACTURER_DATA) {
		parsed->manufacturer_len =
			MIN(data->data_len,
			    (uint8_t)sizeof(parsed->manufacturer_data));
		memcpy(parsed->manufacturer_data, data->data,
		       parsed->manufacturer_len);
		parsed->manufacturer_found = parsed->manufacturer_len > 0;

		if (parsed->manufacturer_len < data->data_len) {
			printk("Manufacturer data truncated from %u to %u bytes\n",
			       data->data_len, parsed->manufacturer_len);
		}
	}

	return true;
}

void radio_command_begin(void)
{
	command_old_params_radio = params_radio;
	command_batch_active = true;
}

static void radio_apply_command(uint8_t parameter, uint16_t value)
{
	int err;
	uint16_t converted;

	switch (parameter) {
	case P_ADV_INTERVAL_MS:
		err = adv_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid advertising interval %u ms\n", value);
			return;
		}
		params_radio.adv_interval_min = converted;
		params_radio.adv_interval_max = converted;
		break;
	case P_ADV_INTERVAL_LOWACTIVITY_MS:
		err = adv_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid low-activity advertising interval %u ms\n",
			       value);
			return;
		}
		params_radio.adv_interval_min_lowactivity = converted;
		params_radio.adv_interval_max_lowactivity = converted;
		break;
	case P_SCAN_INTERVAL_MS:
		err = scan_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid scan interval %u ms\n", value);
			return;
		}
		params_radio.scan_interval = converted;
		break;
	case P_SCAN_INTERVAL_LOWACTIVITY_MS:
		err = scan_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid low-activity scan interval %u ms\n",
			       value);
			return;
		}
		params_radio.scan_interval_lowactivity = converted;
		break;
	case P_SCAN_WINDOW_MS:
		err = scan_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid scan window %u ms\n", value);
			return;
		}
		params_radio.scan_window = converted;
		break;
	case P_SCAN_WINDOW_LOWACTIVITY_MS:
		err = scan_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid low-activity scan window %u ms\n",
			       value);
			return;
		}
		params_radio.scan_window_lowactivity = converted;
		break;
	case P_RADIO_RESET_PARAMS:
		set_radio_params_init();
		break;
	case P_SET_RAD_ACTIVE:
		params_radio.mode = value ? HIGH_ACTIVITY : LOW_ACTIVITY;
		break;
	default:
		printk("Unknown radio parameter 0x%02x value %u\n", parameter, value);
		break;
	}
}

void radio_command_commit(void)
{
	uint8_t update_errors;
	int err;

	if (!command_batch_active) {
		return;
	}
	command_batch_active = false;

	if (!radio_params_equal(&command_old_params_radio, &params_radio)) {
		err = radio_params_validate(&params_radio);
		if (err) {
			printk("Rejecting invalid radio parameters (err %d)\n", err);
			params_radio = command_old_params_radio;
			return;
		}

		set_ble_params(&params_radio);
		update_errors = update_ble_params(&scan_params, &adv_params);
		if (update_errors & (BLE_UPDATE_ADV_ERROR | BLE_UPDATE_SCAN_ERROR)) {
			printk("Radio parameter update had error flags 0x%02x, restoring old parameters\n",
			       update_errors);

			params_radio = command_old_params_radio;
			set_ble_params(&params_radio);
			update_errors = update_ble_params(&scan_params, &adv_params);
			if (update_errors & BLE_UPDATE_ADV_ERROR) {
				printk("Failed to restore advertising parameters, not saving radio parameters\n");
			} else if (update_errors & BLE_UPDATE_SCAN_ERROR) {
				printk("Failed to restore scan parameters, not saving radio parameters\n");
			}
			return;
		}
		if (update_errors & BLE_UPDATE_STATUS_ERROR) {
			printk("Radio status advertising data update failed, keeping accepted parameters\n");
		}

		err = radio_params_save();
		if (err) {
			printk("Failed to save radio parameters (err %d)\n", err);
		}
	}
}

int radio_params_load(void)
{
	uint8_t stored[RADIO_PARAMS_STORED_SIZE];
	struct radio_params loaded;
	int err;

	err = param_storage_load(RADIO_PARAMS_STORAGE_KEY, stored, sizeof(stored));
	if (!err) {
		loaded.adv_interval_min = sys_get_be16(&stored[0]);
		loaded.adv_interval_max = sys_get_be16(&stored[2]);
		loaded.adv_interval_min_lowactivity = sys_get_be16(&stored[4]);
		loaded.adv_interval_max_lowactivity = sys_get_be16(&stored[6]);
		loaded.scan_interval = sys_get_be16(&stored[8]);
		loaded.scan_interval_lowactivity = sys_get_be16(&stored[10]);
		loaded.scan_window = sys_get_be16(&stored[12]);
		loaded.scan_window_lowactivity = sys_get_be16(&stored[14]);
		loaded.mode = stored[16];
		if (radio_params_validate(&loaded)) {
			device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, true);
			return -EBADMSG;
		}
		params_radio = loaded;
		device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, false);
		return 0;
	}
	if (err == -ENOENT) {
		device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, false);
		return err;
	}

	err = param_storage_load_legacy(RADIO_PARAMS_STORAGE_KEY,
					&loaded, sizeof(loaded));
	if (err || radio_params_validate(&loaded)) {
		device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, true);
		return err ? err : -EBADMSG;
	}

	params_radio = loaded;
	err = radio_params_save();
	if (!err) {
		printk("Migrated radio parameters to versioned storage\n");
	}
	device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, err != 0);
	return err;
}

int radio_params_save(void)
{
	uint8_t stored[RADIO_PARAMS_STORED_SIZE];

	sys_put_be16(params_radio.adv_interval_min, &stored[0]);
	sys_put_be16(params_radio.adv_interval_max, &stored[2]);
	sys_put_be16(params_radio.adv_interval_min_lowactivity, &stored[4]);
	sys_put_be16(params_radio.adv_interval_max_lowactivity, &stored[6]);
	sys_put_be16(params_radio.scan_interval, &stored[8]);
	sys_put_be16(params_radio.scan_interval_lowactivity, &stored[10]);
	sys_put_be16(params_radio.scan_window, &stored[12]);
	sys_put_be16(params_radio.scan_window_lowactivity, &stored[14]);
	stored[16] = params_radio.mode;

	int err = param_storage_save(RADIO_PARAMS_STORAGE_KEY, stored,
				    sizeof(stored));

	device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, err != 0);
	return err;
}

static void evaluate_command_data(const uint8_t *data, uint8_t len)
{
	if ((len % 3U) != 0U) {
		printk("Rejecting malformed command payload length %u\n", len);
		return;
	}

	led_command_begin();
	network_command_begin();
	radio_command_begin();

	for (uint8_t offset = 0; offset + 2 < len; offset += 3) {
		uint8_t parameter = data[offset];
		uint16_t value = sys_get_be16(&data[offset + 1]);

		printk("Command parameter 0x%02x value %u\n", parameter, value);

		switch (parameter & P_BASE_MASK) {
		case P_BASE_MAIN:
			led_apply_command(parameter, value);
			break;
		case P_BASE_NETWORK:
			network_apply_command(parameter, value);
			break;
		case P_BASE_RADIO:
			radio_apply_command(parameter, value);
			break;
		default:
			printk("Unknown parameter base 0x%02x for parameter 0x%02x\n",
			       parameter & P_BASE_MASK, parameter);
			break;
		}
	}

	led_command_commit();
	network_command_commit();
	radio_command_commit();

}

static void radio_evaluate_command_data(const uint8_t *data, uint8_t len)
{
	uint8_t target;

	if (len < 1) {
		printk("Command data missing target byte\n");
		return;
	}

	target = data[0];
	if (target != mfg_data[ADV_POS_ID] && target != COMMAND_TARGET_BROADCAST) {
		printk("Ignoring command for target 0x%02x, own id 0x%02x\n",
		       target, mfg_data[ADV_POS_ID]);
		return;
	}

	evaluate_command_data(&data[1], len - 1);
}

static void command_work_handler(struct k_work *work)
{
	struct command_msg msg;

	ARG_UNUSED(work);

	while (k_msgq_get(&command_msgq, &msg, K_NO_WAIT) == 0) {
		radio_evaluate_command_data(msg.data, msg.len);
	}
}

static void enqueue_command(const struct parsed_advertisement *parsed)
{
	int err;
	struct command_msg msg = { 0 };

	if (!parsed->manufacturer_found) {
		printk("Command advertisement has no manufacturer data\n");
		return;
	}

	msg.len = parsed->manufacturer_len;
	memcpy(msg.data, parsed->manufacturer_data, msg.len);

	err = k_msgq_put(&command_msgq, &msg, K_NO_WAIT);
	if (err) {
		printk("Command queue full, dropping command (err %d)\n", err);
		return;
	}

	storage_work_submit(&command_work);
}

static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	struct net_buf_simple ad_temp;
	struct parsed_advertisement parsed = {
		.action = ACTION_NONE,
	};

	net_buf_simple_clone(buf, &ad_temp);
	bt_data_parse(&ad_temp, parse_advertisement_cb, &parsed);

	if (parsed.name_found) {
		printk("Found target device DSA\n");
		switch (parsed.action) {
		case ACTION_DSA:
		case ACTION_DST:
			if (parsed.manufacturer_found &&
			    parsed.manufacturer_len >= 1U) {
				network_evaluate_contact(
					parsed.manufacturer_data[0], rssi);
			} else {
				printk("Contact advertisement has no device id\n");
			}
			break;

		case ACTION_DSZ:
			enqueue_command(&parsed);
			break;
		default:
			break;
		}	
	}
	ARG_UNUSED(addr);
	ARG_UNUSED(adv_type);
}

static void radio_status_update_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (adv_update()) {
		printk("Failed to advertise updated storage status\n");
	}
}

void radio_schedule_status_update(void)
{
	if (advertising_initialized) {
		k_work_submit(&radio_status_update_work);
	}
}

static int radio_params_validate(const struct radio_params *params)
{
	if (params->mode != HIGH_ACTIVITY && params->mode != LOW_ACTIVITY) {
		return -EINVAL;
	}

	if (params->adv_interval_min == 0 ||
	    params->adv_interval_max == 0 ||
	    params->adv_interval_min_lowactivity == 0 ||
	    params->adv_interval_max_lowactivity == 0 ||
	    params->scan_interval == 0 ||
	    params->scan_interval_lowactivity == 0 ||
	    params->scan_window == 0 ||
	    params->scan_window_lowactivity == 0) {
		return -EINVAL;
	}

	if (params->adv_interval_min > params->adv_interval_max ||
	    params->adv_interval_min_lowactivity > params->adv_interval_max_lowactivity) {
		return -EINVAL;
	}

	if (!adv_interval_valid(params->adv_interval_min) ||
	    !adv_interval_valid(params->adv_interval_max) ||
	    !adv_interval_valid(params->adv_interval_min_lowactivity) ||
	    !adv_interval_valid(params->adv_interval_max_lowactivity)) {
		return -EINVAL;
	}

	if (!scan_interval_valid(params->scan_interval) ||
	    !scan_interval_valid(params->scan_interval_lowactivity) ||
	    !scan_interval_valid(params->scan_window) ||
	    !scan_interval_valid(params->scan_window_lowactivity)) {
		return -EINVAL;
	}

	if (params->scan_window > params->scan_interval ||
	    params->scan_window_lowactivity > params->scan_interval_lowactivity) {
		return -EINVAL;
	}

	return 0;
}

static bool radio_params_equal(const struct radio_params *a,
			       const struct radio_params *b)
{
	return a->adv_interval_min == b->adv_interval_min &&
	       a->adv_interval_max == b->adv_interval_max &&
	       a->adv_interval_min_lowactivity ==
		       b->adv_interval_min_lowactivity &&
	       a->adv_interval_max_lowactivity ==
		       b->adv_interval_max_lowactivity &&
	       a->scan_interval == b->scan_interval &&
	       a->scan_interval_lowactivity == b->scan_interval_lowactivity &&
	       a->scan_window == b->scan_window &&
	       a->scan_window_lowactivity == b->scan_window_lowactivity &&
	       a->mode == b->mode;
}

static bool adv_interval_valid(uint16_t interval)
{
	return interval >= BT_LE_ADV_INTERVAL_MIN &&
	       interval <= BT_LE_ADV_INTERVAL_MAX;
}

static bool scan_interval_valid(uint16_t interval)
{
	return interval >= SCAN_INTERVAL_MIN_UNITS &&
	       interval <= SCAN_INTERVAL_MAX_UNITS;
}

static int adv_ms_to_units(uint16_t milliseconds, uint16_t *units)
{
	uint32_t converted;

	if (!units) {
		return -EINVAL;
	}

	converted = ((uint32_t)milliseconds * 8U) / 5U;
	if (converted > UINT16_MAX ||
	    !adv_interval_valid((uint16_t)converted)) {
		return -EINVAL;
	}

	*units = (uint16_t)converted;
	return 0;
}

static int scan_ms_to_units(uint16_t milliseconds, uint16_t *units)
{
	uint32_t converted;

	if (!units) {
		return -EINVAL;
	}

	converted = ((uint32_t)milliseconds * 8U) / 5U;
	if (converted > UINT16_MAX ||
	    !scan_interval_valid((uint16_t)converted)) {
		return -EINVAL;
	}

	*units = (uint16_t)converted;
	return 0;
}

/* Advertising is required. Scan failures are degraded mode and are exposed
 * through the radio status byte.
 */
static uint8_t update_ble_params(struct bt_le_scan_param *parameters_scan, struct bt_le_adv_param *parameters_adv)
{
	int err;
	uint8_t errors = 0;
	bool status_changed = false;

	k_mutex_lock(&radio_operation_lock, K_FOREVER);

	err = bt_le_adv_stop();
	if (err && err != -EALREADY) {
		printk("Advertising stop before parameter update failed (err %d)\n", err);
		errors |= BLE_UPDATE_ADV_ERROR;
	}

	err = bt_le_scan_stop();
	if (err && err != -EALREADY) {
		printk("Scan stop before parameter update failed (err %d)\n", err);
		radio_status_set_local(RADIO_STATUS_SCAN_RUNTIME_ERROR, true);
		status_changed = true;
		errors |= BLE_UPDATE_SCAN_ERROR;
	}

	err = bt_le_adv_start(parameters_adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err && err != -EALREADY) {
		printk("Advertising parameter update failed (err %d)\n", err);
		errors |= BLE_UPDATE_ADV_ERROR;
	}

	err = bt_le_scan_start(parameters_scan, scan_cb);
	if (err && err != -EALREADY) {
		printk("Scan parameter update failed (err %d)\n", err);
		radio_status_set_local(RADIO_STATUS_SCAN_RUNTIME_ERROR, true);
		status_changed = true;
		errors |= BLE_UPDATE_SCAN_ERROR;
	} else {
		radio_status_set_local(RADIO_STATUS_SCAN_RUNTIME_ERROR, false);
		status_changed = true;
	}

	if (status_changed && !(errors & BLE_UPDATE_ADV_ERROR)) {
		err = adv_update_locked();
		if (err) {
			errors |= BLE_UPDATE_STATUS_ERROR;
		}
	}

	k_mutex_unlock(&radio_operation_lock);
	return errors;
}

static int scan_init(void)
{
	int err;
	size_t failed_entries = 0;

	for (size_t i = 0; i < known_device_table_len; i++) {
		err = bt_le_filter_accept_list_add(&known_device_table[i].addr);
		if (err) {
			printk("Failed to add device %u to filter list (err %d)\n", (unsigned int)i, err);
			failed_entries++;
		}
	}

	if (failed_entries == known_device_table_len) {
		return -ENODEV;
	}

	return 0;
}

void adv_init(void)
{
	adv_params.id = 0U;
	adv_params.sid = 0U;
	adv_params.secondary_max_skip = 0U;
	adv_params.options = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY;
	mfg_data[ADV_POS_ID] = get_device_id();
	adv_prepare_status_data();
	advertising_initialized = true;
}

static int adv_update_locked(void)
{
	int err;
	uint8_t old_mfg_data[sizeof(mfg_data)];

	memcpy(old_mfg_data, mfg_data, sizeof(mfg_data));

	adv_prepare_status_data();

	if (nus_is_connected()) {
		return 0;
	}

	err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		memcpy(mfg_data, old_mfg_data, sizeof(mfg_data));
		printk("Advertising data update failed (err %d)\n", err);
		return err;
	}

	return 0;
}

int adv_update(void)
{
	int err;

	k_mutex_lock(&radio_operation_lock, K_FOREVER);
	err = adv_update_locked();
	k_mutex_unlock(&radio_operation_lock);
	return err;
}

static void radio_status_set_local(uint8_t mask, bool active)
{
	device_set_radio_status_bit(mask, active);
}

static void adv_prepare_status_data(void)
{
	mfg_data[ADV_POS_RADIO_STATUS] = device_get_radio_status();
	mfg_data[ADV_POS_NETWORK_STATUS] = device_get_network_status();
}

void set_ble_params(struct radio_params *params)
{
	scan_params.type = BT_LE_SCAN_TYPE_PASSIVE;
	scan_params.options = BT_LE_SCAN_OPT_FILTER_ACCEPT_LIST;
	switch(params->mode)
			{
				case LOW_ACTIVITY:
				{
				    adv_params.interval_min   = params->adv_interval_min_lowactivity;
					adv_params.interval_max   = params->adv_interval_max_lowactivity;
				    scan_params.interval = params->scan_interval_lowactivity;
				    scan_params.window = params->scan_window_lowactivity;
					break;
				}
				case HIGH_ACTIVITY:
				{
					adv_params.interval_min   = params->adv_interval_min;
					adv_params.interval_max   = params->adv_interval_max;
					scan_params.interval = params->scan_interval;
					scan_params.window = params->scan_window;
				    break;
				}
			}
}

int radio_init(void)
{
  	int err;
	int load_err;

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return err;
	}

	printk("Bluetooth initialized\n");

	err = nus_service_init();
	if (err) {
		printk("NUS init failed (err %d), continuing without NUS\n", err);
		radio_status_set_local(RADIO_STATUS_NUS_ERROR, true);
	} else {
		radio_status_set_local(RADIO_STATUS_NUS_ERROR, false);
		printk("NUS initialized\n");
	}
	set_radio_params_init();
	load_err = radio_params_load();
	if (load_err == -ENOENT) {
		printk("No stored radio parameters, using defaults\n");
	} else if (load_err) {
		printk("Failed to load radio parameters (err %d), using defaults\n", load_err);
	} else {
		err = radio_params_validate(&params_radio);
		if (err) {
			printk("Stored radio parameters invalid (err %d), using defaults\n", err);
			set_radio_params_init();
		}
	}
	err = scan_init();
	if (err) {
		radio_status_set_local(RADIO_STATUS_SCAN_CONFIG_ERROR, true);
	} else {
		radio_status_set_local(RADIO_STATUS_SCAN_CONFIG_ERROR, false);
	}
	adv_init();
	return 0;
}

int radio_start(void)
{
    int err;

	k_mutex_lock(&radio_operation_lock, K_FOREVER);
	set_ble_params(&params_radio);
	/* Start advertising */
	err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad),
				      NULL, 0);
	if (err) {
		printk("Advertising failed to start (err %d)\n", err);
		k_mutex_unlock(&radio_operation_lock);
		return err;
	}

	
	err = bt_le_scan_start(&scan_params, scan_cb);
	if (err) {
		printk("Starting scanning failed (err %d), entering advertising-only degraded mode\n", err);
		radio_status_set_local(RADIO_STATUS_SCAN_RUNTIME_ERROR, true);
		(void)adv_update_locked();
		k_mutex_unlock(&radio_operation_lock);
		return 0;  // Scan failure is tolerated; advertising-only operation is intentional.
	}

	radio_status_set_local(RADIO_STATUS_SCAN_RUNTIME_ERROR, false);
	(void)adv_update_locked();

	k_mutex_unlock(&radio_operation_lock);
    return 0;
}


static void radio_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	printk("Connection disconnected, advertising restart waits for recycled callback (reason 0x%02x)\n",
	       reason);
}

static void radio_recycled(void)
{
	int err;

	printk("Connection object recycled, restarting advertising\n");
	k_mutex_lock(&radio_operation_lock, K_FOREVER);
	err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad), NULL, 0);
	k_mutex_unlock(&radio_operation_lock);
	if (err) {
		printk("Advertising failed to restart (err %d)\n", err);
	} else {
		printk("Advertising restarted\n");
	}
}

BT_CONN_CB_DEFINE(radio_conn_callbacks) = {
	.disconnected = radio_disconnected,
	.recycled = radio_recycled,
};
