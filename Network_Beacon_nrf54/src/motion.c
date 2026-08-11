#include <errno.h>
#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
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

/* A fixed post-boot delay before probing (tried previously) can't be sized
 * correctly without knowing the board's actual rail ramp-up time on a
 * genuine power-cycle (LDO turn-on, bulk cap charge - on top of the
 * ADXL367's own 9 ms Power-Up to Standby time, which only starts once VDD
 * is already valid). Guessing a number that "should" be enough isn't
 * reliable, and there's no RTT/scope in the field to measure the real
 * figure and confirm it.
 *
 * So don't guess: the node is marked zephyr,deferred-init in the overlay
 * (Zephyr won't auto-probe it at POST_KERNEL boot), and motion_init() polls
 * the chip directly over I2C - a plain register read, safe to retry as many
 * times as needed - until it actually acks with its correct DEVID. Only
 * once that live read succeeds do we spend Zephyr's one-shot device_init()
 * on the real driver probe.
 *
 * That device_init() really is one-shot: Zephyr latches
 * dev->state->initialized (regardless of success) the moment
 * do_device_init() runs, and this driver registers no deinit_fn, so
 * device_deinit()/a second device_init() can't recover a failed attempt -
 * only a full reboot can. Hence doing the raw-I2C liveness check first
 * instead of retrying device_init() itself.
 */
#define MOTION_SENSOR_PROBE_RETRY_MS 5
#define MOTION_SENSOR_PROBE_TIMEOUT_MS 3000

/* Once the liveness probe above has confirmed the chip is actually on the
 * bus, device_init() can still fail for a reason that has nothing to do
 * with power/timing: adxl367_probe() (adxl367.c, out-of-tree Zephyr driver)
 * unconditionally runs a self-test as part of every single probe - forces
 * a known electrostatic deflection and checks the measured delta falls in
 * a fixed window (ADXL367_SELF_TEST_MIN/MAX, not Kconfig-tunable, no way to
 * skip it). That measurement spans ~450 ms (two ~160-190 ms settle waits at
 * our 25 Hz ODR) during which any real vibration - e.g. someone handling
 * the tag right as it powers up, exactly what a manual power-cycle test
 * looks like - can push the reading outside the expected window and fail
 * the probe, even though the chip itself is fully alive and healthy.
 *
 * Retrying just needs the vibration to have stopped by the next attempt,
 * which a plain device_init() can't do (see the comment above: it's a
 * true one-shot once do_device_init() has run). There's no deinit_fn to
 * unlock it through the supported API, so this reaches past that API and
 * clears dev->state->initialized directly to force a real re-probe -
 * unsupported use of Zephyr's device-model internals, justified only
 * because there is no other way to get adxl367_probe() to run again
 * within the same boot. Safe specifically for this driver because
 * adxl367_init()/adxl367_probe() always soft-resets the chip first, so
 * each attempt starts from a clean hardware state rather than resuming
 * from whatever the previous failed attempt left behind.
 */
#define MOTION_SENSOR_PROBE_ATTEMPTS 5
#define MOTION_SENSOR_PROBE_SETTLE_MS 100

#if DT_HAS_ALIAS(motion_detector)
#define MOTION_SENSOR_NODE DT_ALIAS(motion_detector)
#define MOTION_SENSOR_PRESENT 1
#else
#define MOTION_SENSOR_PRESENT 0
#endif

#if MOTION_SENSOR_PRESENT
BUILD_ASSERT(DT_ON_BUS(MOTION_SENSOR_NODE, i2c),
	    "Motion sensor liveness probe assumes an I2C bus");
#define MOTION_SENSOR_BUS_NODE DT_BUS(MOTION_SENSOR_NODE)
#define MOTION_SENSOR_I2C_ADDR DT_REG_ADDR(MOTION_SENSOR_NODE)
#define MOTION_SENSOR_DEVID_REG 0x00u
#define MOTION_SENSOR_DEVID_VAL 0xADu /* ADXL367 datasheet: fixed Analog Devices ID */
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
static const struct device *const motion_sensor_bus = DEVICE_DT_GET(MOTION_SENSOR_BUS_NODE);
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
	int motion_sensor_err = -ENODEV;
	bool motion_sensor_seen_alive = false;

	if (!device_is_ready(motion_sensor_bus)) {
		printk("Motion sensor bus not ready\n");
	} else {
		int64_t motion_sensor_deadline =
			k_uptime_get() + MOTION_SENSOR_PROBE_TIMEOUT_MS;

		/* Cheap insurance against a bus a prior session may have left
		 * mid-transaction (SDA held low) when power dropped.
		 */
		(void)i2c_recover_bus(motion_sensor_bus);

		for (;;) {
			uint8_t devid = 0;

			motion_sensor_err = i2c_reg_read_byte(motion_sensor_bus,
							      MOTION_SENSOR_I2C_ADDR,
							      MOTION_SENSOR_DEVID_REG,
							      &devid);
			if (!motion_sensor_err && devid != MOTION_SENSOR_DEVID_VAL) {
				motion_sensor_err = -ENODEV;
			}

			if (!motion_sensor_err) {
				motion_sensor_seen_alive = true;
				break;
			}

			if (k_uptime_get() >= motion_sensor_deadline) {
				printk("Motion sensor liveness probe failed (err %d) after %d ms\n",
				       motion_sensor_err, MOTION_SENSOR_PROBE_TIMEOUT_MS);
				break;
			}

			k_sleep(K_MSEC(MOTION_SENSOR_PROBE_RETRY_MS));
		}
	}

	if (!motion_sensor_err) {
		for (int attempt = 0; attempt < MOTION_SENSOR_PROBE_ATTEMPTS; attempt++) {
			motion_sensor_err = device_init(motion_sensor);
			if (!motion_sensor_err) {
				break;
			}

			printk("Motion sensor probe attempt %d/%d failed (err %d)\n",
			       attempt + 1, MOTION_SENSOR_PROBE_ATTEMPTS, motion_sensor_err);

			if (attempt + 1 < MOTION_SENSOR_PROBE_ATTEMPTS) {
				motion_sensor->state->initialized = false;
				k_sleep(K_MSEC(MOTION_SENSOR_PROBE_SETTLE_MS));
			}
		}
	}

	if (!motion_sensor_err) {
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
	} else {
		printk("Motion sensor bring-up failed (err %d)\n", motion_sensor_err);
	}

	/* Only meaningful together with RADIO_STATUS_MOTION_UNAVAILABLE: set
	 * means the liveness probe above never once got a correct DEVID back
	 * (still a power/timing/wiring question); clear means the chip
	 * answered fine but bring-up failed some other way afterwards (a
	 * device_init()/trigger_set() problem instead - a different bug).
	 */
	device_set_radio_status_bit(RADIO_STATUS_MOTION_PROBE_TIMEOUT, !motion_sensor_seen_alive);
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
