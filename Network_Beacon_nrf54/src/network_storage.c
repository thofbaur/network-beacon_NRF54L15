#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "network_storage.h"
#include "param_storage.h"

#define NETWORK_STORAGE_META_KEY "dsa/contact/meta"
#define NETWORK_STORAGE_BLOCK_KEY_FMT "dsa/contact/b%02u"
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
};

static struct network_storage_meta meta;
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

static void block_key(char *key, size_t key_len, uint32_t sequence)
{
	snprintk(key, key_len, NETWORK_STORAGE_BLOCK_KEY_FMT,
		 (unsigned int)(sequence % NETWORK_STORAGE_BLOCK_COUNT));
}

static int save_meta(void)
{
	return param_storage_save(NETWORK_STORAGE_META_KEY, &meta, sizeof(meta));
}

static int load_block(uint32_t sequence, struct network_storage_block *block)
{
	char key[sizeof(NETWORK_STORAGE_BLOCK_KEY_FMT) + 2];
	int err;

	block_key(key, sizeof(key), sequence);
	err = param_storage_load(key, block, sizeof(*block));
	if (err) {
		return err;
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
	char key[sizeof(NETWORK_STORAGE_BLOCK_KEY_FMT) + 2];

	block_key(key, sizeof(key), sequence);
	return param_storage_delete(key);
}

int network_storage_init(void)
{
	int err;

	k_mutex_lock(&storage_lock, K_FOREVER);

	if (initialized) {
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	err = param_storage_load(NETWORK_STORAGE_META_KEY, &meta, sizeof(meta));
	if (err == -ENOENT) {
		reset_meta();
		err = save_meta();
	} else if (!err &&
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
	struct network_storage_block block = { 0 };
	char key[sizeof(NETWORK_STORAGE_BLOCK_KEY_FMT) + 2];
	uint32_t sequence;
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
	block.magic = NETWORK_STORAGE_MAGIC;
	block.version = NETWORK_STORAGE_VERSION;
	block.data_len = len;
	block.sequence = sequence;
	memcpy(block.data, data, len);

	block_key(key, sizeof(key), sequence);
	err = param_storage_save(key, &block, sizeof(block));
	if (err) {
		k_mutex_unlock(&storage_lock);
		return err;
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
	struct network_storage_block block;
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

	err = load_block(meta.oldest_seq, &block);
	if (err) {
		printk("Failed to read contact NVM block %u (err %d)\n",
		       (unsigned int)meta.oldest_seq, err);
		k_mutex_unlock(&storage_lock);
		return 0;
	}

	while ((meta.oldest_offset + bytes_written) < block.data_len &&
	       (buffer_len - bytes_written) >= NETWORK_STORAGE_CONTACT_SIZE) {
		memcpy(&buffer[bytes_written],
		       &block.data[meta.oldest_offset + bytes_written],
		       NETWORK_STORAGE_CONTACT_SIZE);
		bytes_written += NETWORK_STORAGE_CONTACT_SIZE;
	}

	k_mutex_unlock(&storage_lock);
	return bytes_written;
}

uint16_t network_storage_drop(uint16_t bytes_to_drop)
{
	struct network_storage_block block;
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

		err = load_block(meta.oldest_seq, &block);
		if (err) {
			printk("Failed to drop contact NVM block %u (err %d)\n",
			       (unsigned int)meta.oldest_seq, err);
			break;
		}

		remaining = block.data_len - meta.oldest_offset;
		drop_now = MIN(bytes_to_drop, remaining);
		drop_now -= drop_now % NETWORK_STORAGE_CONTACT_SIZE;
		if (drop_now == 0) {
			break;
		}

		meta.oldest_offset += drop_now;
		bytes_to_drop -= drop_now;
		bytes_dropped += drop_now;

		if (meta.oldest_offset >= block.data_len) {
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
	struct network_storage_block block;
	uint32_t bytes = 0;
	uint32_t seq;

	if (network_storage_init()) {
		return 0;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);

	for (uint16_t i = 0; i < meta.pending_blocks; i++) {
		seq = meta.oldest_seq + i;
		if (!load_block(seq, &block)) {
			bytes += block.data_len;
		}
	}

	if (bytes > UINT16_MAX) {
		bytes = UINT16_MAX;
	}

	k_mutex_unlock(&storage_lock);
	return (uint16_t)bytes;
}
