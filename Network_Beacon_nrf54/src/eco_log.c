#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "eco_log.h"
#include "eco_log_storage.h"
#include "storage_work_queue.h"

BUILD_ASSERT(CONFIG_DSA_ECO_LOG_RING_COUNT > 0,
	     "Eco log ring buffer must have at least one entry");
BUILD_ASSERT(CONFIG_DSA_ECO_LOG_FLUSH_BATCH <=
	     CONFIG_DSA_ECO_LOG_FLUSH_THRESHOLD,
	     "Eco log flush batch must not exceed threshold");
BUILD_ASSERT(CONFIG_DSA_ECO_LOG_FLUSH_THRESHOLD <=
	     CONFIG_DSA_ECO_LOG_RING_COUNT,
	     "Eco log flush threshold exceeds RAM ring");
BUILD_ASSERT(CONFIG_DSA_ECO_LOG_FLUSH_BATCH <=
	     ECO_LOG_STORAGE_BLOCK_ENTRIES,
	     "Eco log flush batch exceeds flash block");

struct eco_log_entry {
	uint8_t enter_time[3];
	uint8_t leave_time[3];
};

static struct eco_log_entry
	entries[CONFIG_DSA_ECO_LOG_RING_COUNT];
static uint16_t read_index;
static uint16_t write_index;
static uint16_t entry_count;
static uint16_t export_entries;
static uint16_t flush_read_index;
static uint8_t flush_buffer[ECO_LOG_STORAGE_BLOCK_ENTRIES *
			    ECO_LOG_ENTRY_SIZE];
static bool export_active;
static bool flush_active;
static bool eco_log_nvm_full;
enum eco_log_export_source {
	ECO_LOG_EXPORT_NONE,
	ECO_LOG_EXPORT_FLASH,
	ECO_LOG_EXPORT_RAM,
};
static enum eco_log_export_source export_source;
static K_MUTEX_DEFINE(entry_lock);

static bool pending_session_open;
static uint32_t pending_enter_uptime_s;

static void flush_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(flush_work, flush_handler);

static void schedule_flush_if_needed(void)
{
	bool needed;

	k_mutex_lock(&entry_lock, K_FOREVER);
	needed = entry_count >= CONFIG_DSA_ECO_LOG_FLUSH_THRESHOLD &&
		 !eco_log_nvm_full && !flush_active && !export_active;
	k_mutex_unlock(&entry_lock);

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

	k_mutex_lock(&entry_lock, K_FOREVER);
	if (flush_active || export_active ||
	    entry_count < CONFIG_DSA_ECO_LOG_FLUSH_THRESHOLD ||
	    entry_count < CONFIG_DSA_ECO_LOG_FLUSH_BATCH) {
		k_mutex_unlock(&entry_lock);
		return;
	}

	index = read_index;
	for (uint16_t i = 0; i < CONFIG_DSA_ECO_LOG_FLUSH_BATCH; i++) {
		memcpy(&flush_buffer[i * ECO_LOG_ENTRY_SIZE],
		       &entries[index], ECO_LOG_ENTRY_SIZE);
		index = (index + 1) % CONFIG_DSA_ECO_LOG_RING_COUNT;
	}
	flush_active = true;
	flush_read_index = read_index;
	k_mutex_unlock(&entry_lock);

	err = eco_log_storage_append(
		flush_buffer, CONFIG_DSA_ECO_LOG_FLUSH_BATCH);

	k_mutex_lock(&entry_lock, K_FOREVER);
	if (!err && read_index == flush_read_index &&
	    entry_count >= CONFIG_DSA_ECO_LOG_FLUSH_BATCH) {
		read_index = (read_index + CONFIG_DSA_ECO_LOG_FLUSH_BATCH) %
			     CONFIG_DSA_ECO_LOG_RING_COUNT;
		entry_count -= CONFIG_DSA_ECO_LOG_FLUSH_BATCH;
		schedule_again =
			entry_count >= CONFIG_DSA_ECO_LOG_FLUSH_THRESHOLD;
	} else if (!err) {
		printk("Eco log RAM changed during reserved flash flush\n");
		err = -EIO;
	} else if (err == -ENOSPC) {
		eco_log_nvm_full = true;
		printk("Eco log NVM full; keeping entries in RAM\n");
	} else {
		printk("Failed to flush eco log to NVM (err %d)\n", err);
		schedule_again = true;
	}
	flush_active = false;
	k_mutex_unlock(&entry_lock);

	if (schedule_again) {
		storage_work_reschedule(
			&flush_work,
			err ? K_MSEC(CONFIG_DSA_ECO_LOG_FLUSH_RETRY_MS) :
			      K_NO_WAIT);
	}
}

static void eco_time_put(uint8_t time[3], uint32_t uptime_s)
{
	time[0] = (uptime_s >> 16) & 0xff;
	time[1] = (uptime_s >> 8) & 0xff;
	time[2] = uptime_s & 0xff;
}

static void eco_log_store(uint32_t enter_s, uint32_t leave_s)
{
	k_mutex_lock(&entry_lock, K_FOREVER);

	if (entry_count == CONFIG_DSA_ECO_LOG_RING_COUNT &&
	    (export_active || flush_active)) {
		k_mutex_unlock(&entry_lock);
		schedule_flush_if_needed();
		printk("Eco log RAM full; dropping newest session while flushing\n");
		return;
	}

	eco_time_put(entries[write_index].enter_time, enter_s);
	eco_time_put(entries[write_index].leave_time, leave_s);
	write_index = (write_index + 1) % CONFIG_DSA_ECO_LOG_RING_COUNT;

	if (entry_count == CONFIG_DSA_ECO_LOG_RING_COUNT) {
		read_index = (read_index + 1) % CONFIG_DSA_ECO_LOG_RING_COUNT;
	} else {
		entry_count++;
	}

	k_mutex_unlock(&entry_lock);
	schedule_flush_if_needed();

	printk("Logged eco session %u..%u s\n", enter_s, leave_s);
}

int eco_log_init(void)
{
	int err;

	storage_work_queue_init();
	err = eco_log_storage_init();
	if (err) {
		printk("Failed to initialize eco log NVM storage (err %d)\n",
		       err);
		return err;
	}

	return 0;
}

void eco_log_enter(void)
{
	k_mutex_lock(&entry_lock, K_FOREVER);
	pending_enter_uptime_s = (uint32_t)k_uptime_seconds();
	pending_session_open = true;
	k_mutex_unlock(&entry_lock);
}

void eco_log_leave(void)
{
	uint32_t enter_s;
	uint32_t leave_s;

	k_mutex_lock(&entry_lock, K_FOREVER);
	if (!pending_session_open) {
		k_mutex_unlock(&entry_lock);
		return;
	}
	enter_s = pending_enter_uptime_s;
	leave_s = (uint32_t)k_uptime_seconds();
	pending_session_open = false;
	k_mutex_unlock(&entry_lock);

	eco_log_store(enter_s, leave_s);
}

int eco_log_export_begin(uint8_t *buffer, uint16_t buffer_len,
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
	buffer_len -= buffer_len % ECO_LOG_ENTRY_SIZE;
	if (buffer_len == 0) {
		return 0;
	}

	err = eco_log_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&entry_lock, K_FOREVER);

	if (export_active || flush_active) {
		k_mutex_unlock(&entry_lock);
		return -EBUSY;
	}

	export_active = true;
	export_source = ECO_LOG_EXPORT_FLASH;
	export_entries = 0;
	k_mutex_unlock(&entry_lock);

	err = eco_log_storage_get_count(&flash_count);
	if (err) {
		eco_log_export_abort();
		return err;
	}
	if (flash_count > 0) {
		err = eco_log_storage_peek(buffer, buffer_len, &written);
		if (err || written == 0) {
			eco_log_export_abort();
			return err ? err : -EIO;
		}

		k_mutex_lock(&entry_lock, K_FOREVER);
		if (!export_active ||
		    export_source != ECO_LOG_EXPORT_FLASH ||
		    export_entries != 0) {
			export_active = false;
			export_entries = 0;
			export_source = ECO_LOG_EXPORT_NONE;
			k_mutex_unlock(&entry_lock);
			schedule_flush_if_needed();
			return -EIO;
		}
		export_entries = written / ECO_LOG_ENTRY_SIZE;
		k_mutex_unlock(&entry_lock);
		*bytes_written = written;
		return 0;
	}

	k_mutex_lock(&entry_lock, K_FOREVER);
	if (!export_active ||
	    export_source != ECO_LOG_EXPORT_FLASH ||
	    export_entries != 0) {
		export_active = false;
		export_entries = 0;
		export_source = ECO_LOG_EXPORT_NONE;
		k_mutex_unlock(&entry_lock);
		schedule_flush_if_needed();
		return -EIO;
	}

	entries_available = entry_count;
	index = read_index;

	while (entries_available > 0 &&
	       (buffer_len - written) >= ECO_LOG_ENTRY_SIZE) {
		memcpy(&buffer[written], &entries[index], ECO_LOG_ENTRY_SIZE);
		written += ECO_LOG_ENTRY_SIZE;
		index = (index + 1) % CONFIG_DSA_ECO_LOG_RING_COUNT;
		entries_available--;
	}
	if (written > 0) {
		export_source = ECO_LOG_EXPORT_RAM;
		export_entries = written / ECO_LOG_ENTRY_SIZE;
	} else {
		export_active = false;
		export_source = ECO_LOG_EXPORT_NONE;
	}
	*bytes_written = written;
	k_mutex_unlock(&entry_lock);

	return 0;
}

int eco_log_export_commit(void)
{
	enum eco_log_export_source source;
	uint16_t exported_entries;
	bool block_retired = false;
	int err = 0;

	k_mutex_lock(&entry_lock, K_FOREVER);

	if (!export_active) {
		err = -EINVAL;
		goto out;
	}

	source = export_source;
	exported_entries = export_entries;
	if (source == ECO_LOG_EXPORT_FLASH) {
		k_mutex_unlock(&entry_lock);
		err = eco_log_storage_drop(exported_entries, &block_retired);
		k_mutex_lock(&entry_lock, K_FOREVER);
		if (!export_active || export_source != source ||
		    export_entries != exported_entries) {
			err = -EIO;
		}
		if (!err && block_retired) {
			eco_log_nvm_full = false;
		}
	} else if (source == ECO_LOG_EXPORT_RAM &&
		   exported_entries <= entry_count) {
		read_index = (read_index + export_entries) %
			     CONFIG_DSA_ECO_LOG_RING_COUNT;
		entry_count -= export_entries;
	} else {
		err = -EINVAL;
	}

out:
	export_active = false;
	export_entries = 0;
	export_source = ECO_LOG_EXPORT_NONE;
	k_mutex_unlock(&entry_lock);

	if (!err) {
		schedule_flush_if_needed();
	}
	return err;
}

int eco_log_sync_storage(void)
{
	return eco_log_storage_sync();
}

void eco_log_export_abort(void)
{
	k_mutex_lock(&entry_lock, K_FOREVER);
	export_active = false;
	export_entries = 0;
	export_source = ECO_LOG_EXPORT_NONE;
	k_mutex_unlock(&entry_lock);

	schedule_flush_if_needed();
}

int eco_log_get_count(uint16_t *count)
{
	uint32_t total;
	int err;

	if (!count) {
		return -EINVAL;
	}

	err = eco_log_storage_get_count(&total);
	if (err) {
		return err;
	}

	k_mutex_lock(&entry_lock, K_FOREVER);
	total += entry_count;
	k_mutex_unlock(&entry_lock);

	*count = (uint16_t)MIN(total, UINT16_MAX);
	return 0;
}
