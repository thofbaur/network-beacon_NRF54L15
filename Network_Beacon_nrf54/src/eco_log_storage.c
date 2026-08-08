#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "device.h"
#include "eco_log.h"
#include "eco_log_storage.h"

#define ECO_LOG_STORAGE_PARTITION eco_log_storage
#define ECO_LOG_STORAGE_DEVICE \
	FIXED_PARTITION_DEVICE(ECO_LOG_STORAGE_PARTITION)
#define ECO_LOG_STORAGE_OFFSET \
	FIXED_PARTITION_OFFSET(ECO_LOG_STORAGE_PARTITION)
#define ECO_LOG_STORAGE_SIZE \
	FIXED_PARTITION_SIZE(ECO_LOG_STORAGE_PARTITION)

#define ECO_LOG_STORAGE_SECTOR_SIZE 4096U
#define ECO_LOG_STORAGE_TOTAL_SIZE CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES
#define ECO_LOG_STORAGE_RESERVED_SECTORS 1U
#define ECO_LOG_STORAGE_BLOCKS_PER_SECTOR 4U
#define ECO_LOG_STORAGE_META_ID 1U
#define ECO_LOG_STORAGE_BLOCK_ID_BASE 0x100U
#define ECO_LOG_STORAGE_BLOCK_COUNT \
	(((ECO_LOG_STORAGE_TOTAL_SIZE / ECO_LOG_STORAGE_SECTOR_SIZE) - \
	  ECO_LOG_STORAGE_RESERVED_SECTORS) * \
	 ECO_LOG_STORAGE_BLOCKS_PER_SECTOR)
#define ECO_LOG_STORAGE_MAGIC 0x44534145U
#define ECO_LOG_STORAGE_VERSION 1U

struct eco_log_storage_meta {
	uint32_t magic;
	uint16_t version;
	uint32_t partition_size;
	uint16_t pending_blocks;
	uint32_t pending_entries;
	uint32_t oldest_seq;
	uint32_t next_seq;
	uint16_t oldest_offset;
};

struct eco_log_storage_block {
	uint32_t magic;
	uint16_t version;
	uint16_t entry_count;
	uint32_t sequence;
	uint8_t entries[ECO_LOG_STORAGE_BLOCK_ENTRIES *
			ECO_LOG_ENTRY_SIZE];
} __packed;

BUILD_ASSERT(ECO_LOG_STORAGE_SIZE == ECO_LOG_STORAGE_TOTAL_SIZE,
	     "Eco log NVS partition size mismatch");
BUILD_ASSERT((ECO_LOG_STORAGE_TOTAL_SIZE %
	      ECO_LOG_STORAGE_SECTOR_SIZE) == 0,
	     "Eco log NVS partition must contain whole sectors");
BUILD_ASSERT(ECO_LOG_STORAGE_TOTAL_SIZE >=
	     (2U * ECO_LOG_STORAGE_SECTOR_SIZE),
	     "Eco log NVS partition must contain at least two sectors");
BUILD_ASSERT(ECO_LOG_STORAGE_BLOCK_COUNT > 0,
	     "Eco log flash must contain at least one flush batch");

static struct nvs_fs eco_fs;
static struct eco_log_storage_meta meta;
static struct eco_log_storage_block block_cache;
static bool initialized;
static bool meta_dirty;
static K_MUTEX_DEFINE(storage_lock);

/*
 * Init-failure diagnostics captured for a deferred report. Boot-time RTT
 * output is congested enough that printk() calls issued during early init
 * (this runs before Bluetooth is up) can be silently dropped at the
 * transport layer, with no "messages dropped" notice the way the deferred
 * LOG subsystem gives - unlike printk() calls later in boot, which have
 * reliably shown up in every capture so far. Recording the values and
 * printing them once, a few seconds after boot, avoids losing them.
 */
struct eco_log_storage_diag {
	int device_ready_err;
	int page_info_err;
	bool page_size_mismatch;
	uint32_t page_size;
	int mount_err;
	bool mount_retried;
	int mount_retry_err;
	int meta_read_result;
	int recover_meta_err;
	int uncommitted_err;
	int final_err;
};

static struct eco_log_storage_diag last_init_diag;
static bool diag_report_scheduled;

static void diag_report_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	printk("Eco log NVM init diagnostics: device_err=%d page_info_err=%d page_mismatch=%d page_size=%u mount_err=%d retried=%d retry_err=%d meta_read=%d recover_meta_err=%d uncommitted_err=%d final_err=%d\n",
	       last_init_diag.device_ready_err, last_init_diag.page_info_err,
	       last_init_diag.page_size_mismatch,
	       (unsigned int)last_init_diag.page_size,
	       last_init_diag.mount_err, last_init_diag.mount_retried,
	       last_init_diag.mount_retry_err, last_init_diag.meta_read_result,
	       last_init_diag.recover_meta_err, last_init_diag.uncommitted_err,
	       last_init_diag.final_err);
}

static K_WORK_DELAYABLE_DEFINE(diag_report_work, diag_report_handler);

static uint16_t block_id(uint32_t sequence)
{
	return ECO_LOG_STORAGE_BLOCK_ID_BASE +
	       (uint16_t)(sequence % ECO_LOG_STORAGE_BLOCK_COUNT);
}

static void reset_meta(void)
{
	meta.magic = ECO_LOG_STORAGE_MAGIC;
	meta.version = ECO_LOG_STORAGE_VERSION;
	meta.partition_size = ECO_LOG_STORAGE_TOTAL_SIZE;
	meta.pending_blocks = 0;
	meta.pending_entries = 0;
	meta.oldest_seq = 0;
	meta.next_seq = 0;
	meta.oldest_offset = 0;
}

static int save_meta(void)
{
	ssize_t written = nvs_write(&eco_fs, ECO_LOG_STORAGE_META_ID,
				    &meta, sizeof(meta));

	if (written < 0) {
		printk("Eco log NVM meta write failed (err %d)\n", (int)written);
		device_set_storage_fault(STORAGE_FAULT_ECO_LOG_META, true);
		return (int)written;
	}
	if (written != 0 && written != sizeof(meta)) {
		device_set_storage_fault(STORAGE_FAULT_ECO_LOG_META, true);
		return -EIO;
	}

	device_set_storage_fault(STORAGE_FAULT_ECO_LOG_META, false);
	meta_dirty = false;
	return 0;
}

static int reset_storage(void)
{
	int err = nvs_clear(&eco_fs);

	if (err) {
		return err;
	}

	reset_meta();
	return save_meta();
}

static bool block_record_valid(const struct eco_log_storage_block *block,
			       ssize_t read)
{
	return read == sizeof(*block) &&
	       block->magic == ECO_LOG_STORAGE_MAGIC &&
	       block->version == ECO_LOG_STORAGE_VERSION &&
	       block->entry_count > 0 &&
	       block->entry_count <= ECO_LOG_STORAGE_BLOCK_ENTRIES;
}

static int load_block(uint32_t sequence)
{
	ssize_t read;

	read = nvs_read(&eco_fs, block_id(sequence), &block_cache,
			sizeof(block_cache));
	if (read < 0) {
		device_set_storage_fault(STORAGE_FAULT_ECO_LOG_READ,
					 read != -ENOENT);
		return (int)read;
	}
	if (block_cache.sequence != sequence ||
	    !block_record_valid(&block_cache, read)) {
		device_set_storage_fault(STORAGE_FAULT_ECO_LOG_READ, true);
		return -EINVAL;
	}

	device_set_storage_fault(STORAGE_FAULT_ECO_LOG_READ, false);
	return 0;
}

static int recover_meta_from_blocks(void)
{
	uint32_t newest_sequence = 0;
	uint32_t pending_entries = 0;
	uint16_t pending_blocks = 0;
	bool found = false;
	ssize_t read;
	int scan_err = 0;

	for (uint16_t i = 0; i < ECO_LOG_STORAGE_BLOCK_COUNT; i++) {
		read = nvs_read(&eco_fs,
				ECO_LOG_STORAGE_BLOCK_ID_BASE + i,
				&block_cache, sizeof(block_cache));
		if (read < 0) {
			if (read != -ENOENT && scan_err == 0) {
				scan_err = (int)read;
			}
			continue;
		}
		if (!block_record_valid(&block_cache, read) ||
		    block_id(block_cache.sequence) !=
			    ECO_LOG_STORAGE_BLOCK_ID_BASE + i) {
			continue;
		}

		if (!found ||
		    (int32_t)(block_cache.sequence - newest_sequence) > 0) {
			newest_sequence = block_cache.sequence;
			found = true;
		}
	}

	/* Never replace metadata using a partial scan caused by an I/O error. */
	if (scan_err) {
		return scan_err;
	}

	reset_meta();
	if (!found) {
		return save_meta();
	}

	while (pending_blocks < ECO_LOG_STORAGE_BLOCK_COUNT) {
		uint32_t sequence = newest_sequence - pending_blocks;

		read = nvs_read(&eco_fs, block_id(sequence), &block_cache,
				sizeof(block_cache));
		if (read < 0 || block_cache.sequence != sequence ||
		    !block_record_valid(&block_cache, read)) {
			break;
		}

		pending_entries += block_cache.entry_count;
		pending_blocks++;
	}

	if (pending_blocks == 0) {
		reset_meta();
		return save_meta();
	}

	meta.pending_blocks = pending_blocks;
	meta.pending_entries = pending_entries;
	meta.oldest_seq = newest_sequence - pending_blocks + 1U;
	meta.next_seq = newest_sequence + 1U;
	meta.oldest_offset = 0;

	printk("Recovered %u eco log NVM block(s), %u entrie(s)\n",
	       pending_blocks, (unsigned int)pending_entries);
	return save_meta();
}

static int recover_uncommitted_blocks(void)
{
	bool recovered = false;
	int err;

	while (meta.pending_blocks < ECO_LOG_STORAGE_BLOCK_COUNT) {
		err = load_block(meta.next_seq);
		if (err == -ENOENT || err == -EINVAL) {
			device_set_storage_fault(STORAGE_FAULT_ECO_LOG_READ,
						 false);
			break;
		}
		if (err) {
			return err;
		}

		if (meta.pending_blocks == 0) {
			meta.oldest_seq = meta.next_seq;
			meta.oldest_offset = 0;
		}
		meta.next_seq++;
		meta.pending_blocks++;
		meta.pending_entries += block_cache.entry_count;
		recovered = true;
	}

	if (!recovered) {
		return 0;
	}

	printk("Recovered uncommitted eco log NVM block(s); %u entrie(s) pending\n",
	       (unsigned int)meta.pending_entries);
	return save_meta();
}

int eco_log_storage_init(void)
{
	struct flash_pages_info info;
	ssize_t read;
	int err = 0;

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (initialized) {
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	memset(&last_init_diag, 0, sizeof(last_init_diag));

	eco_fs.flash_device = ECO_LOG_STORAGE_DEVICE;
	if (!device_is_ready(eco_fs.flash_device)) {
		err = -ENODEV;
		last_init_diag.device_ready_err = err;
		goto out;
	}

	eco_fs.offset = ECO_LOG_STORAGE_OFFSET;
	err = flash_get_page_info_by_offs(eco_fs.flash_device,
					  eco_fs.offset, &info);
	last_init_diag.page_info_err = err;
	if (err) {
		goto out;
	}
	if (info.size != ECO_LOG_STORAGE_SECTOR_SIZE) {
		err = -ENOTSUP;
		last_init_diag.page_size_mismatch = true;
		last_init_diag.page_size = info.size;
		goto out;
	}

	eco_fs.sector_size = info.size;
	eco_fs.sector_count = ECO_LOG_STORAGE_SIZE / info.size;
	err = nvs_mount(&eco_fs);
	last_init_diag.mount_err = err;
	if (err) {
		/* The partition may hold stale data left over from before this
		 * partition existed (e.g. a layout change without a full chip
		 * erase). Erase it once and retry before giving up.
		 */
		last_init_diag.mount_retried = true;
		if (flash_erase(eco_fs.flash_device, eco_fs.offset,
				ECO_LOG_STORAGE_SIZE) == 0) {
			err = nvs_mount(&eco_fs);
		}
		last_init_diag.mount_retry_err = err;
		if (err) {
			goto out;
		}
	}

	read = nvs_read(&eco_fs, ECO_LOG_STORAGE_META_ID, &meta,
			sizeof(meta));
	last_init_diag.meta_read_result = (int)read;
	if (read == -ENOENT) {
		err = recover_meta_from_blocks();
		last_init_diag.recover_meta_err = err;
	} else if (read < 0) {
		err = (int)read;
	} else if (read != sizeof(meta) ||
		   meta.magic != ECO_LOG_STORAGE_MAGIC ||
		   meta.version != ECO_LOG_STORAGE_VERSION ||
		   meta.partition_size != ECO_LOG_STORAGE_TOTAL_SIZE) {
		printk("Incompatible eco log NVM format or capacity; clearing stored entries\n");
		err = reset_storage();
	} else if (meta.pending_blocks > ECO_LOG_STORAGE_BLOCK_COUNT ||
		   meta.pending_entries >
			   ((uint32_t)meta.pending_blocks *
			    CONFIG_DSA_ECO_LOG_FLUSH_BATCH) ||
		   meta.oldest_offset >= ECO_LOG_STORAGE_BLOCK_ENTRIES ||
		   (meta.pending_blocks == 0 &&
		    (meta.pending_entries != 0 || meta.oldest_offset != 0))) {
		printk("Invalid eco log NVM metadata; clearing stored entries\n");
		err = reset_storage();
	}

	if (!err) {
		err = recover_uncommitted_blocks();
		last_init_diag.uncommitted_err = err;
	}

	if (!err) {
		initialized = true;
	}
out:
	device_set_storage_fault(STORAGE_FAULT_ECO_LOG_INIT, err != 0);
	if (err) {
		last_init_diag.final_err = err;
		if (!diag_report_scheduled) {
			diag_report_scheduled = true;
			k_work_schedule(&diag_report_work, K_SECONDS(3));
		}
	}
	k_mutex_unlock(&storage_lock);
	return err;
}

static int discard_oldest_block(void)
{
	struct eco_log_storage_meta previous_meta;
	uint32_t discarded_entries;
	uint32_t retired_sequence;
	int err;

	if (meta.pending_blocks == 0) {
		return 0;
	}

	err = load_block(meta.oldest_seq);
	if (err) {
		return err;
	}

	previous_meta = meta;
	retired_sequence = meta.oldest_seq;
	discarded_entries = block_cache.entry_count - meta.oldest_offset;
	meta.oldest_seq++;
	meta.pending_blocks--;
	meta.oldest_offset = 0;
	meta.pending_entries -= MIN(meta.pending_entries, discarded_entries);
	if (meta.pending_blocks == 0) {
		meta.oldest_seq = meta.next_seq;
	}

	err = save_meta();
	if (err) {
		meta = previous_meta;
		return err;
	}

	err = nvs_delete(&eco_fs, block_id(retired_sequence));
	if (err && err != -ENOENT) {
		printk("Failed to delete overwritten eco log block %u (err %d)\n",
		       (unsigned int)retired_sequence, err);
		device_set_storage_fault(STORAGE_FAULT_ECO_LOG_DELETE, true);
		return 0;
	}

	device_set_storage_fault(STORAGE_FAULT_ECO_LOG_DELETE, false);
	return 0;
}

int eco_log_storage_append(const uint8_t *entries, uint16_t entry_count)
{
	struct eco_log_storage_meta previous_meta;
	bool previous_meta_dirty;
	uint32_t sequence;
	ssize_t written;
	int err;

	if (!entries ||
	    entry_count != CONFIG_DSA_ECO_LOG_FLUSH_BATCH) {
		return -EINVAL;
	}

	err = eco_log_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (meta.pending_blocks >= ECO_LOG_STORAGE_BLOCK_COUNT) {
		err = discard_oldest_block();
		if (err) {
			k_mutex_unlock(&storage_lock);
			return err;
		}
	}

	previous_meta = meta;
	previous_meta_dirty = meta_dirty;
	sequence = meta.next_seq;
	memset(&block_cache, 0, sizeof(block_cache));
	block_cache.magic = ECO_LOG_STORAGE_MAGIC;
	block_cache.version = ECO_LOG_STORAGE_VERSION;
	block_cache.entry_count = entry_count;
	block_cache.sequence = sequence;
	memcpy(block_cache.entries, entries,
	       entry_count * ECO_LOG_ENTRY_SIZE);

	written = nvs_write(&eco_fs, block_id(sequence), &block_cache,
			    sizeof(block_cache));
	if (written < 0) {
		err = (int)written;
		goto out;
	}
	if (written != 0 && written != sizeof(block_cache)) {
		err = -EIO;
		goto out;
	}

	if (meta.pending_blocks == 0) {
		meta.oldest_seq = sequence;
		meta.oldest_offset = 0;
	}
	meta.next_seq++;
	meta.pending_blocks++;
	meta.pending_entries += entry_count;

	err = save_meta();
	if (err) {
		meta = previous_meta;
		meta_dirty = previous_meta_dirty;
	}
out:
	device_set_storage_fault(STORAGE_FAULT_ECO_LOG_WRITE, err != 0);
	k_mutex_unlock(&storage_lock);
	return err;
}

int eco_log_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			 uint16_t *bytes_written)
{
	uint16_t available;
	uint16_t entry_count;
	int err;

	if (!buffer || !bytes_written) {
		return -EINVAL;
	}
	*bytes_written = 0;
	if (buffer_len < ECO_LOG_ENTRY_SIZE) {
		return 0;
	}

	err = eco_log_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (meta.pending_blocks == 0) {
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	err = load_block(meta.oldest_seq);
	if (err) {
		k_mutex_unlock(&storage_lock);
		return err;
	}

	available = block_cache.entry_count - meta.oldest_offset;
	entry_count = MIN(available,
			  (uint16_t)(buffer_len / ECO_LOG_ENTRY_SIZE));
	memcpy(buffer,
	       &block_cache.entries[meta.oldest_offset *
				    ECO_LOG_ENTRY_SIZE],
	       entry_count * ECO_LOG_ENTRY_SIZE);
	*bytes_written = entry_count * ECO_LOG_ENTRY_SIZE;
	k_mutex_unlock(&storage_lock);
	return 0;
}

int eco_log_storage_drop(uint16_t entry_count, bool *block_retired)
{
	struct eco_log_storage_meta previous_meta;
	bool previous_meta_dirty;
	uint16_t remaining;
	uint32_t retired_sequence;
	int err;

	if (!block_retired) {
		return -EINVAL;
	}
	*block_retired = false;
	if (entry_count == 0) {
		return 0;
	}

	err = eco_log_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (meta.pending_blocks == 0) {
		err = -ENOENT;
		goto out;
	}

	err = load_block(meta.oldest_seq);
	if (err) {
		goto out;
	}

	remaining = block_cache.entry_count - meta.oldest_offset;
	if (entry_count > remaining) {
		err = -EINVAL;
		goto out;
	}

	previous_meta = meta;
	previous_meta_dirty = meta_dirty;
	meta.pending_entries -= entry_count;
	if (entry_count == remaining) {
		retired_sequence = meta.oldest_seq;
		meta.oldest_seq++;
		meta.pending_blocks--;
		meta.oldest_offset = 0;
		if (meta.pending_blocks == 0) {
			meta.oldest_seq = meta.next_seq;
		}
		meta_dirty = true;

		/* Commit the new queue head before deleting a retired block. */
		err = save_meta();
		if (err) {
			meta = previous_meta;
			meta_dirty = previous_meta_dirty;
			goto out;
		}
	} else {
		meta.oldest_offset += entry_count;
		retired_sequence = UINT32_MAX;
		meta_dirty = true;
	}

	if (retired_sequence != UINT32_MAX) {
		err = nvs_delete(&eco_fs, block_id(retired_sequence));
		if (err && err != -ENOENT) {
			printk("Failed to delete retired eco log block %u (err %d)\n",
			       (unsigned int)retired_sequence, err);
			device_set_storage_fault(
				STORAGE_FAULT_ECO_LOG_DELETE, true);
		} else {
			device_set_storage_fault(
				STORAGE_FAULT_ECO_LOG_DELETE, false);
		}
		/* Metadata is already committed; deletion is cleanup only. */
		*block_retired = true;
		err = 0;
	}
out:
	k_mutex_unlock(&storage_lock);
	return err;
}

int eco_log_storage_sync(void)
{
	int err;

	err = eco_log_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (meta_dirty) {
		err = save_meta();
	} else {
		err = 0;
	}
	k_mutex_unlock(&storage_lock);
	return err;
}

int eco_log_storage_get_count(uint32_t *count)
{
	int err;

	if (!count) {
		return -EINVAL;
	}
	*count = 0;

	err = eco_log_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	*count = meta.pending_entries;
	k_mutex_unlock(&storage_lock);
	return 0;
}
