#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "common_include.h"
#include "device.h"
#include "led.h"
#include "param_storage.h"

#define LED_PARAMS_STORAGE_KEY "dsa/main"
#define LED_PARAMS_STORED_SIZE 4U
#define LED_PARAMS_STORED_SIZE_V2 3U

#if DT_NODE_HAS_STATUS(DT_NODELABEL(led1_green), okay)
#define LED_NODE DT_NODELABEL(led1_green)
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(led0), okay)
#define LED_NODE DT_NODELABEL(led0)
#else
#error "Board must provide led1_green or led0"
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(led1_red), okay)
#define SELF_REPORT_LED_NODE DT_NODELABEL(led1_red)
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(led1), okay)
#define SELF_REPORT_LED_NODE DT_NODELABEL(led1)
#else
#error "Board must provide led1_red or led1"
#endif

/* Optional: a solid indicator LED for energy conservation mode. Not every
 * board has a third LED color, so this degrades gracefully instead of
 * requiring it like the two LEDs above.
 */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(led1_blue), okay)
#define MOTION_ECO_LED_NODE DT_NODELABEL(led1_blue)
#define MOTION_ECO_LED_PRESENT 1
#else
#define MOTION_ECO_LED_PRESENT 0
#endif

BUILD_ASSERT(!DT_SAME_NODE(SELF_REPORT_LED_NODE, LED_NODE),
	     "Status and self-report LEDs must be separate");
#if MOTION_ECO_LED_PRESENT
BUILD_ASSERT(!DT_SAME_NODE(MOTION_ECO_LED_NODE, LED_NODE) &&
	     !DT_SAME_NODE(MOTION_ECO_LED_NODE, SELF_REPORT_LED_NODE),
	     "Energy-conservation indicator LED must be separate");
#endif
BUILD_ASSERT(CONFIG_DSA_LED_BLINK_INTERVAL_MS % 1000 == 0,
	     "Default status LED interval must use whole seconds");
BUILD_ASSERT(CONFIG_DSA_FIREWORK_INTERVAL_MIN_MS <=
	     CONFIG_DSA_FIREWORK_INTERVAL_MAX_MS,
	     "Firework interval min must not exceed max");

struct led_params {
	bool led_active;
	uint16_t interval_s;
	bool firework_active;
};

static struct led_params params_led;
static struct led_params command_old_params_led;
static bool command_batch_active;
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
static const struct gpio_dt_spec self_report_led =
	GPIO_DT_SPEC_GET(SELF_REPORT_LED_NODE, gpios);
static bool led_ready;
static bool led_on;
static bool self_report_led_ready;
static bool firework_running;
#if MOTION_ECO_LED_PRESENT
static const struct gpio_dt_spec eco_led = GPIO_DT_SPEC_GET(MOTION_ECO_LED_NODE, gpios);
static bool eco_led_ready;
#endif

static void led_blink_handler(struct k_work *work);
static void led_self_report_handler(struct k_work *work);
static void led_firework_handler(struct k_work *work);
static bool led_params_equal(const struct led_params *a,
			     const struct led_params *b);

static K_WORK_DELAYABLE_DEFINE(led_blink_work, led_blink_handler);
static K_WORK_DELAYABLE_DEFINE(led_self_report_work,
			       led_self_report_handler);
static K_WORK_DELAYABLE_DEFINE(led_firework_work, led_firework_handler);

static void led_params_reset(void)
{
	params_led.led_active = IS_ENABLED(CONFIG_DSA_DEFAULT_LED_ACTIVE);
	params_led.interval_s = CONFIG_DSA_LED_BLINK_INTERVAL_MS / 1000;
	params_led.firework_active = IS_ENABLED(CONFIG_DSA_DEFAULT_FIREWORK_ACTIVE);
}

static bool led_params_equal(const struct led_params *a,
			     const struct led_params *b)
{
	return a->led_active == b->led_active &&
	       a->interval_s == b->interval_s &&
	       a->firework_active == b->firework_active;
}

static int led_set(bool on)
{
	int err;

	if (!led_ready) {
		return -ENODEV;
	}

	err = gpio_pin_set_dt(&led, on ? 1 : 0);
	if (!err) {
		led_on = on;
	}

	return err;
}

static int self_report_led_set(bool on)
{
	if (!self_report_led_ready) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(&self_report_led, on ? 1 : 0);
}

#if MOTION_ECO_LED_PRESENT
static int eco_led_set(bool on)
{
	if (!eco_led_ready) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(&eco_led, on ? 1 : 0);
}
#endif

static void led_schedule_next_blink(void)
{
	if (led_ready && params_led.led_active) {
		k_work_reschedule(&led_blink_work,
				  K_SECONDS(params_led.interval_s));
	}
}

static void led_stop_blinking(void)
{
	k_work_cancel_delayable(&led_blink_work);
	led_set(false);
}

/* Temporary, non-persisted suspend/resume for automatic energy-conservation
 * triggers. Does not touch params_led or go through led_params_save().
 *
 * CONFIG_DSA_DEV_ECO_LED_INDICATOR keeps a solid LED lit instead of off
 * while conserving energy, as a development-time visual aid. It works
 * against the point of energy conservation and should be off for
 * production builds.
 */
void led_suspend_blinking(void)
{
	led_stop_blinking();
#if MOTION_ECO_LED_PRESENT
	if (IS_ENABLED(CONFIG_DSA_DEV_ECO_LED_INDICATOR)) {
		eco_led_set(true);
	}
#endif
}

void led_resume_blinking(void)
{
#if MOTION_ECO_LED_PRESENT
	if (IS_ENABLED(CONFIG_DSA_DEV_ECO_LED_INDICATOR)) {
		eco_led_set(false);
	}
#endif
	led_schedule_next_blink();
}

static void led_blink_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!led_ready || !params_led.led_active) {
		led_set(false);
		return;
	}

	if (led_on) {
		led_set(false);
		led_schedule_next_blink();
		return;
	}

	if (!led_set(true)) {
		k_work_reschedule(&led_blink_work,
				  K_MSEC(CONFIG_DSA_LED_BLINK_ON_MS));
	}
}

static void led_self_report_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	self_report_led_set(false);
}

/* Explicitly-triggered visual effect (P_MAIN_FIREWORK_ACTIVE): flickers
 * randomly across whichever of the three LEDs are actually present/ready,
 * independent of eco mode - see led_suspend_blinking()/led_resume_blinking()
 * above, neither of which touch this work item, so firework keeps running
 * through eco transitions untouched. Deliberate: this is a command the user
 * turned on, not a normal operating mode that should go dark to save power.
 */
static void led_firework_handler(struct k_work *work)
{
	int (*candidates[3])(bool);
	size_t candidate_count = 0;
	uint32_t delay_ms;

	ARG_UNUSED(work);

	if (!firework_running) {
		return;
	}

	led_set(false);
	self_report_led_set(false);
	if (led_ready) {
		candidates[candidate_count++] = led_set;
	}
	if (self_report_led_ready) {
		candidates[candidate_count++] = self_report_led_set;
	}
#if MOTION_ECO_LED_PRESENT
	eco_led_set(false);
	if (eco_led_ready) {
		candidates[candidate_count++] = eco_led_set;
	}
#endif

	if (candidate_count > 0) {
		candidates[sys_rand32_get() % candidate_count](true);
	}

	delay_ms = CONFIG_DSA_FIREWORK_INTERVAL_MIN_MS +
		   sys_rand32_get() %
		   (CONFIG_DSA_FIREWORK_INTERVAL_MAX_MS -
		    CONFIG_DSA_FIREWORK_INTERVAL_MIN_MS + 1);
	k_work_reschedule(&led_firework_work, K_MSEC(delay_ms));
}

static void led_firework_start(void)
{
	if (firework_running) {
		return;
	}
	firework_running = true;

	led_stop_blinking();
	k_work_reschedule(&led_firework_work, K_NO_WAIT);
}

static void led_firework_stop(void)
{
	if (!firework_running) {
		return;
	}
	firework_running = false;

	k_work_cancel_delayable(&led_firework_work);
	led_set(false);
	self_report_led_set(false);
#if MOTION_ECO_LED_PRESENT
	eco_led_set(false);
#endif
	led_schedule_next_blink();
}

static void led_gpio_init(void)
{
	int err;

	if (!device_is_ready(led.port)) {
		printk("LED GPIO device not ready\n");
		return;
	}

	err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("LED configure failed (err %d)\n", err);
		return;
	}

	led_ready = true;
	led_schedule_next_blink();
}

static void self_report_led_gpio_init(void)
{
	int err;

	if (!device_is_ready(self_report_led.port)) {
		printk("Self-report LED GPIO device not ready\n");
		return;
	}

	err = gpio_pin_configure_dt(&self_report_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("Self-report LED configure failed (err %d)\n", err);
		return;
	}

	self_report_led_ready = true;
}

#if MOTION_ECO_LED_PRESENT
static void eco_led_gpio_init(void)
{
	int err;

	if (!device_is_ready(eco_led.port)) {
		printk("Energy-conservation indicator LED GPIO device not ready\n");
		return;
	}

	err = gpio_pin_configure_dt(&eco_led, GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("Energy-conservation indicator LED configure failed (err %d)\n",
		       err);
		return;
	}

	eco_led_ready = true;
}
#endif

void led_init(void)
{
	int err;

	led_params_reset();
	err = led_params_load();
	if (err == -ENOENT) {
		printk("No stored LED parameters, using defaults\n");
	} else if (err) {
		printk("Failed to load LED parameters (err %d), using defaults\n", err);
	}

	led_gpio_init();
	self_report_led_gpio_init();
#if MOTION_ECO_LED_PRESENT
	eco_led_gpio_init();
#endif

	if (params_led.firework_active) {
		led_firework_start();
	}
}

void led_signal_self_report(void)
{
	if (!self_report_led_ready) {
		return;
	}

	if (!self_report_led_set(true)) {
		k_work_reschedule(&led_self_report_work,
				  K_MSEC(CONFIG_DSA_SELF_REPORT_LED_ON_MS));
	}
}

void led_command_begin(void)
{
	command_old_params_led = params_led;
	command_batch_active = true;
}

void led_apply_command(uint8_t parameter, uint16_t value)
{
	switch (parameter) {
	case P_MAIN_LED_ACTIVE:
		if (value > 1U) {
			printk("Rejecting invalid status-LED-active value %u\n",
			       value);
			return;
		}
		params_led.led_active = value != 0;
		printk("Status LED %s\n",
		       params_led.led_active ? "enabled" : "disabled");
		break;
	case P_MAIN_LED_INTERVAL_S:
		if (value == 0U) {
			printk("Rejecting zero status LED interval\n");
			return;
		}
		params_led.interval_s = value;
		printk("Status LED interval set to %u s\n", value);
		break;
	case P_MAIN_FIREWORK_ACTIVE:
		if (value > 1U) {
			printk("Rejecting invalid firework-active value %u\n", value);
			return;
		}
		params_led.firework_active = value != 0;
		printk("Firework effect %s\n",
		       params_led.firework_active ? "enabled" : "disabled");
		break;
	case P_MAIN_RESET_PARAMS:
		led_params_reset();
		printk("LED parameters reset\n");
		break;
	default:
		printk("Unknown LED parameter 0x%02x value %u\n", parameter, value);
		break;
	}
}

void led_command_commit(void)
{
	if (!command_batch_active) {
		return;
	}
	command_batch_active = false;

	if (!led_params_equal(&command_old_params_led, &params_led)) {
		int err = led_params_save();

		if (params_led.firework_active) {
			led_firework_start();
		} else {
			led_firework_stop();
			if (params_led.led_active) {
				led_schedule_next_blink();
			} else {
				led_stop_blinking();
			}
		}
		if (err) {
			printk("Failed to save LED parameters (err %d)\n", err);
		}
	}
}

int led_params_load(void)
{
	uint8_t stored[LED_PARAMS_STORED_SIZE];
	uint8_t stored_v2[LED_PARAMS_STORED_SIZE_V2];
	uint8_t old_active;
	int err;

	err = param_storage_load(LED_PARAMS_STORAGE_KEY, stored, sizeof(stored));
	if (!err) {
		if (stored[0] > 1U || sys_get_be16(&stored[1]) == 0U ||
		    stored[3] > 1U) {
			device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, true);
			return -EBADMSG;
		}
		params_led.led_active = stored[0] != 0U;
		params_led.interval_s = sys_get_be16(&stored[1]);
		params_led.firework_active = stored[3] != 0U;
		device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, false);
		return 0;
	}

	/* Migrate the previous versioned record (active + interval, no
	 * firework byte yet) - firework_active keeps whatever
	 * led_params_reset() already set it to (the Kconfig default).
	 */
	err = param_storage_load(LED_PARAMS_STORAGE_KEY, stored_v2,
				 sizeof(stored_v2));
	if (!err) {
		if (stored_v2[0] > 1U || sys_get_be16(&stored_v2[1]) == 0U) {
			device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, true);
			return -EBADMSG;
		}
		params_led.led_active = stored_v2[0] != 0U;
		params_led.interval_s = sys_get_be16(&stored_v2[1]);
		err = led_params_save();
		if (!err) {
			printk("Migrated LED parameters with default firework setting\n");
		}
		device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, err != 0);
		return err;
	}

	/* Migrate the older versioned record, which only stored active. */
	err = param_storage_load(LED_PARAMS_STORAGE_KEY,
				 &old_active, sizeof(old_active));
	if (!err) {
		if (old_active > 1U) {
			device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, true);
			return -EBADMSG;
		}
		params_led.led_active = old_active != 0U;
		err = led_params_save();
		if (!err) {
			printk("Migrated LED parameters with default interval\n");
		}
		device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, err != 0);
		return err;
	}
	if (err == -ENOENT) {
		device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, false);
		return err;
	}

	err = param_storage_load_legacy(LED_PARAMS_STORAGE_KEY,
					&old_active, sizeof(old_active));
	if (err || old_active > 1U) {
		device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, true);
		return err ? err : -EBADMSG;
	}

	params_led.led_active = old_active != 0U;
	err = led_params_save();
	if (!err) {
		printk("Migrated LED parameters to versioned storage\n");
	}
	device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, err != 0);
	return err;
}

int led_params_save(void)
{
	uint8_t stored[LED_PARAMS_STORED_SIZE];

	stored[0] = params_led.led_active ? 1U : 0U;
	sys_put_be16(params_led.interval_s, &stored[1]);
	stored[3] = params_led.firework_active ? 1U : 0U;

	int err = param_storage_save(LED_PARAMS_STORAGE_KEY,
				    stored, sizeof(stored));

	device_set_storage_fault(STORAGE_FAULT_LED_PARAMS, err != 0);
	return err;
}
