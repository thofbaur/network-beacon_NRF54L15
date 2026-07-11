#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "common_include.h"
#include "led.h"
#include "param_storage.h"

#define LED_PARAMS_STORAGE_KEY "dsa/main"
#define LED_BLINK_INTERVAL_MS 20000 // TODO  increase for production
#define LED_BLINK_ON_MS 30
#define LED_SELF_REPORT_ON_MS 2000

#if DT_NODE_HAS_STATUS(DT_ALIAS(green_led), okay)
#define LED_NODE DT_ALIAS(green_led)
#define LED_ALIAS_NAME "green_led"
#else
#define LED_NODE DT_ALIAS(led0)
#define LED_ALIAS_NAME "led0"
#endif

#if DT_NODE_HAS_STATUS(DT_ALIAS(red_led), okay) && \
	!DT_SAME_NODE(DT_ALIAS(red_led), LED_NODE)
#define SELF_REPORT_LED_NODE DT_ALIAS(red_led)
#define SELF_REPORT_LED_ALIAS_NAME "red_led"
#elif DT_NODE_HAS_STATUS(DT_ALIAS(led1), okay) && \
	!DT_SAME_NODE(DT_ALIAS(led1), LED_NODE)
#define SELF_REPORT_LED_NODE DT_ALIAS(led1)
#define SELF_REPORT_LED_ALIAS_NAME "led1"
#elif DT_NODE_HAS_STATUS(DT_ALIAS(led2), okay) && \
	!DT_SAME_NODE(DT_ALIAS(led2), LED_NODE)
#define SELF_REPORT_LED_NODE DT_ALIAS(led2)
#define SELF_REPORT_LED_ALIAS_NAME "led2"
#elif DT_NODE_HAS_STATUS(DT_ALIAS(led3), okay) && \
	!DT_SAME_NODE(DT_ALIAS(led3), LED_NODE)
#define SELF_REPORT_LED_NODE DT_ALIAS(led3)
#define SELF_REPORT_LED_ALIAS_NAME "led3"
#elif DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay) && \
	!DT_SAME_NODE(DT_ALIAS(led0), LED_NODE)
#define SELF_REPORT_LED_NODE DT_ALIAS(led0)
#define SELF_REPORT_LED_ALIAS_NAME "led0"
#else
#define SELF_REPORT_LED_NODE LED_NODE
#define SELF_REPORT_LED_ALIAS_NAME LED_ALIAS_NAME
#endif

#define SELF_REPORT_LED_IS_STATUS_LED \
	DT_SAME_NODE(SELF_REPORT_LED_NODE, LED_NODE)

struct led_params {
	bool led_active;
};

static struct led_params params_led;
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(LED_NODE, gpios, { 0 });
static const struct gpio_dt_spec self_report_led =
	GPIO_DT_SPEC_GET_OR(SELF_REPORT_LED_NODE, gpios, { 0 });
static bool led_ready;
static bool led_on;
static bool self_report_led_ready;

static void led_blink_handler(struct k_work *work);
static void led_self_report_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(led_blink_work, led_blink_handler);
static K_WORK_DELAYABLE_DEFINE(led_self_report_work,
			       led_self_report_handler);

static void led_params_reset(void)
{
	params_led.led_active = true;
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
	if (SELF_REPORT_LED_IS_STATUS_LED) {
		return led_set(on);
	}

	if (!self_report_led_ready) {
		return -ENODEV;
	}

	return gpio_pin_set_dt(&self_report_led, on ? 1 : 0);
}

static void led_schedule_next_blink(void)
{
	if (led_ready && params_led.led_active) {
		k_work_reschedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
	}
}

static void led_stop_blinking(void)
{
	k_work_cancel_delayable(&led_blink_work);
	led_set(false);
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
		k_work_reschedule(&led_blink_work, K_MSEC(LED_BLINK_ON_MS));
	}
}

static void led_self_report_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	self_report_led_set(false);
	if (SELF_REPORT_LED_IS_STATUS_LED) {
		led_schedule_next_blink();
	}
}

static void led_gpio_init(void)
{
	int err;

	if (!led.port) {
		printk("LED alias %s not available\n", LED_ALIAS_NAME);
		return;
	}

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

	if (SELF_REPORT_LED_IS_STATUS_LED) {
		self_report_led_ready = led_ready;
		return;
	}

	if (!self_report_led.port) {
		printk("Self-report LED alias %s not available\n",
		       SELF_REPORT_LED_ALIAS_NAME);
		return;
	}

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
}

void led_signal_self_report(void)
{
	if (!self_report_led_ready) {
		return;
	}

	if (SELF_REPORT_LED_IS_STATUS_LED) {
		k_work_cancel_delayable(&led_blink_work);
	}

	if (!self_report_led_set(true)) {
		k_work_reschedule(&led_self_report_work,
				  K_MSEC(LED_SELF_REPORT_ON_MS));
	}
}

void led_apply_command(uint8_t parameter, uint16_t value)
{
	struct led_params old_params_led = params_led;

	switch (parameter) {
	case P_MAIN_LED_ACTIVE:
		params_led.led_active = value != 0;
		printk("LED blink %s\n", params_led.led_active ? "enabled" : "disabled");
		if (params_led.led_active) {
			led_schedule_next_blink();
		} else {
			led_stop_blinking();
		}
		break;
	case P_MAIN_RESET_PARAMS:
		led_params_reset();
		led_schedule_next_blink();
		printk("LED parameters reset\n");
		break;
	default:
		printk("Unknown LED parameter 0x%02x value %u\n", parameter, value);
		break;
	}

	if (memcmp(&old_params_led, &params_led, sizeof(params_led)) != 0) {
		int err = led_params_save();

		if (err) {
			printk("Failed to save LED parameters (err %d)\n", err);
		}
	}
}

int led_params_load(void)
{
	return param_storage_load(LED_PARAMS_STORAGE_KEY,
				  &params_led, sizeof(params_led));
}

int led_params_save(void)
{
	return param_storage_save(LED_PARAMS_STORAGE_KEY,
				  &params_led, sizeof(params_led));
}
