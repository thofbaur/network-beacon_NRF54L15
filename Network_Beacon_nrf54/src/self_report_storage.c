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
#include "self_report.h"
#include "self_report_storage.h"

#define SELF_REPORT_STORAGE_PARTITION self_report_storage
#define SELF_REPORT_STORAGE_DEVICE \
	FIXED_PARTITION_DEVICE(SELF_REPORT_STORAGE_PARTITION)
#define SELF_REPORT_STORAGE_OFFSET \
	FIXED_PARTITION_OFFSET(SELF_REPORT_STORAGE_PARTITION)
#define SELF_REPORT_STORAGE_SIZE \
	FIXED_PARTITION_SIZE(SELF_REPORT_STORAGE_PARTITION)

#define SELF_REPORT_STORAGE_SECTOR_SIZE 4096U
#define SELF_REPORT_STORAGE_TOTAL_SIZE CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES
#define SELF_REPORT_STORAGE_RESERVED_SECTORS 1U
#define SELF_REPORT_STORAGE_BLOCKS_PER_SECTOR 4U
#define SELF_REPORT_STORAGE_META_ID 1U
#define SELF_REPORT_STORAGE_BLOCK_ID_BASE 0x100U
#define SELF_REPORT_STORAGE_BLOCK_COUNT \
	(((SELF_REPORT_STORAGE_TOTAL_SIZE / SELF_REPORT_STORAGE_SECTOR_SIZE) - \
	  SELF_REPORT_STORAGE_RESERVED_SECTORS) * \
	 SELF_REPORT_STORAGE_BLOCKS_PER_SECTOR)
#define SELF_REPORT_STORAGE_MAGIC 0x44534152U
#define SELF_REPORT_STORAGE_VERSION 3U

struct self_report_storage_meta {
	uint32_t magic;
	uint16_t version;
	uint32_t partition_size;
	uint16_t pending_blocks;
	uint32_t pending_reports;
	uint32_t oldest_seq;
	uint32_t next_seq;
	uint16_t oldest_offset;
};

struct self_report_storage_block {
	uint32_t magic;
	uint16_t version;
	uint16_t report_count;
	uint32_t sequence;
	uint8_t reports[SELF_REPORT_STORAGE_BLOCK_ENTRIES *
			SELF_REPORT_ENTRY_SIZE];
} __packed;

BUILD_ASSERT(SELF_REPORT_STORAGE_SIZE == SELF_REPORT_STORAGE_TOTAL_SIZE,
	     "Self-report NVS partition size mismatch");
BUILD_ASSERT((SELF_REPORT_STORAGE_TOTAL_SIZE %
	      SELF_REPORT_STORAGE_SECTOR_SIZE) == 0,
	     "Self-report NVS partition must contain whole sectors");
BUILD_ASSERT(SELF_REPORT_STORAGE_TOTAL_SIZE >=
	     (2U * SELF_REPORT_STORAGE_SECTOR_SIZE),
	     "Self-report NVS partition must contain at least two sectors");
BUILD_ASSERT(SELF_REPORT_STORAGE_BLOCK_COUNT > 0,
	     "Self-report flash must contain at least one flush batch");

static struct nvs_fs report_fs;
static struct self_report_storage_meta meta;
static struct self_report_storage_block block_cache;
static bool initialized;
static K_MUTEX_DEFINE(storage_lock);

static uint16_t block_id(uint32_t sequence)
{
	return SELF_REPORT_STORAGE_BLOCK_ID_BASE +
	       (uint16_t)(sequence % SELF_REPORT_STORAGE_BLOCK_COUNT);
}

static void reset_meta(void)
{
	meta.magic = SELF_REPORT_STORAGE_MAGIC;
	meta.version = SELF_REPORT_STORAGE_VERSION;
	meta.partition_size = SELF_REPORT_STORAGE_TOTAL_SIZE;
	meta.pending_blocks = 0;
	meta.pending_reports = 0;
	meta.oldest_seq = 0;
	meta.next_seq = 0;
	meta.oldest_offset = 0;
}

static int save_meta(void)
{
	ssize_t written = nvs_write(&report_fs, SELF_REPORT_STORAGE_META_ID,
				    &meta, sizeof(meta));

	if (written < 0) {
		device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_META, true);
		return (int)written;
	}
	if (written != 0 && written != sizeof(meta)) {
		device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_META, true);
		return -EIO;
	}

	device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_META, false);
	return 0;
}

static int reset_storage(void)
{
	int err = nvs_clear(&report_fs);

	if (err) {
		return err;
	}

	reset_meta();
	return save_meta();
}

static bool block_record_valid(const struct self_report_storage_block *block,
			       ssize_t read)
{
	return read == sizeof(*block) &&
	       block->magic == SELF_REPORT_STORAGE_MAGIC &&
	       block->version == SELF_REPORT_STORAGE_VERSION &&
	       block->report_count > 0 &&
	       block->report_count <= SELF_REPORT_STORAGE_BLOCK_ENTRIES;
}

static int load_block(uint32_t sequence)
{
	ssize_t read;

	read = nvs_read(&report_fs, block_id(sequence), &block_cache,
			sizeof(block_cache));
	if (read < 0) {
		device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_READ,
					 read != -ENOENT);
		return (int)read;
	}
	if (block_cache.sequence != sequence ||
	    !block_record_valid(&block_cache, read)) {
		device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_READ, true);
		return -EINVAL;
	}

	device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_READ, false);
	return 0;
}

static int recover_meta_from_blocks(void)
{
	uint32_t newest_sequence = 0;
	uint32_t pending_reports = 0;
	uint16_t pending_blocks = 0;
	bool found = false;
	ssize_t read;
	int scan_err = 0;

	for (uint16_t i = 0; i < SELF_REPORT_STORAGE_BLOCK_COUNT; i++) {
		read = nvs_read(&report_fs,
				SELF_REPORT_STORAGE_BLOCK_ID_BASE + i,
				&block_cache, sizeof(block_cache));
		if (read < 0) {
			if (read != -ENOENT && scan_err == 0) {
				scan_err = (int)read;
			}
			continue;
		}
		if (!block_record_valid(&block_cache, read) ||
		    block_id(block_cache.sequence) !=
			    SELF_REPORT_STORAGE_BLOCK_ID_BASE + i) {
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

	while (pending_blocks < SELF_REPORT_STORAGE_BLOCK_COUNT) {
		uint32_t sequence = newest_sequence - pending_blocks;

		read = nvs_read(&report_fs, block_id(sequence), &block_cache,
				sizeof(block_cache));
		if (read < 0 || block_cache.sequence != sequence ||
		    !block_record_valid(&block_cache, read)) {
			break;
		}

		pending_reports += block_cache.report_count;
		pending_blocks++;
	}

	if (pending_blocks == 0) {
		reset_meta();
		return save_meta();
	}

	meta.pending_blocks = pending_blocks;
	meta.pending_reports = pending_reports;
	meta.oldest_seq = newest_sequence - pending_blocks + 1U;
	meta.next_seq = newest_sequence + 1U;
	meta.oldest_offset = 0;

	printk("Recovered %u self-report NVM block(s), %u report(s)\n",
	       pending_blocks, (unsigned int)pending_reports);
	return save_meta();
}

static int recover_uncommitted_blocks(void)
{
	bool recovered = false;
	int err;

	while (meta.pending_blocks < SELF_REPORT_STORAGE_BLOCK_COUNT) {
		err = load_block(meta.next_seq);
		if (err == -ENOENT || err == -EINVAL) {
			device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_READ,
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
		meta.pending_reports += block_cache.report_count;
		recovered = true;
	}

	if (!recovered) {
		return 0;
	}

	printk("Recovered uncommitted self-report NVM block(s); %u report(s) pending\n",
	       (unsigned int)meta.pending_reports);
	return save_meta();
}

int self_report_storage_init(void)
{
	struct flash_pages_info info;
	ssize_t read;
	int err = 0;

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (initialized) {
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	report_fs.flash_device = SELF_REPORT_STORAGE_DEVICE;
	if (!device_is_ready(report_fs.flash_device)) {
		err = -ENODEV;
		goto out;
	}

	report_fs.offset = SELF_REPORT_STORAGE_OFFSET;
	err = flash_get_page_info_by_offs(report_fs.flash_device,
					  report_fs.offset, &info);
	if (err) {
		goto out;
	}
	if (info.size != SELF_REPORT_STORAGE_SECTOR_SIZE) {
		err = -ENOTSUP;
		goto out;
	}

	report_fs.sector_size = info.size;
	report_fs.sector_count = SELF_REPORT_STORAGE_SIZE / info.size;
	err = nvs_mount(&report_fs);
	if (err) {
		goto out;
	}

	read = nvs_read(&report_fs, SELF_REPORT_STORAGE_META_ID, &meta,
			sizeof(meta));
	if (read == -ENOENT) {
		err = recover_meta_from_blocks();
	} else if (read < 0) {
		err = (int)read;
	} else if (read != sizeof(meta) ||
		   meta.magic != SELF_REPORT_STORAGE_MAGIC ||
		   meta.version != SELF_REPORT_STORAGE_VERSION ||
		   meta.partition_size != SELF_REPORT_STORAGE_TOTAL_SIZE) {
		printk("Incompatible self-report NVM format or capacity; clearing stored reports\n");
		err = reset_storage();
	} else if (meta.pending_blocks > SELF_REPORT_STORAGE_BLOCK_COUNT ||
		   meta.pending_reports >
			   ((uint32_t)meta.pending_blocks *
			    CONFIG_DSA_SELF_REPORT_FLUSH_BATCH) ||
		   meta.oldest_offset >= SELF_REPORT_STORAGE_BLOCK_ENTRIES ||
		   (meta.pending_blocks == 0 &&
		    (meta.pending_reports != 0 || meta.oldest_offset != 0))) {
		printk("Invalid self-report NVM metadata; clearing stored reports\n");
		err = reset_storage();
	}

	if (!err) {
		err = recover_uncommitted_blocks();
	}

	if (!err) {
		initialized = true;
	}
out:
	device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_INIT, err != 0);
	if (err) {
		printk("Self-report NVM initialization failed (err %d)\n", err);
	}
	k_mutex_unlock(&storage_lock);
	return err;
}

static int discard_oldest_block(void)
{
	struct self_report_storage_meta previous_meta;
	uint32_t discarded_reports;
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
	discarded_reports = block_cache.report_count - meta.oldest_offset;
	meta.oldest_seq++;
	meta.pending_blocks--;
	meta.oldest_offset = 0;
	meta.pending_reports -= MIN(meta.pending_reports, discarded_reports);
	if (meta.pending_blocks == 0) {
		meta.oldest_seq = meta.next_seq;
	}

	err = save_meta();
	if (err) {
		meta = previous_meta;
		return err;
	}

	err = nvs_delete(&report_fs, block_id(retired_sequence));
	if (err && err != -ENOENT) {
		printk("Failed to delete overwritten self-report block %u (err %d)\n",
		       (unsigned int)retired_sequence, err);
		device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_DELETE, true);
		return 0;
	}

	device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_DELETE, false);
	return 0;
}

int self_report_storage_append(const uint8_t *reports, uint16_t report_count)
{
	struct self_report_storage_meta previous_meta;
	uint32_t sequence;
	ssize_t written;
	int err;

	if (!reports ||
	    report_count != CONFIG_DSA_SELF_REPORT_FLUSH_BATCH) {
		return -EINVAL;
	}

	err = self_report_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (meta.pending_blocks >= SELF_REPORT_STORAGE_BLOCK_COUNT) {
		err = discard_oldest_block();
		if (err) {
			k_mutex_unlock(&storage_lock);
			return err;
		}
	}

	previous_meta = meta;
	sequence = meta.next_seq;
	memset(&block_cache, 0, sizeof(block_cache));
	block_cache.magic = SELF_REPORT_STORAGE_MAGIC;
	block_cache.version = SELF_REPORT_STORAGE_VERSION;
	block_cache.report_count = report_count;
	block_cache.sequence = sequence;
	memcpy(block_cache.reports, reports,
	       report_count * SELF_REPORT_ENTRY_SIZE);

	written = nvs_write(&report_fs, block_id(sequence), &block_cache,
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
	meta.pending_reports += report_count;

	err = save_meta();
	if (err) {
		meta = previous_meta;
	}
out:
	device_set_storage_fault(STORAGE_FAULT_SELF_REPORT_WRITE, err != 0);
	k_mutex_unlock(&storage_lock);
	return err;
}

int self_report_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			     uint16_t *bytes_written)
{
	uint16_t available;
	uint16_t report_count;
	int err;

	if (!buffer || !bytes_written) {
		return -EINVAL;
	}
	*bytes_written = 0;
	if (buffer_len < SELF_REPORT_ENTRY_SIZE) {
		return 0;
	}

	err = self_report_storage_init();
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

	available = block_cache.report_count - meta.oldest_offset;
	report_count = MIN(available,
			   (uint16_t)(buffer_len / SELF_REPORT_ENTRY_SIZE));
	memcpy(buffer,
	       &block_cache.reports[meta.oldest_offset *
				    SELF_REPORT_ENTRY_SIZE],
	       report_count * SELF_REPORT_ENTRY_SIZE);
	*bytes_written = report_count * SELF_REPORT_ENTRY_SIZE;
	k_mutex_unlock(&storage_lock);
	return 0;
}

int self_report_storage_drop(uint16_t report_count, bool *block_retired)
{
	struct self_report_storage_meta previous_meta;
	uint16_t remaining;
	uint32_t retired_sequence;
	int err;

	if (!block_retired) {
		return -EINVAL;
	}
	*block_retired = false;
	if (report_count == 0) {
		return 0;
	}

	err = self_report_storage_init();
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

	remaining = block_cache.report_count - meta.oldest_offset;
	if (report_count > remaining) {
		err = -EINVAL;
		goto out;
	}

	previous_meta = meta;
	meta.pending_reports -= report_count;
	if (report_count == remaining) {
		retired_sequence = meta.oldest_seq;
		meta.oldest_seq++;
		meta.pending_blocks--;
		meta.oldest_offset = 0;
		if (meta.pending_blocks == 0) {
			meta.oldest_seq = meta.next_seq;
		}
	} else {
		meta.oldest_offset += report_count;
		retired_sequence = UINT32_MAX;
	}

	/* Persist the advanced read position before deleting a retired block. */
	err = save_meta();
	if (err) {
		meta = previous_meta;
		goto out;
	}

	if (retired_sequence != UINT32_MAX) {
		err = nvs_delete(&report_fs, block_id(retired_sequence));
		if (err && err != -ENOENT) {
			printk("Failed to delete retired self-report block %u (err %d)\n",
			       (unsigned int)retired_sequence, err);
			device_set_storage_fault(
				STORAGE_FAULT_SELF_REPORT_DELETE, true);
		} else {
			device_set_storage_fault(
				STORAGE_FAULT_SELF_REPORT_DELETE, false);
		}
		/* Metadata is already committed; deletion is cleanup only. */
		*block_retired = true;
		err = 0;
	}
out:
	k_mutex_unlock(&storage_lock);
	return err;
}

int self_report_storage_get_count(uint32_t *count)
{
	int err;

	if (!count) {
		return -EINVAL;
	}
	*count = 0;

	err = self_report_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	*count = meta.pending_reports;
	k_mutex_unlock(&storage_lock);
	return 0;
}
