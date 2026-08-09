#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "device.h"
#include "eco_log.h"
#include "eco_log_storage.h"
#include "ram_log_ring.h"
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

static bool pending_session_open;
static uint32_t pending_enter_uptime_s;
static K_MUTEX_DEFINE(pending_session_lock);

static const struct ram_log_ring_config eco_log_cfg = {
	.ring_count = CONFIG_DSA_ECO_LOG_RING_COUNT,
	.entry_size = ECO_LOG_ENTRY_SIZE,
	.flush_threshold = CONFIG_DSA_ECO_LOG_FLUSH_THRESHOLD,
	.flush_batch = CONFIG_DSA_ECO_LOG_FLUSH_BATCH,
	.flush_retry_ms = CONFIG_DSA_ECO_LOG_FLUSH_RETRY_MS,
	.storage_init = eco_log_storage_init,
	.storage_append = eco_log_storage_append,
	.storage_peek = eco_log_storage_peek,
	.storage_drop = eco_log_storage_drop,
	.storage_sync = eco_log_storage_sync,
	.storage_get_count = eco_log_storage_get_count,
	.storage_full_bit = STORAGE_FULL_ECO_LOG,
	.domain_name = "eco log",
};

static uint8_t eco_log_entries[CONFIG_DSA_ECO_LOG_RING_COUNT *
			       ECO_LOG_ENTRY_SIZE];
static uint8_t eco_log_flush_buffer[ECO_LOG_STORAGE_BLOCK_ENTRIES *
				    ECO_LOG_ENTRY_SIZE];
static K_MUTEX_DEFINE(eco_log_lock);

static struct ram_log_ring eco_log_ring = {
	.cfg = &eco_log_cfg,
	.lock = &eco_log_lock,
	.entries = eco_log_entries,
	.flush_buffer = eco_log_flush_buffer,
};

static void eco_time_put(uint8_t time[3], uint32_t uptime_s)
{
	time[0] = (uptime_s >> 16) & 0xff;
	time[1] = (uptime_s >> 8) & 0xff;
	time[2] = uptime_s & 0xff;
}

static void eco_log_store(uint32_t enter_s, uint32_t leave_s)
{
	uint8_t entry[ECO_LOG_ENTRY_SIZE];

	eco_time_put(&entry[0], enter_s);
	eco_time_put(&entry[3], leave_s);
	if (!ram_log_ring_push(&eco_log_ring, entry)) {
		return;
	}

	printk("Logged eco session %u..%u s\n", enter_s, leave_s);
}

int eco_log_init(void)
{
	int err;

	storage_work_queue_init();
	ram_log_ring_init(&eco_log_ring);
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
	k_mutex_lock(&pending_session_lock, K_FOREVER);
	pending_enter_uptime_s = (uint32_t)k_uptime_seconds();
	pending_session_open = true;
	k_mutex_unlock(&pending_session_lock);
}

void eco_log_leave(void)
{
	uint32_t enter_s;
	uint32_t leave_s;

	k_mutex_lock(&pending_session_lock, K_FOREVER);
	if (!pending_session_open) {
		k_mutex_unlock(&pending_session_lock);
		return;
	}
	enter_s = pending_enter_uptime_s;
	leave_s = (uint32_t)k_uptime_seconds();
	pending_session_open = false;
	k_mutex_unlock(&pending_session_lock);

	eco_log_store(enter_s, leave_s);
}

int eco_log_export_begin(uint8_t *buffer, uint16_t buffer_len,
			 uint16_t *bytes_written)
{
	return ram_log_ring_export_begin(&eco_log_ring, buffer, buffer_len,
					 bytes_written);
}

int eco_log_export_commit(void)
{
	return ram_log_ring_export_commit(&eco_log_ring);
}

int eco_log_sync_storage(void)
{
	return ram_log_ring_sync(&eco_log_ring);
}

void eco_log_export_abort(void)
{
	ram_log_ring_export_abort(&eco_log_ring);
}

int eco_log_get_count(uint16_t *count)
{
	return ram_log_ring_get_count(&eco_log_ring, count);
}
