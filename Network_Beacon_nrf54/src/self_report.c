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
#include "led.h"
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

struct self_report_entry {
	uint8_t uptime_s[SELF_REPORT_ENTRY_SIZE];
};

static const struct gpio_dt_spec self_report_button =
	GPIO_DT_SPEC_GET(SELF_REPORT_BUTTON_NODE, gpios);

static struct gpio_callback self_report_button_cb;
static struct self_report_entry
	reports[CONFIG_DSA_SELF_REPORT_RING_COUNT];
static uint16_t read_index;
static uint16_t write_index;
static uint16_t report_count;
static uint16_t export_entries;
static uint16_t flush_read_index;
static uint8_t flush_buffer[SELF_REPORT_STORAGE_BLOCK_ENTRIES *
			    SELF_REPORT_ENTRY_SIZE];
static bool export_active;
static bool flush_active;
static bool self_report_nvm_full;
static bool button_ready;
static bool button_pressed;
enum self_report_export_source {
	SELF_REPORT_EXPORT_NONE,
	SELF_REPORT_EXPORT_FLASH,
	SELF_REPORT_EXPORT_RAM,
};
static enum self_report_export_source export_source;
static K_MUTEX_DEFINE(report_lock);

static void long_press_handler(struct k_work *work);
static void debounce_handler(struct k_work *work);
static void flush_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(long_press_work, long_press_handler);
static K_WORK_DELAYABLE_DEFINE(debounce_work, debounce_handler);
static K_WORK_DELAYABLE_DEFINE(flush_work, flush_handler);

static void schedule_flush_if_needed(void)
{
	bool needed;

	k_mutex_lock(&report_lock, K_FOREVER);
	needed = report_count >= CONFIG_DSA_SELF_REPORT_FLUSH_THRESHOLD &&
		 !self_report_nvm_full && !flush_active && !export_active;
	k_mutex_unlock(&report_lock);

	if (needed) {
		storage_work_reschedule(&flush_work, K_NO_WAIT);
	}
}

static void flush_handler(struct k_work *work)
{
	uint16_t index;
	bool schedule_again = false;
	int err;

	ARG_UNUSED(work);

	k_mutex_lock(&report_lock, K_FOREVER);
	if (flush_active || export_active ||
	    report_count < CONFIG_DSA_SELF_REPORT_FLUSH_THRESHOLD ||
	    report_count < CONFIG_DSA_SELF_REPORT_FLUSH_BATCH) {
		k_mutex_unlock(&report_lock);
		return;
	}

	index = read_index;
	for (uint16_t i = 0; i < CONFIG_DSA_SELF_REPORT_FLUSH_BATCH; i++) {
		memcpy(&flush_buffer[i * SELF_REPORT_ENTRY_SIZE],
		       reports[index].uptime_s, SELF_REPORT_ENTRY_SIZE);
		index = (index + 1) % CONFIG_DSA_SELF_REPORT_RING_COUNT;
	}
	flush_active = true;
	flush_read_index = read_index;
	k_mutex_unlock(&report_lock);

	err = self_report_storage_append(
		flush_buffer, CONFIG_DSA_SELF_REPORT_FLUSH_BATCH);

	k_mutex_lock(&report_lock, K_FOREVER);
	if (!err && read_index == flush_read_index &&
	    report_count >= CONFIG_DSA_SELF_REPORT_FLUSH_BATCH) {
		read_index = (read_index + CONFIG_DSA_SELF_REPORT_FLUSH_BATCH) %
			     CONFIG_DSA_SELF_REPORT_RING_COUNT;
		report_count -= CONFIG_DSA_SELF_REPORT_FLUSH_BATCH;
		schedule_again =
			report_count >= CONFIG_DSA_SELF_REPORT_FLUSH_THRESHOLD;
	} else if (!err) {
		printk("Self-report RAM changed during reserved flash flush\n");
		err = -EIO;
	} else if (err == -ENOSPC) {
		self_report_nvm_full = true;
		printk("Self-report NVM full; keeping reports in RAM\n");
	} else {
		printk("Failed to flush self reports to NVM (err %d)\n", err);
		schedule_again = true;
	}
	flush_active = false;
	k_mutex_unlock(&report_lock);

	if (schedule_again) {
		storage_work_reschedule(
			&flush_work,
			err ? K_MSEC(CONFIG_DSA_SELF_REPORT_FLUSH_RETRY_MS) :
			      K_NO_WAIT);
	}
}

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

	if (report_count == CONFIG_DSA_SELF_REPORT_RING_COUNT &&
	    (export_active || flush_active)) {
		k_mutex_unlock(&report_lock);
		schedule_flush_if_needed();
		printk("Self-report RAM full; dropping newest report while flushing\n");
		return;
	}

	self_report_time_put(reports[write_index].uptime_s, uptime_s);
	write_index = (write_index + 1) %
		      CONFIG_DSA_SELF_REPORT_RING_COUNT;

	if (report_count == CONFIG_DSA_SELF_REPORT_RING_COUNT) {
		read_index = (read_index + 1) %
			     CONFIG_DSA_SELF_REPORT_RING_COUNT;
	} else {
		report_count++;
	}

	k_mutex_unlock(&report_lock);
	schedule_flush_if_needed();

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
	uint16_t entries_available;
	uint16_t written = 0;
	uint16_t index;
	uint32_t flash_count;
	int err;

	if (!buffer || !bytes_written) {
		return -EINVAL;
	}

	*bytes_written = 0;
	buffer_len -= buffer_len % SELF_REPORT_ENTRY_SIZE;
	if (buffer_len == 0) {
		return 0;
	}

	err = self_report_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&report_lock, K_FOREVER);

	if (export_active || flush_active) {
		k_mutex_unlock(&report_lock);
		return -EBUSY;
	}

	export_active = true;
	export_source = SELF_REPORT_EXPORT_FLASH;
	export_entries = 0;
	k_mutex_unlock(&report_lock);

	err = self_report_storage_get_count(&flash_count);
	if (err) {
		self_report_export_abort();
		return err;
	}
	if (flash_count > 0) {
		err = self_report_storage_peek(buffer, buffer_len, &written);
		if (err || written == 0) {
			self_report_export_abort();
			return err ? err : -EIO;
		}

		k_mutex_lock(&report_lock, K_FOREVER);
		if (!export_active ||
		    export_source != SELF_REPORT_EXPORT_FLASH ||
		    export_entries != 0) {
			export_active = false;
			export_entries = 0;
			export_source = SELF_REPORT_EXPORT_NONE;
			k_mutex_unlock(&report_lock);
			schedule_flush_if_needed();
			return -EIO;
		}
		export_entries = written / SELF_REPORT_ENTRY_SIZE;
		k_mutex_unlock(&report_lock);
		*bytes_written = written;
		return 0;
	}

	k_mutex_lock(&report_lock, K_FOREVER);
	if (!export_active ||
	    export_source != SELF_REPORT_EXPORT_FLASH ||
	    export_entries != 0) {
		export_active = false;
		export_entries = 0;
		export_source = SELF_REPORT_EXPORT_NONE;
		k_mutex_unlock(&report_lock);
		schedule_flush_if_needed();
		return -EIO;
	}

	entries_available = report_count;
	index = read_index;

	while (entries_available > 0 &&
	       (buffer_len - written) >= SELF_REPORT_ENTRY_SIZE) {
		memcpy(&buffer[written], reports[index].uptime_s,
		       SELF_REPORT_ENTRY_SIZE);
		written += SELF_REPORT_ENTRY_SIZE;
		index = (index + 1) %
			CONFIG_DSA_SELF_REPORT_RING_COUNT;
		entries_available--;
	}
	if (written > 0) {
		export_source = SELF_REPORT_EXPORT_RAM;
		export_entries = written / SELF_REPORT_ENTRY_SIZE;
	} else {
		export_active = false;
		export_source = SELF_REPORT_EXPORT_NONE;
	}
	*bytes_written = written;
	k_mutex_unlock(&report_lock);

	return 0;
}

int self_report_export_commit(void)
{
	enum self_report_export_source source;
	uint16_t entries;
	bool block_retired = false;
	int err = 0;

	k_mutex_lock(&report_lock, K_FOREVER);

	if (!export_active) {
		err = -EINVAL;
		goto out;
	}

	source = export_source;
	entries = export_entries;
	if (source == SELF_REPORT_EXPORT_FLASH) {
		k_mutex_unlock(&report_lock);
		err = self_report_storage_drop(entries, &block_retired);
		k_mutex_lock(&report_lock, K_FOREVER);
		if (!export_active || export_source != source ||
		    export_entries != entries) {
			err = -EIO;
		}
		if (!err && block_retired) {
			self_report_nvm_full = false;
		}
	} else if (source == SELF_REPORT_EXPORT_RAM &&
		   entries <= report_count) {
		read_index = (read_index + export_entries) %
			     CONFIG_DSA_SELF_REPORT_RING_COUNT;
		report_count -= export_entries;
	} else {
		err = -EINVAL;
	}

out:
	export_active = false;
	export_entries = 0;
	export_source = SELF_REPORT_EXPORT_NONE;
	k_mutex_unlock(&report_lock);

	if (!err) {
		schedule_flush_if_needed();
	}
	return err;
}

int self_report_sync_storage(void)
{
	return self_report_storage_sync();
}

void self_report_export_abort(void)
{
	k_mutex_lock(&report_lock, K_FOREVER);
	export_active = false;
	export_entries = 0;
	export_source = SELF_REPORT_EXPORT_NONE;
	k_mutex_unlock(&report_lock);

	schedule_flush_if_needed();
}

int self_report_get_count(uint16_t *count)
{
	uint32_t total;
	int err;

	if (!count) {
		return -EINVAL;
	}

	err = self_report_storage_get_count(&total);
	if (err) {
		return err;
	}

	k_mutex_lock(&report_lock, K_FOREVER);
	total += report_count;
	k_mutex_unlock(&report_lock);

	*count = (uint16_t)MIN(total, UINT16_MAX);
	return 0;
}

#if defined(CONFIG_DSA_DEV_SYNTHETIC_SELF_REPORTS)
void self_report_development_validation_append(uint32_t uptime_s)
{
	self_report_store(uptime_s);
}
#endif
