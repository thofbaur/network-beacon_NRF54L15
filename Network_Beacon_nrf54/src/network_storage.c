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

#include "network_storage.h"

#define NETWORK_STORAGE_PARTITION contact_storage
#define NETWORK_STORAGE_PARTITION_DEVICE \
	FIXED_PARTITION_DEVICE(NETWORK_STORAGE_PARTITION)
#define NETWORK_STORAGE_PARTITION_OFFSET \
	FIXED_PARTITION_OFFSET(NETWORK_STORAGE_PARTITION)
#define NETWORK_STORAGE_PARTITION_SIZE \
	FIXED_PARTITION_SIZE(NETWORK_STORAGE_PARTITION)

#define NETWORK_STORAGE_META_ID 1U
#define NETWORK_STORAGE_BLOCK_ID_BASE 0x100U
#define NETWORK_STORAGE_MAGIC 0x44534143U
#define NETWORK_STORAGE_VERSION 1U

struct network_storage_meta {
	uint32_t magic;
	uint16_t version;
	uint16_t pending_blocks;
	uint32_t oldest_seq;
	uint32_t next_seq;
	uint16_t oldest_offset;
};

struct network_storage_block {
	uint32_t magic;
	uint16_t version;
	uint16_t data_len;
	uint32_t sequence;
	uint8_t data[NETWORK_STORAGE_BLOCK_DATA_LEN];
} __packed;

BUILD_ASSERT((NETWORK_STORAGE_TOTAL_BYTES % NETWORK_STORAGE_BLOCK_BYTES) == 0,
	     "Contact NVM reservation must be an exact number of blocks");
BUILD_ASSERT((NETWORK_STORAGE_BLOCK_DATA_LEN % NETWORK_STORAGE_CONTACT_SIZE) == 0,
	     "Contact NVM block payload must fit whole contacts");
BUILD_ASSERT(NETWORK_STORAGE_TOTAL_BYTES == NETWORK_STORAGE_PARTITION_SIZE,
	     "Contact NVM reservation must match contact_storage partition size");

static struct network_storage_meta meta;
static struct network_storage_block block_cache;
static struct nvs_fs contact_fs;
static bool initialized;
static K_MUTEX_DEFINE(storage_lock);

static void reset_meta(void)
{
	meta.magic = NETWORK_STORAGE_MAGIC;
	meta.version = NETWORK_STORAGE_VERSION;
	meta.pending_blocks = 0;
	meta.oldest_seq = 0;
	meta.next_seq = 0;
	meta.oldest_offset = 0;
}

static uint16_t block_id(uint32_t sequence)
{
	return NETWORK_STORAGE_BLOCK_ID_BASE +
	       (uint16_t)(sequence % NETWORK_STORAGE_BLOCK_COUNT);
}

static int save_meta(void)
{
	ssize_t written = nvs_write(&contact_fs, NETWORK_STORAGE_META_ID,
				    &meta, sizeof(meta));

	if (written < 0) {
		return (int)written;
	}
	if (written != 0 && written != sizeof(meta)) {
		return -EIO;
	}

	return 0;
}

static int load_block(uint32_t sequence, struct network_storage_block *block)
{
	ssize_t read;

	read = nvs_read(&contact_fs, block_id(sequence), block, sizeof(*block));
	if (read < 0) {
		return (int)read;
	}
	if (read != sizeof(*block)) {
		return -EINVAL;
	}

	if (block->magic != NETWORK_STORAGE_MAGIC ||
	    block->version != NETWORK_STORAGE_VERSION ||
	    block->sequence != sequence ||
	    block->data_len > NETWORK_STORAGE_BLOCK_DATA_LEN ||
	    (block->data_len % NETWORK_STORAGE_CONTACT_SIZE) != 0) {
		return -EINVAL;
	}

	return 0;
}

static int delete_block(uint32_t sequence)
{
	return nvs_delete(&contact_fs, block_id(sequence));
}

int network_storage_init(void)
{
	struct flash_pages_info info;
	ssize_t read;
	int err;

	k_mutex_lock(&storage_lock, K_FOREVER);

	if (initialized) {
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	contact_fs.flash_device = NETWORK_STORAGE_PARTITION_DEVICE;
	if (!device_is_ready(contact_fs.flash_device)) {
		printk("Contact NVM flash device is not ready\n");
		k_mutex_unlock(&storage_lock);
		return -ENODEV;
	}

	contact_fs.offset = NETWORK_STORAGE_PARTITION_OFFSET;
	err = flash_get_page_info_by_offs(contact_fs.flash_device,
					  contact_fs.offset, &info);
	if (err) {
		printk("Failed to read contact NVM page info (err %d)\n", err);
		k_mutex_unlock(&storage_lock);
		return err;
	}

	contact_fs.sector_size = info.size;
	contact_fs.sector_count = NETWORK_STORAGE_PARTITION_SIZE / info.size;

	err = nvs_mount(&contact_fs);
	if (err) {
		printk("Failed to mount contact NVM storage (err %d)\n", err);
		k_mutex_unlock(&storage_lock);
		return err;
	}

	read = nvs_read(&contact_fs, NETWORK_STORAGE_META_ID, &meta, sizeof(meta));
	if (read == -ENOENT) {
		reset_meta();
		err = save_meta();
	} else if (read < 0) {
		err = (int)read;
	} else if (read != sizeof(meta)) {
		printk("Invalid contact NVM metadata length, resetting queue\n");
		reset_meta();
		err = save_meta();
	} else if (
		   (meta.magic != NETWORK_STORAGE_MAGIC ||
		    meta.version != NETWORK_STORAGE_VERSION ||
		    meta.pending_blocks > NETWORK_STORAGE_BLOCK_COUNT ||
		    meta.oldest_offset >= NETWORK_STORAGE_BLOCK_DATA_LEN ||
		    (meta.oldest_offset % NETWORK_STORAGE_CONTACT_SIZE) != 0)) {
		printk("Invalid contact NVM metadata, resetting queue\n");
		reset_meta();
		err = save_meta();
	}

	if (!err) {
		initialized = true;
	}

	k_mutex_unlock(&storage_lock);
	return err;
}

int network_storage_append_block(const uint8_t *data, uint16_t len)
{
	uint32_t sequence;
	ssize_t written;
	int err;

	if (len == 0 || len > NETWORK_STORAGE_BLOCK_DATA_LEN ||
	    (len % NETWORK_STORAGE_CONTACT_SIZE) != 0) {
		return -EINVAL;
	}

	err = network_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);

	if (meta.pending_blocks >= NETWORK_STORAGE_BLOCK_COUNT) {
		k_mutex_unlock(&storage_lock);
		return -ENOSPC;
	}

	sequence = meta.next_seq;
	memset(&block_cache, 0, sizeof(block_cache));
	block_cache.magic = NETWORK_STORAGE_MAGIC;
	block_cache.version = NETWORK_STORAGE_VERSION;
	block_cache.data_len = len;
	block_cache.sequence = sequence;
	memcpy(block_cache.data, data, len);

	written = nvs_write(&contact_fs, block_id(sequence), &block_cache,
			    sizeof(block_cache));
	if (written < 0) {
		k_mutex_unlock(&storage_lock);
		return (int)written;
	}
	if (written != 0 && written != sizeof(block_cache)) {
		k_mutex_unlock(&storage_lock);
		return -EIO;
	}

	if (meta.pending_blocks == 0) {
		meta.oldest_seq = sequence;
		meta.oldest_offset = 0;
	}
	meta.next_seq++;
	meta.pending_blocks++;

	err = save_meta();
	if (err) {
		printk("Failed to save contact NVM metadata after block write (err %d)\n", err);
	}

	k_mutex_unlock(&storage_lock);
	return err;
}

uint16_t network_storage_peek(uint8_t *buffer, uint16_t buffer_len)
{
	uint16_t bytes_written = 0;
	int err;

	if (network_storage_init()) {
		return 0;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);

	if (meta.pending_blocks == 0) {
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	err = load_block(meta.oldest_seq, &block_cache);
	if (err) {
		printk("Failed to read contact NVM block %u (err %d)\n",
		       (unsigned int)meta.oldest_seq, err);
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	while ((meta.oldest_offset + bytes_written) < block_cache.data_len &&
	       (buffer_len - bytes_written) >= NETWORK_STORAGE_CONTACT_SIZE) {
		memcpy(&buffer[bytes_written],
		       &block_cache.data[meta.oldest_offset + bytes_written],
		       NETWORK_STORAGE_CONTACT_SIZE);
		bytes_written += NETWORK_STORAGE_CONTACT_SIZE;
	}

	k_mutex_unlock(&storage_lock);
	return bytes_written;
}

uint16_t network_storage_drop(uint16_t bytes_to_drop)
{
	uint16_t bytes_dropped = 0;
	int err;

	bytes_to_drop -= bytes_to_drop % NETWORK_STORAGE_CONTACT_SIZE;
	if (bytes_to_drop == 0 || network_storage_init()) {
		return 0;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);

	while (bytes_to_drop > 0 && meta.pending_blocks > 0) {
		uint16_t remaining;
		uint16_t drop_now;

		err = load_block(meta.oldest_seq, &block_cache);
		if (err) {
			printk("Failed to drop contact NVM block %u (err %d)\n",
			       (unsigned int)meta.oldest_seq, err);
			break;
		}

		remaining = block_cache.data_len - meta.oldest_offset;
		drop_now = MIN(bytes_to_drop, remaining);
		drop_now -= drop_now % NETWORK_STORAGE_CONTACT_SIZE;
		if (drop_now == 0) {
			break;
		}

		meta.oldest_offset += drop_now;
		bytes_to_drop -= drop_now;
		bytes_dropped += drop_now;

		if (meta.oldest_offset >= block_cache.data_len) {
			err = delete_block(meta.oldest_seq);
			if (err && err != -ENOENT) {
				printk("Failed to delete sent contact NVM block %u (err %d)\n",
				       (unsigned int)meta.oldest_seq, err);
				break;
			}

			meta.oldest_seq++;
			meta.pending_blocks--;
			meta.oldest_offset = 0;
			if (meta.pending_blocks == 0) {
				meta.oldest_seq = meta.next_seq;
			}
		}
	}

	if (bytes_dropped > 0) {
		err = save_meta();
		if (err) {
			printk("Failed to save contact NVM metadata after drop (err %d)\n", err);
		}
	}

	k_mutex_unlock(&storage_lock);
	return bytes_dropped;
}

uint16_t network_storage_pending_bytes(void)
{
	uint32_t bytes = 0;
	uint32_t seq;

	if (network_storage_init()) {
		return 0;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);

	for (uint16_t i = 0; i < meta.pending_blocks; i++) {
		seq = meta.oldest_seq + i;
		if (!load_block(seq, &block_cache)) {
			bytes += block_cache.data_len;
		}
	}

	if (bytes > UINT16_MAX) {
		bytes = UINT16_MAX;
	}

	k_mutex_unlock(&storage_lock);
	return (uint16_t)bytes;
}
