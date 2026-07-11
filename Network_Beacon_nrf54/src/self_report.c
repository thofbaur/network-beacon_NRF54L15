#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "common_include.h"
#include "led.h"
#include "self_report.h"

#define SELF_REPORT_LONG_PRESS_MS 3000
#define SELF_REPORT_RING_COUNT 100

#if DT_NODE_HAS_STATUS(DT_ALIAS(button0), okay)
#define SELF_REPORT_BUTTON_NODE DT_ALIAS(button0)
#define SELF_REPORT_BUTTON_ALIAS "button0"
#elif DT_NODE_HAS_STATUS(DT_NODELABEL(button0), okay)
#define SELF_REPORT_BUTTON_NODE DT_NODELABEL(button0)
#define SELF_REPORT_BUTTON_ALIAS "button0"
#elif DT_NODE_HAS_STATUS(DT_ALIAS(sw0), okay)
#define SELF_REPORT_BUTTON_NODE DT_ALIAS(sw0)
#define SELF_REPORT_BUTTON_ALIAS "sw0"
#else
#define SELF_REPORT_BUTTON_NODE DT_INVALID_NODE
#define SELF_REPORT_BUTTON_ALIAS "button0/sw0"
#endif

BUILD_ASSERT(SELF_REPORT_RING_COUNT > 0,
	     "Self-report ring buffer must have at least one entry");

struct self_report_entry {
	uint8_t uptime_s[SELF_REPORT_ENTRY_SIZE];
};

static const struct gpio_dt_spec self_report_button =
	GPIO_DT_SPEC_GET_OR(SELF_REPORT_BUTTON_NODE, gpios, { 0 });

static struct gpio_callback self_report_button_cb;
static struct self_report_entry reports[SELF_REPORT_RING_COUNT];
static uint16_t read_index;
static uint16_t write_index;
static uint16_t report_count;
static bool button_ready;
static bool button_pressed;
static K_MUTEX_DEFINE(report_lock);

static void long_press_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(long_press_work, long_press_handler);

static void self_report_time_put(uint8_t time[SELF_REPORT_ENTRY_SIZE],
				 uint32_t uptime_s)
{
	time[0] = (uptime_s >> 16) & 0xff;
	time[1] = (uptime_s >> 8) & 0xff;
	time[2] = uptime_s & 0xff;
}

static void self_report_store(uint32_t uptime_s)
{
	k_mutex_lock(&report_lock, K_FOREVER);

	self_report_time_put(reports[write_index].uptime_s, uptime_s);
	write_index = (write_index + 1) % SELF_REPORT_RING_COUNT;

	if (report_count == SELF_REPORT_RING_COUNT) {
		read_index = (read_index + 1) % SELF_REPORT_RING_COUNT;
	} else {
		report_count++;
	}

	k_mutex_unlock(&report_lock);

	printk("Stored self report at uptime %u s\n", uptime_s);
	led_signal_self_report();
}

static bool self_report_button_is_pressed(void)
{
	int state;

	if (!button_ready) {
		return false;
	}

	state = gpio_pin_get_dt(&self_report_button);
	if (state < 0) {
		printk("Self-report button read failed (err %d)\n", state);
		return false;
	}

	return state > 0;
}

static void long_press_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!self_report_button_is_pressed()) {
		return;
	}

	self_report_store((uint32_t)k_uptime_seconds());
}

static void self_report_button_handler(const struct device *port,
				       struct gpio_callback *cb,
				       uint32_t pins)
{
	bool pressed;

	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	pressed = self_report_button_is_pressed();
	if (pressed == button_pressed) {
		return;
	}

	button_pressed = pressed;
	if (pressed) {
		k_work_reschedule(&long_press_work,
				  K_MSEC(SELF_REPORT_LONG_PRESS_MS));
	} else {
		k_work_cancel_delayable(&long_press_work);
	}
}

void self_report_init(void)
{
	int err;

	if (!self_report_button.port) {
		printk("Self-report button alias %s not available\n",
		       SELF_REPORT_BUTTON_ALIAS);
		return;
	}

	if (!device_is_ready(self_report_button.port)) {
		printk("Self-report button GPIO device not ready\n");
		return;
	}

	err = gpio_pin_configure_dt(&self_report_button, GPIO_INPUT);
	if (err) {
		printk("Self-report button configure failed (err %d)\n", err);
		return;
	}

	button_ready = true;
	button_pressed = self_report_button_is_pressed();

	gpio_init_callback(&self_report_button_cb, self_report_button_handler,
			   BIT(self_report_button.pin));

	err = gpio_add_callback(self_report_button.port, &self_report_button_cb);
	if (err) {
		printk("Self-report button callback failed (err %d)\n", err);
		button_ready = false;
		return;
	}

	err = gpio_pin_interrupt_configure_dt(&self_report_button,
					     GPIO_INT_EDGE_BOTH);
	if (err) {
		printk("Self-report button interrupt failed (err %d)\n", err);
		gpio_remove_callback(self_report_button.port,
				     &self_report_button_cb);
		button_ready = false;
		return;
	}

	if (button_pressed) {
		k_work_reschedule(&long_press_work,
				  K_MSEC(SELF_REPORT_LONG_PRESS_MS));
	}

	printk("Self-report button initialized on %s\n",
	       SELF_REPORT_BUTTON_ALIAS);
}

uint16_t self_report_peek(uint16_t entry_offset, uint8_t *buffer,
			  uint16_t buffer_len)
{
	uint16_t entries_available;
	uint16_t bytes_written = 0;
	uint16_t index;

	k_mutex_lock(&report_lock, K_FOREVER);

	if (entry_offset >= report_count) {
		k_mutex_unlock(&report_lock);
		return 0;
	}

	entries_available = report_count - entry_offset;
	index = (read_index + entry_offset) % SELF_REPORT_RING_COUNT;

	while (entries_available > 0 &&
	       (buffer_len - bytes_written) >= SELF_REPORT_ENTRY_SIZE) {
		memcpy(&buffer[bytes_written], reports[index].uptime_s,
		       SELF_REPORT_ENTRY_SIZE);
		bytes_written += SELF_REPORT_ENTRY_SIZE;
		index = (index + 1) % SELF_REPORT_RING_COUNT;
		entries_available--;
	}

	k_mutex_unlock(&report_lock);

	return bytes_written;
}

uint16_t self_report_get_count(void)
{
	uint16_t count;

	k_mutex_lock(&report_lock, K_FOREVER);
	count = report_count;
	k_mutex_unlock(&report_lock);

	return count;
}
