#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#include "common_include.h"
#include "device.h"
#include "led.h"
#include "motion.h"
#include "param_storage.h"
#include "radio.h"

#define MOTION_PARAMS_STORAGE_KEY "dsa/motion"
#define MOTION_PARAMS_STORED_SIZE 3U

#if DT_HAS_ALIAS(motion_detector)
#define MOTION_SENSOR_NODE DT_ALIAS(motion_detector)
#define MOTION_SENSOR_PRESENT 1
#else
#define MOTION_SENSOR_PRESENT 0
#endif

struct motion_state_params {
	bool active;
	uint16_t timeout_s;
};

static struct motion_state_params params_motion;
static struct motion_state_params command_old_params_motion;
static bool command_batch_active;

#if MOTION_SENSOR_PRESENT
static const struct device *const motion_sensor = DEVICE_DT_GET(MOTION_SENSOR_NODE);
#endif
static bool motion_available;
static bool eco_active;
static K_MUTEX_DEFINE(motion_lock);

static void inactivity_timeout_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(inactivity_timeout_work, inactivity_timeout_handler);

static void motion_params_reset(void)
{
	params_motion.active = IS_ENABLED(CONFIG_DSA_MOTION_DEFAULT_ACTIVE);
	params_motion.timeout_s = CONFIG_DSA_MOTION_INACTIVITY_TIMEOUT_S;
}

static bool motion_params_equal(const struct motion_state_params *a,
				const struct motion_state_params *b)
{
	return a->active == b->active && a->timeout_s == b->timeout_s;
}

static void motion_enter_eco(void)
{
	if (eco_active) {
		return;
	}
	eco_active = true;
	radio_set_eco_override(true);
	led_suspend_blinking();
	device_set_network_status_bits(ECO_MODE_MASK, ECO_MODE_MASK);
	printk("Motion: no activity for %u s, entering energy conservation mode\n",
	       params_motion.timeout_s);
}

static void motion_exit_eco(void)
{
	if (!eco_active) {
		return;
	}
	eco_active = false;
	radio_set_eco_override(false);
	led_resume_blinking();
	device_set_network_status_bits(ECO_MODE_MASK, 0);
	printk("Motion: activity detected, leaving energy conservation mode\n");
}

/* Caller must hold motion_lock. */
static void motion_reschedule_timer_locked(void)
{
	if (params_motion.active && motion_available) {
		k_work_reschedule(&inactivity_timeout_work,
				  K_SECONDS(params_motion.timeout_s));
	} else {
		k_work_cancel_delayable(&inactivity_timeout_work);
	}
}

static void inactivity_timeout_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&motion_lock, K_FOREVER);
	if (params_motion.active && motion_available) {
		motion_enter_eco();
	}
	k_mutex_unlock(&motion_lock);
}

#if MOTION_SENSOR_PRESENT
static void motion_trigger_handler(const struct device *dev,
				   const struct sensor_trigger *trigger)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(trigger);

	k_mutex_lock(&motion_lock, K_FOREVER);
	motion_exit_eco();
	motion_reschedule_timer_locked();
	k_mutex_unlock(&motion_lock);
}
#endif

int motion_init(void)
{
	int err;

	motion_params_reset();
	err = motion_params_load();
	if (err == -ENOENT) {
		printk("No stored motion parameters, using defaults\n");
	} else if (err) {
		printk("Failed to load motion parameters (err %d), using defaults\n", err);
	}

#if MOTION_SENSOR_PRESENT
	if (!device_is_ready(motion_sensor)) {
		printk("Motion sensor device not ready\n");
	} else {
		struct sensor_trigger trig = {
			.type = SENSOR_TRIG_THRESHOLD,
			.chan = SENSOR_CHAN_ACCEL_XYZ,
		};

		err = sensor_trigger_set(motion_sensor, &trig, motion_trigger_handler);
		if (err) {
			printk("Motion sensor trigger setup failed (err %d)\n", err);
		} else {
			motion_available = true;
			printk("Motion sensor initialized\n");
		}
	}
#else
	printk("Board provides no motion sensor; inactivity detection disabled\n");
#endif

	device_set_radio_status_bit(RADIO_STATUS_MOTION_UNAVAILABLE, !motion_available);

	k_mutex_lock(&motion_lock, K_FOREVER);
	motion_reschedule_timer_locked();
	k_mutex_unlock(&motion_lock);

	return 0;
}

void motion_command_begin(void)
{
	command_old_params_motion = params_motion;
	command_batch_active = true;
}

void motion_apply_command(uint8_t parameter, uint16_t value)
{
	switch (parameter) {
	case P_MOTION_ACTIVE:
		if (value > 1U) {
			printk("Rejecting invalid motion-active value %u\n", value);
			return;
		}
		params_motion.active = value != 0U;
		printk("Inactivity detection %s\n",
		       params_motion.active ? "enabled" : "disabled");
		break;
	case P_MOTION_INACTIVITY_TIMEOUT_S:
		if (value == 0U) {
			printk("Rejecting zero inactivity timeout\n");
			return;
		}
		params_motion.timeout_s = value;
		printk("Inactivity timeout set to %u s\n", value);
		break;
	case P_MOTION_RESET_PARAMS:
		motion_params_reset();
		printk("Motion parameters reset\n");
		break;
	default:
		printk("Unknown motion parameter 0x%02x value %u\n", parameter, value);
		break;
	}
}

void motion_command_commit(void)
{
	int err;

	if (!command_batch_active) {
		return;
	}
	command_batch_active = false;

	if (motion_params_equal(&command_old_params_motion, &params_motion)) {
		return;
	}

	err = motion_params_save();
	if (err) {
		printk("Failed to save motion parameters (err %d)\n", err);
	}

	k_mutex_lock(&motion_lock, K_FOREVER);
	if (!params_motion.active) {
		motion_exit_eco();
	}
	motion_reschedule_timer_locked();
	k_mutex_unlock(&motion_lock);
}

int motion_params_load(void)
{
	uint8_t stored[MOTION_PARAMS_STORED_SIZE];
	int err;

	err = param_storage_load(MOTION_PARAMS_STORAGE_KEY, stored, sizeof(stored));
	if (err == -ENOENT) {
		device_set_storage_fault(STORAGE_FAULT_MOTION_PARAMS, false);
		return err;
	}
	if (err) {
		device_set_storage_fault(STORAGE_FAULT_MOTION_PARAMS, true);
		return err;
	}

	if (stored[0] > 1U || sys_get_be16(&stored[1]) == 0U) {
		device_set_storage_fault(STORAGE_FAULT_MOTION_PARAMS, true);
		return -EBADMSG;
	}

	params_motion.active = stored[0] != 0U;
	params_motion.timeout_s = sys_get_be16(&stored[1]);
	device_set_storage_fault(STORAGE_FAULT_MOTION_PARAMS, false);
	return 0;
}

int motion_params_save(void)
{
	uint8_t stored[MOTION_PARAMS_STORED_SIZE];
	int err;

	stored[0] = params_motion.active ? 1U : 0U;
	sys_put_be16(params_motion.timeout_s, &stored[1]);

	err = param_storage_save(MOTION_PARAMS_STORAGE_KEY, stored, sizeof(stored));
	device_set_storage_fault(STORAGE_FAULT_MOTION_PARAMS, err != 0);
	return err;
}
