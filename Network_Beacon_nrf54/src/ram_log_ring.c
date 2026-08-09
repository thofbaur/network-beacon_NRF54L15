#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "device.h"
#include "ram_log_ring.h"
#include "storage_work_queue.h"

static uint8_t *entry_at(struct ram_log_ring *ring, uint16_t index)
{
	return &ring->entries[(size_t)index * ring->cfg->entry_size];
}

static void schedule_flush_if_needed(struct ram_log_ring *ring)
{
	bool needed;

	k_mutex_lock(ring->lock, K_FOREVER);
	needed = ring->count >= ring->cfg->flush_threshold &&
		 !ring->nvm_full && !ring->flush_active && !ring->export_active;
	k_mutex_unlock(ring->lock);

	if (needed) {
		storage_work_reschedule(&ring->flush_work, K_NO_WAIT);
	}
}

static void flush_handler(struct k_work *work)
{
	struct ram_log_ring *ring = CONTAINER_OF(
		k_work_delayable_from_work(work), struct ram_log_ring, flush_work);
	const struct ram_log_ring_config *cfg = ring->cfg;
	uint16_t index;
	bool schedule_again = false;
	int err;

	k_mutex_lock(ring->lock, K_FOREVER);
	if (ring->flush_active || ring->export_active ||
	    ring->count < cfg->flush_threshold ||
	    ring->count < cfg->flush_batch) {
		k_mutex_unlock(ring->lock);
		return;
	}

	index = ring->read_index;
	for (uint16_t i = 0; i < cfg->flush_batch; i++) {
		memcpy(&ring->flush_buffer[(size_t)i * cfg->entry_size],
		       entry_at(ring, index), cfg->entry_size);
		index = (index + 1) % cfg->ring_count;
	}
	ring->flush_active = true;
	ring->flush_read_index = ring->read_index;
	k_mutex_unlock(ring->lock);

	err = cfg->storage_append(ring->flush_buffer, cfg->flush_batch);

	k_mutex_lock(ring->lock, K_FOREVER);
	if (!err && ring->read_index == ring->flush_read_index &&
	    ring->count >= cfg->flush_batch) {
		ring->read_index = (ring->read_index + cfg->flush_batch) %
				   cfg->ring_count;
		ring->count -= cfg->flush_batch;
		schedule_again = ring->count >= cfg->flush_threshold;
	} else if (!err) {
		printk("%s RAM changed during reserved flash flush\n",
		       cfg->domain_name);
		err = -EIO;
	} else if (err == -ENOSPC) {
		ring->nvm_full = true;
		device_set_storage_full(cfg->storage_full_bit, true);
		printk("%s NVM full; keeping entries in RAM\n", cfg->domain_name);
	} else {
		printk("Failed to flush %s to NVM (err %d)\n", cfg->domain_name, err);
		schedule_again = true;
	}
	ring->flush_active = false;
	k_mutex_unlock(ring->lock);

	if (schedule_again) {
		storage_work_reschedule(
			&ring->flush_work,
			err ? K_MSEC(cfg->flush_retry_ms) : K_NO_WAIT);
	}
}

void ram_log_ring_init(struct ram_log_ring *ring)
{
	k_work_init_delayable(&ring->flush_work, flush_handler);
}

bool ram_log_ring_push(struct ram_log_ring *ring, const uint8_t *entry)
{
	const struct ram_log_ring_config *cfg = ring->cfg;

	k_mutex_lock(ring->lock, K_FOREVER);

	if (ring->count == cfg->ring_count &&
	    (ring->export_active || ring->flush_active)) {
		k_mutex_unlock(ring->lock);
		schedule_flush_if_needed(ring);
		printk("%s RAM full; dropping newest entry while flushing\n",
		       cfg->domain_name);
		return false;
	}

	memcpy(entry_at(ring, ring->write_index), entry, cfg->entry_size);
	ring->write_index = (ring->write_index + 1) % cfg->ring_count;

	if (ring->count == cfg->ring_count) {
		ring->read_index = (ring->read_index + 1) % cfg->ring_count;
	} else {
		ring->count++;
	}

	k_mutex_unlock(ring->lock);
	schedule_flush_if_needed(ring);
	return true;
}

int ram_log_ring_export_begin(struct ram_log_ring *ring, uint8_t *buffer,
			      uint16_t buffer_len, uint16_t *bytes_written)
{
	const struct ram_log_ring_config *cfg = ring->cfg;
	uint16_t entries_available;
	uint16_t written = 0;
	uint16_t index;
	uint32_t flash_count;
	int err;

	if (!buffer || !bytes_written) {
		return -EINVAL;
	}

	*bytes_written = 0;
	buffer_len -= buffer_len % cfg->entry_size;
	if (buffer_len == 0) {
		return 0;
	}

	err = cfg->storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(ring->lock, K_FOREVER);
	if (ring->export_active || ring->flush_active) {
		k_mutex_unlock(ring->lock);
		return -EBUSY;
	}
	ring->export_active = true;
	ring->export_source = RAM_LOG_RING_EXPORT_FLASH;
	ring->export_entries = 0;
	k_mutex_unlock(ring->lock);

	err = cfg->storage_get_count(&flash_count);
	if (err) {
		ram_log_ring_export_abort(ring);
		return err;
	}
	if (flash_count > 0) {
		err = cfg->storage_peek(buffer, buffer_len, &written);
		if (err || written == 0) {
			ram_log_ring_export_abort(ring);
			return err ? err : -EIO;
		}

		k_mutex_lock(ring->lock, K_FOREVER);
		if (!ring->export_active ||
		    ring->export_source != RAM_LOG_RING_EXPORT_FLASH ||
		    ring->export_entries != 0) {
			ring->export_active = false;
			ring->export_entries = 0;
			ring->export_source = RAM_LOG_RING_EXPORT_NONE;
			k_mutex_unlock(ring->lock);
			schedule_flush_if_needed(ring);
			return -EIO;
		}
		ring->export_entries = written / cfg->entry_size;
		k_mutex_unlock(ring->lock);
		*bytes_written = written;
		return 0;
	}

	k_mutex_lock(ring->lock, K_FOREVER);
	if (!ring->export_active ||
	    ring->export_source != RAM_LOG_RING_EXPORT_FLASH ||
	    ring->export_entries != 0) {
		ring->export_active = false;
		ring->export_entries = 0;
		ring->export_source = RAM_LOG_RING_EXPORT_NONE;
		k_mutex_unlock(ring->lock);
		schedule_flush_if_needed(ring);
		return -EIO;
	}

	entries_available = ring->count;
	index = ring->read_index;

	while (entries_available > 0 &&
	       (buffer_len - written) >= cfg->entry_size) {
		memcpy(&buffer[written], entry_at(ring, index), cfg->entry_size);
		written += cfg->entry_size;
		index = (index + 1) % cfg->ring_count;
		entries_available--;
	}
	if (written > 0) {
		ring->export_source = RAM_LOG_RING_EXPORT_RAM;
		ring->export_entries = written / cfg->entry_size;
	} else {
		ring->export_active = false;
		ring->export_source = RAM_LOG_RING_EXPORT_NONE;
	}
	*bytes_written = written;
	k_mutex_unlock(ring->lock);

	return 0;
}

int ram_log_ring_export_commit(struct ram_log_ring *ring)
{
	const struct ram_log_ring_config *cfg = ring->cfg;
	enum ram_log_ring_export_source source;
	uint16_t exported_entries;
	bool block_retired = false;
	int err = 0;

	k_mutex_lock(ring->lock, K_FOREVER);

	if (!ring->export_active) {
		err = -EINVAL;
		goto out;
	}

	source = ring->export_source;
	exported_entries = ring->export_entries;
	if (source == RAM_LOG_RING_EXPORT_FLASH) {
		k_mutex_unlock(ring->lock);
		err = cfg->storage_drop(exported_entries, &block_retired);
		k_mutex_lock(ring->lock, K_FOREVER);
		if (!ring->export_active || ring->export_source != source ||
		    ring->export_entries != exported_entries) {
			err = -EIO;
		}
		if (!err && block_retired) {
			ring->nvm_full = false;
			device_set_storage_full(cfg->storage_full_bit, false);
		}
	} else if (source == RAM_LOG_RING_EXPORT_RAM &&
		   exported_entries <= ring->count) {
		ring->read_index = (ring->read_index + exported_entries) %
				   cfg->ring_count;
		ring->count -= exported_entries;
	} else {
		err = -EINVAL;
	}

out:
	ring->export_active = false;
	ring->export_entries = 0;
	ring->export_source = RAM_LOG_RING_EXPORT_NONE;
	k_mutex_unlock(ring->lock);

	if (!err) {
		schedule_flush_if_needed(ring);
	}
	return err;
}

void ram_log_ring_export_abort(struct ram_log_ring *ring)
{
	k_mutex_lock(ring->lock, K_FOREVER);
	ring->export_active = false;
	ring->export_entries = 0;
	ring->export_source = RAM_LOG_RING_EXPORT_NONE;
	k_mutex_unlock(ring->lock);

	schedule_flush_if_needed(ring);
}

int ram_log_ring_sync(struct ram_log_ring *ring)
{
	return ring->cfg->storage_sync();
}

int ram_log_ring_get_count(struct ram_log_ring *ring, uint16_t *count)
{
	uint32_t total;
	int err;

	if (!count) {
		return -EINVAL;
	}

	err = ring->cfg->storage_get_count(&total);
	if (err) {
		return err;
	}

	k_mutex_lock(ring->lock, K_FOREVER);
	total += ring->count;
	k_mutex_unlock(ring->lock);

	*count = (uint16_t)MIN(total, UINT16_MAX);
	return 0;
}
