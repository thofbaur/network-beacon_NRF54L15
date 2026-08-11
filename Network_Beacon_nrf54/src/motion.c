#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
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

/* device_is_ready() only reflects Zephyr's boot-time driver init result, not
 * a live probe - it can't recover on its own if the ADXL367 wasn't
 * powered/settled yet when that ran, which happens on a genuine power-cycle
 * (a debugger-triggered reset leaves the sensor rail already warm from the
 * prior session). sensor_trigger_set() does talk to the hardware live, so
 * retry the whole bring-up sequence - readiness check and trigger setup -
 * for a bounded window instead of giving up after one attempt.
 */
#define MOTION_SENSOR_READY_RETRY_MS 20
#define MOTION_SENSOR_READY_TIMEOUT_MS 500

#if DT_HAS_ALIAS(motion_detector)
#define MOTION_SENSOR_NODE DT_ALIAS(motion_detector)
#define MOTION_SENSOR_PRESENT 1
#else
#define MOTION_SENSOR_PRESENT 0
#endif

#if MOTION_SENSOR_PRESENT
/* The ADXL367 Zephyr driver probes the chip exactly once, automatically,
 * from its own POST_KERNEL init at CONFIG_SENSOR_INIT_PRIORITY - well
 * before main()/motion_init() ever runs, and before the sensor's VDD rail
 * has necessarily settled on a genuine power-cycle (a debugger-triggered
 * reset never drops that rail, so it never hits this window). If that one
 * probe fails, device_is_ready() is latched false for the rest of the
 * session with no application-level way to retry it. Run a plain blocking
 * delay at a lower POST_KERNEL priority so it executes first, giving the
 * rail time to settle before the driver's probe runs at all.
 */
#define MOTION_SENSOR_BOOT_DELAY_MS 50
#define MOTION_SENSOR_BOOT_DELAY_PRIORITY 10

BUILD_ASSERT(MOTION_SENSOR_BOOT_DELAY_PRIORITY < CONFIG_SENSOR_INIT_PRIORITY,
	    "Motion sensor boot delay must run before the sensor driver's own init");

static int motion_sensor_boot_delay(void)
{
	k_msleep(MOTION_SENSOR_BOOT_DELAY_MS);
	return 0;
}

SYS_INIT(motion_sensor_boot_delay, POST_KERNEL, MOTION_SENSOR_BOOT_DELAY_PRIORITY);
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
	int64_t motion_sensor_retry_deadline = k_uptime_get() + MOTION_SENSOR_READY_TIMEOUT_MS;
	int motion_sensor_err;

	for (;;) {
		if (!device_is_ready(motion_sensor)) {
			motion_sensor_err = -ENODEV;
		} else {
			struct sensor_trigger trig = {
				.type = SENSOR_TRIG_THRESHOLD,
				.chan = SENSOR_CHAN_ACCEL_XYZ,
			};

			motion_sensor_err = sensor_trigger_set(motion_sensor, &trig,
							       motion_trigger_handler);
		}

		if (!motion_sensor_err) {
			motion_available = true;
			printk("Motion sensor initialized\n");
			break;
		}

		if (k_uptime_get() >= motion_sensor_retry_deadline) {
			printk("Motion sensor bring-up failed (err %d) after %d ms\n",
			       motion_sensor_err, MOTION_SENSOR_READY_TIMEOUT_MS);
			break;
		}

		k_sleep(K_MSEC(MOTION_SENSOR_READY_RETRY_MS));
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
