#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "common_include.h"
#include "device.h"
#include "led.h"
#include "ram_log_ring.h"
#include "self_report.h"
#include "self_report_storage.h"
#include "storage_work_queue.h"

#define SELF_REPORT_DEBOUNCE_MS 40

#define SELF_REPORT_BUTTON_NODE DT_NODELABEL(button0)

BUILD_ASSERT(DT_NODE_HAS_STATUS(SELF_REPORT_BUTTON_NODE, okay),
	     "Board must provide button0");

BUILD_ASSERT(CONFIG_DSA_SELF_REPORT_RING_COUNT > 0,
	     "Self-report ring buffer must have at least one entry");
BUILD_ASSERT(CONFIG_DSA_SELF_REPORT_FLUSH_BATCH <=
	     CONFIG_DSA_SELF_REPORT_FLUSH_THRESHOLD,
	     "Self-report flush batch must not exceed threshold");
BUILD_ASSERT(CONFIG_DSA_SELF_REPORT_FLUSH_THRESHOLD <=
	     CONFIG_DSA_SELF_REPORT_RING_COUNT,
	     "Self-report flush threshold exceeds RAM ring");
BUILD_ASSERT(CONFIG_DSA_SELF_REPORT_FLUSH_BATCH <=
	     SELF_REPORT_STORAGE_BLOCK_ENTRIES,
	     "Self-report flush batch exceeds flash block");

static const struct gpio_dt_spec self_report_button =
	GPIO_DT_SPEC_GET(SELF_REPORT_BUTTON_NODE, gpios);

static struct gpio_callback self_report_button_cb;
static bool button_ready;
static bool button_pressed;

static void long_press_handler(struct k_work *work);
static void debounce_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(long_press_work, long_press_handler);
static K_WORK_DELAYABLE_DEFINE(debounce_work, debounce_handler);

static const struct ram_log_ring_config self_report_cfg = {
	.ring_count = CONFIG_DSA_SELF_REPORT_RING_COUNT,
	.entry_size = SELF_REPORT_ENTRY_SIZE,
	.flush_threshold = CONFIG_DSA_SELF_REPORT_FLUSH_THRESHOLD,
	.flush_batch = CONFIG_DSA_SELF_REPORT_FLUSH_BATCH,
	.flush_retry_ms = CONFIG_DSA_SELF_REPORT_FLUSH_RETRY_MS,
	.storage_init = self_report_storage_init,
	.storage_append = self_report_storage_append,
	.storage_peek = self_report_storage_peek,
	.storage_drop = self_report_storage_drop,
	.storage_sync = self_report_storage_sync,
	.storage_get_count = self_report_storage_get_count,
	.storage_full_bit = STORAGE_FULL_SELF_REPORT,
	.domain_name = "self-report",
};

static uint8_t self_report_entries[CONFIG_DSA_SELF_REPORT_RING_COUNT *
				   SELF_REPORT_ENTRY_SIZE];
static uint8_t self_report_flush_buffer[SELF_REPORT_STORAGE_BLOCK_ENTRIES *
					SELF_REPORT_ENTRY_SIZE];
static K_MUTEX_DEFINE(self_report_lock);

static struct ram_log_ring self_report_ring = {
	.cfg = &self_report_cfg,
	.lock = &self_report_lock,
	.entries = self_report_entries,
	.flush_buffer = self_report_flush_buffer,
};

static void self_report_time_put(uint8_t time[SELF_REPORT_ENTRY_SIZE],
				 uint32_t uptime_s)
{
	time[0] = (uptime_s >> 16) & 0xff;
	time[1] = (uptime_s >> 8) & 0xff;
	time[2] = uptime_s & 0xff;
}

static void self_report_store(uint32_t uptime_s)
{
	uint8_t entry[SELF_REPORT_ENTRY_SIZE];

	self_report_time_put(entry, uptime_s);
	if (!ram_log_ring_push(&self_report_ring, entry)) {
		return;
	}

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

static void debounce_handler(struct k_work *work)
{
	bool pressed;

	ARG_UNUSED(work);

	pressed = self_report_button_is_pressed();
	if (pressed == button_pressed) {
		return;
	}

	button_pressed = pressed;
	if (pressed) {
		k_work_reschedule(&long_press_work,
				  K_MSEC(CONFIG_DSA_SELF_REPORT_LONG_PRESS_MS));
	} else {
		k_work_cancel_delayable(&long_press_work);
	}
}

static void self_report_button_handler(const struct device *port,
				       struct gpio_callback *cb,
				       uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_reschedule(&debounce_work,
			  K_MSEC(SELF_REPORT_DEBOUNCE_MS));
}

int self_report_init(void)
{
	int err;

	storage_work_queue_init();
	ram_log_ring_init(&self_report_ring);
	err = self_report_storage_init();
	if (err) {
		printk("Failed to initialize self-report NVM storage (err %d)\n",
		       err);
		return err;
	}

	if (!device_is_ready(self_report_button.port)) {
		printk("Self-report button GPIO device not ready\n");
		return 0;
	}

	err = gpio_pin_configure_dt(&self_report_button, GPIO_INPUT);
	if (err) {
		printk("Self-report button configure failed (err %d)\n", err);
		return 0;
	}

	button_ready = true;
	button_pressed = self_report_button_is_pressed();

	gpio_init_callback(&self_report_button_cb, self_report_button_handler,
			   BIT(self_report_button.pin));

	err = gpio_add_callback(self_report_button.port, &self_report_button_cb);
	if (err) {
		printk("Self-report button callback failed (err %d)\n", err);
		button_ready = false;
		return 0;
	}

	err = gpio_pin_interrupt_configure_dt(&self_report_button,
					     GPIO_INT_EDGE_BOTH);
	if (err) {
		printk("Self-report button interrupt failed (err %d)\n", err);
		gpio_remove_callback(self_report_button.port,
				     &self_report_button_cb);
		button_ready = false;
		return 0;
	}

	if (button_pressed) {
		k_work_reschedule(&long_press_work,
				  K_MSEC(CONFIG_DSA_SELF_REPORT_LONG_PRESS_MS));
	}

	printk("Self-report button initialized on button0\n");
	return 0;
}

int self_report_export_begin(uint8_t *buffer, uint16_t buffer_len,
			     uint16_t *bytes_written)
{
	return ram_log_ring_export_begin(&self_report_ring, buffer, buffer_len,
					 bytes_written);
}

int self_report_export_commit(void)
{
	return ram_log_ring_export_commit(&self_report_ring);
}

int self_report_sync_storage(void)
{
	return ram_log_ring_sync(&self_report_ring);
}

void self_report_export_abort(void)
{
	ram_log_ring_export_abort(&self_report_ring);
}

int self_report_get_count(uint16_t *count)
{
	return ram_log_ring_get_count(&self_report_ring, count);
}

#if defined(CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORTS)
void self_report_development_validation_append(uint32_t uptime_s)
{
	self_report_store(uptime_s);
}
#endif
