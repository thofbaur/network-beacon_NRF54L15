#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include "common_include.h"
#include "radio_ids.h"
#include "network.h"
#include "param_storage.h"
#include "radio.h"
#include "nus.h"
#include "device.h"
#include "eco_log.h"
#include "led.h"
#include "motion.h"
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
#define CONNECTABLE_ADV_INTERVAL_MIN_ECO \
	BT_GAP_MS_TO_ADV_INTERVAL( \
		CONFIG_DSA_ECO_ADV_INTERVAL_MIN_MS)
#define CONNECTABLE_ADV_INTERVAL_MAX_ECO \
	BT_GAP_MS_TO_ADV_INTERVAL( \
		CONFIG_DSA_ECO_ADV_INTERVAL_MAX_MS)
#define ECO_SCAN_WINDOW \
	BT_GAP_MS_TO_SCAN_WINDOW(CONFIG_DSA_ECO_SCAN_WINDOW_MS)
#define HIGH_ACTIVITY				1
#define ECO_ACTIVITY				0
#define COMMAND_TARGET_BROADCAST	0xff
#define COMMAND_DATA_MAX_LEN		31
#define COMMAND_QUEUE_DEPTH		4
/* Renamed from "dsa/radio": low-activity mode was replaced by eco mode and
 * the stored field layout's meaning changed (see DECISIONS.md), so the old
 * key is deliberately abandoned rather than reinterpreted.
 */
#define RADIO_PARAMS_STORAGE_KEY	"dsa/radio2"
#define RADIO_PARAMS_STORED_SIZE	17U

#define SCAN_INTERVAL_MIN_UNITS		0x0004
#define SCAN_INTERVAL_MAX_UNITS		0x4000



static uint8_t mfg_data[] = { 0xff, 0x00, 0x00 };

static const struct bt_data ad[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, strlen(CONFIG_BT_DEVICE_NAME)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

struct radio_params {
	uint16_t	adv_interval_min;
	uint16_t	adv_interval_max;
	uint16_t	adv_interval_min_eco;
	uint16_t	adv_interval_max_eco;
	uint16_t	scan_interval;
	uint16_t	scan_window;
	/* Burst duration (BLE window units) for eco-mode scanning. */
	uint16_t	eco_scan_window;
	/* Burst period in seconds, not BLE units: exceeds the ~10.24 s max
	 * of the native scan-interval field, so eco scanning is periodic
	 * application-triggered bursts rather than a single scan parameter.
	 */
	uint16_t	eco_scan_period_s;
	uint8_t		mode;
};

static struct radio_params params_radio;
static struct radio_params command_old_params_radio;
static bool command_batch_active;
static bool eco_override_active;
static bool eco_scan_burst_active;
/* Tracks the mode radio_apply_mode_locked() last actually applied, so eco
 * session logging (eco_log_enter/leave) fires only on real transitions,
 * regardless of which of the two triggers (manual command or motion
 * override) caused it, and regardless of how many times the function is
 * called for unrelated parameter changes while the mode itself is unchanged.
 */
static uint8_t last_applied_mode = HIGH_ACTIVITY;

static struct bt_le_scan_param scan_params;
static struct bt_le_adv_param adv_params;
static bool advertising_initialized;
/* Independently tracked so a runtime scan start/stop failure and a boot-time
 * accept-list configuration failure don't clobber each other's state when
 * both fold into the single advertised RADIO_STATUS_SCAN_ERROR bit.
 */
static bool scan_runtime_fault;
static bool scan_config_fault;

enum target_action {
    ACTION_NONE = 0,
    ACTION_DSA,
    ACTION_DST,
    ACTION_DSZ,
    ACTION_DSL,
};

struct target_device {
    const char *name;
    enum target_action action;
};

static const struct target_device target_devices[] = {
    { .name = "DSA",    .action = ACTION_DSA },
    { .name = "DSZ", 	.action = ACTION_DSZ },
    { .name = "DST", 	.action = ACTION_DST },
    { .name = "DSL", 	.action = ACTION_DSL },
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
static void eco_scan_handler(struct k_work *work);
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf);
static void radio_status_set_local(uint8_t mask, bool active);
static void radio_update_scan_status(void);
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
static K_WORK_DELAYABLE_DEFINE(eco_scan_work, eco_scan_handler);
static K_MUTEX_DEFINE(radio_operation_lock);

static uint8_t radio_apply_mode_locked(uint8_t mode);
static int radio_params_validate(const struct radio_params *params);
static void scan_init(void);

BUILD_ASSERT(CONFIG_DSA_ECO_SCAN_INTERVAL_MS % 1000 == 0,
	     "Eco scan burst period must use whole seconds");

void set_radio_params_init(void)
{
	params_radio.adv_interval_min 		= CONNECTABLE_ADV_INTERVAL_MIN;
	params_radio.adv_interval_max 		= CONNECTABLE_ADV_INTERVAL_MAX;
	params_radio.adv_interval_min_eco 	= CONNECTABLE_ADV_INTERVAL_MIN_ECO;
	params_radio.adv_interval_max_eco 	= CONNECTABLE_ADV_INTERVAL_MAX_ECO;
	params_radio.scan_interval 		= (uint16_t)SCAN_INTERVAL;
	params_radio.scan_window 		= (uint16_t)SCAN_WINDOW;
	params_radio.eco_scan_window 		= (uint16_t)ECO_SCAN_WINDOW;
	params_radio.eco_scan_period_s 	= CONFIG_DSA_ECO_SCAN_INTERVAL_MS / 1000;
	params_radio.mode =
		IS_ENABLED(CONFIG_DSA_RADIO_DEFAULT_HIGH_ACTIVITY) ?
			HIGH_ACTIVITY : ECO_ACTIVITY;
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
	case P_ADV_INTERVAL_ECO_MS:
		err = adv_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid eco advertising interval %u ms\n",
			       value);
			return;
		}
		params_radio.adv_interval_min_eco = converted;
		params_radio.adv_interval_max_eco = converted;
		break;
	case P_SCAN_INTERVAL_MS:
		err = scan_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid scan interval %u ms\n", value);
			return;
		}
		params_radio.scan_interval = converted;
		break;
	case P_ECO_SCAN_PERIOD_S:
		if (value == 0U) {
			printk("Rejecting zero eco scan burst period\n");
			return;
		}
		params_radio.eco_scan_period_s = value;
		break;
	case P_SCAN_WINDOW_MS:
		err = scan_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid scan window %u ms\n", value);
			return;
		}
		params_radio.scan_window = converted;
		break;
	case P_ECO_SCAN_WINDOW_MS:
		err = scan_ms_to_units(value, &converted);
		if (err) {
			printk("Rejecting invalid eco scan burst window %u ms\n",
			       value);
			return;
		}
		params_radio.eco_scan_window = converted;
		break;
	case P_RADIO_RESET_PARAMS:
		set_radio_params_init();
		break;
	case P_SET_RAD_ACTIVE:
		params_radio.mode = value ? HIGH_ACTIVITY : ECO_ACTIVITY;
		break;
	default:
		printk("Unknown radio parameter 0x%02x value %u\n", parameter, value);
		break;
	}
}

/* Applies advertising + scanning for the given mode (HIGH_ACTIVITY or
 * ECO_ACTIVITY) and returns BLE_UPDATE_*_ERROR bits. Caller must hold
 * radio_operation_lock. Used by both the manually-persisted radio mode
 * (P_SET_RAD_ACTIVE) and motion.c's temporary, non-persisted override, so
 * the two never diverge in how "eco" is actually implemented.
 *
 * Advertising keeps a normal, continuous parameter set in both modes.
 * Scanning differs: HIGH_ACTIVITY scans continuously; ECO_ACTIVITY scans in
 * short periodic bursts (eco_scan_handler), since CONFIG_DSA_ECO_SCAN_INTERVAL_MS
 * exceeds what the BLE scan-interval field can express (max ~10.24 s).
 */
static uint8_t radio_apply_mode_locked(uint8_t mode)
{
	int err;
	uint8_t errors = 0;

	if (mode != last_applied_mode) {
		if (mode == ECO_ACTIVITY) {
			eco_log_enter();
		} else {
			eco_log_leave();
		}
		last_applied_mode = mode;
	}

	k_work_cancel_delayable(&eco_scan_work);
	err = bt_le_scan_stop();
	if (err && err != -EALREADY) {
		printk("Scan stop before mode change failed (err %d)\n", err);
		scan_runtime_fault = true;
		radio_update_scan_status();
		errors |= BLE_UPDATE_SCAN_ERROR;
	}
	eco_scan_burst_active = false;

	err = bt_le_adv_stop();
	if (err && err != -EALREADY) {
		printk("Advertising stop before mode change failed (err %d)\n", err);
		errors |= BLE_UPDATE_ADV_ERROR;
	}

	if (mode == ECO_ACTIVITY) {
		adv_params.interval_min = params_radio.adv_interval_min_eco;
		adv_params.interval_max = params_radio.adv_interval_max_eco;
	} else {
		adv_params.interval_min = params_radio.adv_interval_min;
		adv_params.interval_max = params_radio.adv_interval_max;
	}

	err = bt_le_adv_start(&adv_params, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err && err != -EALREADY) {
		printk("Advertising start failed (err %d)\n", err);
		errors |= BLE_UPDATE_ADV_ERROR;
	}

	scan_params.type = BT_LE_SCAN_TYPE_PASSIVE;
	scan_params.options = BT_LE_SCAN_OPT_FILTER_ACCEPT_LIST;

	if (mode == ECO_ACTIVITY) {
		scan_params.interval = params_radio.eco_scan_window;
		scan_params.window = params_radio.eco_scan_window;
		scan_runtime_fault = false;
		radio_update_scan_status();
		k_work_reschedule(&eco_scan_work, K_NO_WAIT);
	} else {
		scan_params.interval = params_radio.scan_interval;
		scan_params.window = params_radio.scan_window;
		err = bt_le_scan_start(&scan_params, scan_cb);
		if (err && err != -EALREADY) {
			printk("Scan start failed (err %d)\n", err);
			scan_runtime_fault = true;
			errors |= BLE_UPDATE_SCAN_ERROR;
		} else {
			scan_runtime_fault = false;
		}
		radio_update_scan_status();
	}

	if (!(errors & BLE_UPDATE_ADV_ERROR)) {
		err = adv_update_locked();
		if (err) {
			errors |= BLE_UPDATE_STATUS_ERROR;
		}
	}

	return errors;
}

void radio_command_commit(void)
{
	uint8_t update_errors;
	int err;
	uint8_t effective_mode;

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

		k_mutex_lock(&radio_operation_lock, K_FOREVER);
		effective_mode = eco_override_active ? ECO_ACTIVITY : params_radio.mode;
		update_errors = radio_apply_mode_locked(effective_mode);
		k_mutex_unlock(&radio_operation_lock);

		if (update_errors & (BLE_UPDATE_ADV_ERROR | BLE_UPDATE_SCAN_ERROR)) {
			printk("Radio parameter update had error flags 0x%02x, restoring old parameters\n",
			       update_errors);

			params_radio = command_old_params_radio;
			k_mutex_lock(&radio_operation_lock, K_FOREVER);
			effective_mode = eco_override_active ? ECO_ACTIVITY : params_radio.mode;
			update_errors = radio_apply_mode_locked(effective_mode);
			k_mutex_unlock(&radio_operation_lock);
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

/* Scanning while inactive is duty-cycled in software: eco_scan_period_s
 * (5 minutes by default) is far beyond what the BLE scan-interval field can
 * express (max ~10.24 s), so this alternates short scan bursts with the
 * radio off in between rather than setting a single scan parameter.
 */
static void eco_scan_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	k_mutex_lock(&radio_operation_lock, K_FOREVER);
	if (!eco_override_active) {
		k_mutex_unlock(&radio_operation_lock);
		return;
	}

	if (eco_scan_burst_active) {
		err = bt_le_scan_stop();
		if (err && err != -EALREADY) {
			printk("Energy-conservation scan burst stop failed (err %d)\n", err);
		}
		eco_scan_burst_active = false;
		k_work_reschedule(&eco_scan_work,
				  K_SECONDS(params_radio.eco_scan_period_s));
	} else {
		err = bt_le_scan_start(&scan_params, scan_cb);
		if (err && err != -EALREADY) {
			printk("Energy-conservation scan burst start failed (err %d)\n", err);
		}
		eco_scan_burst_active = true;
		/* Convert the BLE-unit burst window back to milliseconds
		 * (inverse of scan_ms_to_units's ms*8/5) for the wall-clock
		 * delay before ending the burst.
		 */
		k_work_reschedule(&eco_scan_work,
				  K_MSEC(((uint32_t)params_radio.eco_scan_window * 5U) / 8U));
	}
	k_mutex_unlock(&radio_operation_lock);
}

void radio_set_eco_override(bool active)
{
	uint8_t update_errors;

	k_mutex_lock(&radio_operation_lock, K_FOREVER);

	if (eco_override_active == active) {
		k_mutex_unlock(&radio_operation_lock);
		return;
	}
	eco_override_active = active;

	update_errors = radio_apply_mode_locked(active ? ECO_ACTIVITY : params_radio.mode);
	if (update_errors & (BLE_UPDATE_ADV_ERROR | BLE_UPDATE_SCAN_ERROR)) {
		printk("Energy-conservation radio update had error flags 0x%02x\n",
		       update_errors);
	}

	k_mutex_unlock(&radio_operation_lock);
}

int radio_params_load(void)
{
	uint8_t stored[RADIO_PARAMS_STORED_SIZE];
	struct radio_params loaded;
	int err;

	err = param_storage_load(RADIO_PARAMS_STORAGE_KEY, stored, sizeof(stored));
	if (err == -ENOENT) {
		device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, false);
		return err;
	}
	if (err) {
		device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, true);
		return err;
	}

	loaded.adv_interval_min = sys_get_be16(&stored[0]);
	loaded.adv_interval_max = sys_get_be16(&stored[2]);
	loaded.adv_interval_min_eco = sys_get_be16(&stored[4]);
	loaded.adv_interval_max_eco = sys_get_be16(&stored[6]);
	loaded.scan_interval = sys_get_be16(&stored[8]);
	loaded.scan_window = sys_get_be16(&stored[10]);
	loaded.eco_scan_window = sys_get_be16(&stored[12]);
	loaded.eco_scan_period_s = sys_get_be16(&stored[14]);
	loaded.mode = stored[16];
	if (radio_params_validate(&loaded)) {
		device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, true);
		return -EBADMSG;
	}
	params_radio = loaded;
	device_set_storage_fault(STORAGE_FAULT_RADIO_PARAMS, false);
	return 0;
}

int radio_params_save(void)
{
	uint8_t stored[RADIO_PARAMS_STORED_SIZE];

	sys_put_be16(params_radio.adv_interval_min, &stored[0]);
	sys_put_be16(params_radio.adv_interval_max, &stored[2]);
	sys_put_be16(params_radio.adv_interval_min_eco, &stored[4]);
	sys_put_be16(params_radio.adv_interval_max_eco, &stored[6]);
	sys_put_be16(params_radio.scan_interval, &stored[8]);
	sys_put_be16(params_radio.scan_window, &stored[10]);
	sys_put_be16(params_radio.eco_scan_window, &stored[12]);
	sys_put_be16(params_radio.eco_scan_period_s, &stored[14]);
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
	motion_command_begin();

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
		case P_BASE_MOTION:
			motion_apply_command(parameter, value);
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
	motion_command_commit();

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
		case ACTION_DSL:
			if (parsed.manufacturer_found &&
			    parsed.manufacturer_len >= 1U) {
				network_evaluate_contact(
					parsed.manufacturer_data[0], rssi,
					parsed.action == ACTION_DSL);
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
	if (params->mode != HIGH_ACTIVITY && params->mode != ECO_ACTIVITY) {
		return -EINVAL;
	}

	if (params->adv_interval_min == 0 ||
	    params->adv_interval_max == 0 ||
	    params->adv_interval_min_eco == 0 ||
	    params->adv_interval_max_eco == 0 ||
	    params->scan_interval == 0 ||
	    params->scan_window == 0 ||
	    params->eco_scan_window == 0 ||
	    params->eco_scan_period_s == 0) {
		return -EINVAL;
	}

	if (params->adv_interval_min > params->adv_interval_max ||
	    params->adv_interval_min_eco > params->adv_interval_max_eco) {
		return -EINVAL;
	}

	if (!adv_interval_valid(params->adv_interval_min) ||
	    !adv_interval_valid(params->adv_interval_max) ||
	    !adv_interval_valid(params->adv_interval_min_eco) ||
	    !adv_interval_valid(params->adv_interval_max_eco)) {
		return -EINVAL;
	}

	if (!scan_interval_valid(params->scan_interval) ||
	    !scan_interval_valid(params->scan_window) ||
	    !scan_interval_valid(params->eco_scan_window)) {
		return -EINVAL;
	}

	if (params->scan_window > params->scan_interval) {
		return -EINVAL;
	}

	return 0;
}

static bool radio_params_equal(const struct radio_params *a,
			       const struct radio_params *b)
{
	return a->adv_interval_min == b->adv_interval_min &&
	       a->adv_interval_max == b->adv_interval_max &&
	       a->adv_interval_min_eco == b->adv_interval_min_eco &&
	       a->adv_interval_max_eco == b->adv_interval_max_eco &&
	       a->scan_interval == b->scan_interval &&
	       a->scan_window == b->scan_window &&
	       a->eco_scan_window == b->eco_scan_window &&
	       a->eco_scan_period_s == b->eco_scan_period_s &&
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

/* Adds every known device to the filter accept list. A failure on any single
 * entry (e.g. the list is full) is reported via RADIO_STATUS_SCAN_ERROR but
 * does not stop the loop or fail the caller: the remaining entries are still
 * worth adding, and scanning proceeds in degraded mode with whichever
 * devices made it in.
 */
static void scan_init(void)
{
	int err;

	scan_config_fault = false;
	for (size_t i = 0; i < known_device_table_len; i++) {
		err = bt_le_filter_accept_list_add(&known_device_table[i].addr);
		if (err) {
			printk("Failed to add device %u to filter list (err %d)\n", (unsigned int)i, err);
			scan_config_fault = true;
		}
	}

	radio_update_scan_status();
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

static void radio_update_scan_status(void)
{
	radio_status_set_local(RADIO_STATUS_SCAN_ERROR,
			       scan_runtime_fault || scan_config_fault);
}

static void adv_prepare_status_data(void)
{
	mfg_data[ADV_POS_RADIO_STATUS] = device_get_radio_status();
	mfg_data[ADV_POS_NETWORK_STATUS] = device_get_network_status();
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
	scan_init();
	adv_init();
	return 0;
}

/* Advertising is required; scan failures are degraded mode, tolerated and
 * exposed through the radio status byte.
 */
int radio_start(void)
{
	uint8_t update_errors;

	k_mutex_lock(&radio_operation_lock, K_FOREVER);
	update_errors = radio_apply_mode_locked(params_radio.mode);
	k_mutex_unlock(&radio_operation_lock);

	if (update_errors & BLE_UPDATE_ADV_ERROR) {
		printk("Advertising failed to start\n");
		return -EIO;
	}
	if (update_errors & BLE_UPDATE_SCAN_ERROR) {
		printk("Starting scanning failed, entering advertising-only degraded mode\n");
	}
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
