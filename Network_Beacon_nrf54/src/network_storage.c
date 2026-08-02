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
#define NETWORK_STORAGE_META_VERSION 4U
#define NETWORK_STORAGE_BLOCK_VERSION 3U

struct network_storage_meta {
	uint32_t magic;
	uint16_t version;
	uint16_t pending_blocks;
	uint32_t pending_contacts;
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

BUILD_ASSERT((NETWORK_STORAGE_BLOCK_DATA_LEN % NETWORK_STORAGE_CONTACT_SIZE) == 0,
	     "Contact NVM block payload must fit whole contacts");
BUILD_ASSERT(NETWORK_STORAGE_TOTAL_BYTES == NETWORK_STORAGE_PARTITION_SIZE,
	     "Contact NVM reservation must match contact_storage partition size");
BUILD_ASSERT((NETWORK_STORAGE_TOTAL_BYTES % NETWORK_STORAGE_SECTOR_BYTES) == 0,
	     "Contact NVM reservation must contain whole flash sectors");
BUILD_ASSERT(sizeof(struct network_storage_block) <= NETWORK_STORAGE_BLOCK_BYTES,
	     "Contact NVM block must leave room for NVS bookkeeping");

static struct network_storage_meta meta;
static struct network_storage_block block_cache;
static struct nvs_fs contact_fs;
static bool initialized;
static bool meta_dirty;
static K_MUTEX_DEFINE(storage_lock);

static void reset_meta(void)
{
	meta.magic = NETWORK_STORAGE_MAGIC;
	meta.version = NETWORK_STORAGE_META_VERSION;
	meta.pending_blocks = 0;
	meta.pending_contacts = 0;
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
		device_set_storage_fault(STORAGE_FAULT_CONTACT_META, true);
		return (int)written;
	}
	if (written != 0 && written != sizeof(meta)) {
		device_set_storage_fault(STORAGE_FAULT_CONTACT_META, true);
		return -EIO;
	}

	meta_dirty = false;
	device_set_storage_fault(STORAGE_FAULT_CONTACT_META, false);
	return 0;
}

static bool block_record_valid(const struct network_storage_block *block,
			       ssize_t read)
{
	return read == sizeof(*block) &&
	       block->magic == NETWORK_STORAGE_MAGIC &&
	       block->version == NETWORK_STORAGE_BLOCK_VERSION &&
	       block->data_len > 0 &&
	       block->data_len <= NETWORK_STORAGE_BLOCK_DATA_LEN &&
	       (block->data_len % NETWORK_STORAGE_CONTACT_SIZE) == 0;
}

static int load_block(uint32_t sequence, struct network_storage_block *block)
{
	ssize_t read;

	read = nvs_read(&contact_fs, block_id(sequence), block, sizeof(*block));
	if (read < 0) {
		device_set_storage_fault(STORAGE_FAULT_CONTACT_READ,
					 read != -ENOENT);
		return (int)read;
	}
	if (block->sequence != sequence || !block_record_valid(block, read)) {
		device_set_storage_fault(STORAGE_FAULT_CONTACT_READ, true);
		return -EINVAL;
	}

	device_set_storage_fault(STORAGE_FAULT_CONTACT_READ, false);
	return 0;
}

static int recover_meta_from_blocks(void)
{
	uint32_t newest_sequence = 0;
	uint32_t pending_contacts = 0;
	uint16_t pending_blocks = 0;
	bool found = false;
	ssize_t read;
	int scan_err = 0;

	/* First locate the newest valid sequence stored in any NVS block slot. */
	for (uint16_t i = 0; i < NETWORK_STORAGE_BLOCK_COUNT; i++) {
		read = nvs_read(&contact_fs, NETWORK_STORAGE_BLOCK_ID_BASE + i,
				&block_cache, sizeof(block_cache));
		if (read < 0) {
			if (read != -ENOENT && scan_err == 0) {
				scan_err = (int)read;
			}
			continue;
		}
		if (!block_record_valid(&block_cache, read) ||
		    block_id(block_cache.sequence) !=
			    NETWORK_STORAGE_BLOCK_ID_BASE + i) {
			continue;
		}

		if (!found ||
		    (int32_t)(block_cache.sequence - newest_sequence) > 0) {
			newest_sequence = block_cache.sequence;
			found = true;
		}
	}

	/* Do not replace metadata with an incomplete reconstruction when the
	 * flash device could not be read reliably.
	 */
	if (scan_err) {
		return scan_err;
	}

	reset_meta();
	if (!found) {
		return save_meta();
	}

	/* Walk backwards from the newest block. The first missing or invalid
	 * sequence is the boundary between the live queue and stale NVS data.
	 * The consumed offset cannot be recovered, so the oldest recovered
	 * block is conservatively replayed from its beginning.
	 */
	while (pending_blocks < NETWORK_STORAGE_BLOCK_COUNT) {
		uint32_t sequence = newest_sequence - pending_blocks;

		read = nvs_read(&contact_fs, block_id(sequence), &block_cache,
				sizeof(block_cache));
		if (read < 0 || block_cache.sequence != sequence ||
		    !block_record_valid(&block_cache, read)) {
			break;
		}

		pending_contacts +=
			block_cache.data_len / NETWORK_STORAGE_CONTACT_SIZE;
		pending_blocks++;
	}

	if (pending_blocks == 0) {
		reset_meta();
		return save_meta();
	}

	meta.pending_blocks = pending_blocks;
	meta.pending_contacts = pending_contacts;
	meta.oldest_seq = newest_sequence - pending_blocks + 1U;
	meta.next_seq = newest_sequence + 1U;
	meta.oldest_offset = 0;

	printk("Recovered %u contact NVM block(s), %u contact(s)\n",
	       pending_blocks, (unsigned int)pending_contacts);
	return save_meta();
}

static int recover_uncommitted_blocks(void)
{
	bool recovered = false;
	int err;

	while (meta.pending_blocks < NETWORK_STORAGE_BLOCK_COUNT) {
		err = load_block(meta.next_seq, &block_cache);
		if (err == -ENOENT || err == -EINVAL) {
			device_set_storage_fault(STORAGE_FAULT_CONTACT_READ, false);
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
		meta.pending_contacts +=
			block_cache.data_len / NETWORK_STORAGE_CONTACT_SIZE;
		recovered = true;
	}

	if (!recovered) {
		return 0;
	}

	printk("Recovered uncommitted contact NVM block(s); %u contact(s) pending\n",
	       (unsigned int)meta.pending_contacts);
	return save_meta();
}

static int delete_block(uint32_t sequence)
{
	return nvs_delete(&contact_fs, block_id(sequence));
}

static int reset_storage(void)
{
	int err = nvs_clear(&contact_fs);

	if (err) {
		return err;
	}

	reset_meta();
	return save_meta();
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
		device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, true);
		k_mutex_unlock(&storage_lock);
		return -ENODEV;
	}

	contact_fs.offset = NETWORK_STORAGE_PARTITION_OFFSET;
	err = flash_get_page_info_by_offs(contact_fs.flash_device,
					  contact_fs.offset, &info);
	if (err) {
		printk("Failed to read contact NVM page info (err %d)\n", err);
		device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, true);
		k_mutex_unlock(&storage_lock);
		return err;
	}
	if (info.size != NETWORK_STORAGE_SECTOR_BYTES) {
		printk("Unsupported contact NVM sector size %u (expected %u)\n",
		       (unsigned int)info.size, NETWORK_STORAGE_SECTOR_BYTES);
		device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, true);
		k_mutex_unlock(&storage_lock);
		return -ENOTSUP;
	}

	contact_fs.sector_size = info.size;
	contact_fs.sector_count = NETWORK_STORAGE_PARTITION_SIZE / info.size;

	err = nvs_mount(&contact_fs);
	if (err) {
		printk("Failed to mount contact NVM storage (err %d)\n", err);
		device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, true);
		k_mutex_unlock(&storage_lock);
		return err;
	}

	read = nvs_read(&contact_fs, NETWORK_STORAGE_META_ID, &meta, sizeof(meta));
	if (read == -ENOENT) {
		err = recover_meta_from_blocks();
	} else if (read < 0) {
		err = (int)read;
	} else if (read != sizeof(meta) ||
		   meta.magic != NETWORK_STORAGE_MAGIC ||
		   meta.version != NETWORK_STORAGE_META_VERSION) {
		printk("Incompatible contact NVM format; clearing stored contacts\n");
		err = reset_storage();
	} else if (meta.pending_blocks > NETWORK_STORAGE_BLOCK_COUNT ||
		   meta.pending_contacts >
			   ((uint32_t)meta.pending_blocks *
			    NETWORK_STORAGE_BLOCK_CONTACTS) ||
		   meta.oldest_offset >= NETWORK_STORAGE_BLOCK_DATA_LEN ||
		   (meta.oldest_offset % NETWORK_STORAGE_CONTACT_SIZE) != 0 ||
		   (meta.pending_blocks == 0 &&
		    (meta.pending_contacts != 0 || meta.oldest_offset != 0))) {
		printk("Invalid contact NVM metadata; clearing stored contacts\n");
		err = reset_storage();
	}

	if (!err) {
		err = recover_uncommitted_blocks();
	}

	if (!err) {
		initialized = true;
		device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, false);
	} else {
		device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, true);
	}

	k_mutex_unlock(&storage_lock);
	return err;
}

static int discard_oldest_block(void)
{
	struct network_storage_meta previous_meta;
	uint32_t discarded_contacts;
	uint32_t retired_sequence;
	int err;

	if (meta.pending_blocks == 0) {
		return 0;
	}

	err = load_block(meta.oldest_seq, &block_cache);
	if (err) {
		return err;
	}

	previous_meta = meta;
	retired_sequence = meta.oldest_seq;
	discarded_contacts =
		(block_cache.data_len - meta.oldest_offset) /
		NETWORK_STORAGE_CONTACT_SIZE;
	meta.oldest_seq++;
	meta.pending_blocks--;
	meta.oldest_offset = 0;
	meta.pending_contacts -= MIN(meta.pending_contacts,
				     discarded_contacts);
	if (meta.pending_blocks == 0) {
		meta.oldest_seq = meta.next_seq;
	}

	err = save_meta();
	if (err) {
		meta = previous_meta;
		return err;
	}

	err = delete_block(retired_sequence);
	if (err && err != -ENOENT) {
		printk("Failed to delete overwritten contact NVM block %u (err %d)\n",
		       (unsigned int)retired_sequence, err);
		device_set_storage_fault(STORAGE_FAULT_CONTACT_DELETE, true);
		return 0;
	}

	device_set_storage_fault(STORAGE_FAULT_CONTACT_DELETE, false);
	return 0;
}

int network_storage_append_block(const uint8_t *data, uint16_t len)
{
	struct network_storage_meta previous_meta;
	bool previous_meta_dirty;
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
	block_cache.magic = NETWORK_STORAGE_MAGIC;
	block_cache.version = NETWORK_STORAGE_BLOCK_VERSION;
	block_cache.data_len = len;
	block_cache.sequence = sequence;
	memcpy(block_cache.data, data, len);

	written = nvs_write(&contact_fs, block_id(sequence), &block_cache,
			    sizeof(block_cache));
	if (written < 0) {
		device_set_storage_fault(STORAGE_FAULT_CONTACT_WRITE, true);
		k_mutex_unlock(&storage_lock);
		return (int)written;
	}
	if (written != 0 && written != sizeof(block_cache)) {
		device_set_storage_fault(STORAGE_FAULT_CONTACT_WRITE, true);
		k_mutex_unlock(&storage_lock);
		return -EIO;
	}

	if (meta.pending_blocks == 0) {
		meta.oldest_seq = sequence;
		meta.oldest_offset = 0;
	}
	meta.next_seq++;
	meta.pending_blocks++;
	meta.pending_contacts += len / NETWORK_STORAGE_CONTACT_SIZE;

	err = save_meta();
	if (err) {
		/* The block write is an uncommitted orphan. Restore the queue
		 * state so retrying uses the same sequence and cannot duplicate
		 * these contacts in the logical queue.
		 */
		meta = previous_meta;
		meta_dirty = previous_meta_dirty;
		printk("Failed to save contact NVM metadata after block write (err %d)\n", err);
	} else {
		device_set_storage_fault(STORAGE_FAULT_CONTACT_WRITE, false);
	}

	k_mutex_unlock(&storage_lock);
	return err;
}

int network_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			 uint16_t *bytes_written)
{
	uint16_t written = 0;
	int err;

	if (!buffer || !bytes_written) {
		return -EINVAL;
	}
	*bytes_written = 0;

	err = network_storage_init();
	if (err) {
		return err;
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
		return err;
	}

	while ((meta.oldest_offset + written) < block_cache.data_len &&
	       (buffer_len - written) >= NETWORK_STORAGE_CONTACT_SIZE) {
		memcpy(&buffer[written],
		       &block_cache.data[meta.oldest_offset + written],
		       NETWORK_STORAGE_CONTACT_SIZE);
		written += NETWORK_STORAGE_CONTACT_SIZE;
	}

	*bytes_written = written;
	k_mutex_unlock(&storage_lock);
	return 0;
}

int network_storage_drop(uint16_t bytes_to_drop)
{
	struct network_storage_meta previous_meta;
	bool previous_meta_dirty;
	uint16_t remaining;
	uint32_t contacts_dropped;
	uint32_t retired_sequence;
	int err;

	bytes_to_drop -= bytes_to_drop % NETWORK_STORAGE_CONTACT_SIZE;
	if (bytes_to_drop == 0) {
		return 0;
	}

	err = network_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);

	if (meta.pending_blocks == 0) {
		err = -ENOENT;
		goto out;
	}

	err = load_block(meta.oldest_seq, &block_cache);
	if (err) {
		printk("Failed to drop contact NVM block %u (err %d)\n",
		       (unsigned int)meta.oldest_seq, err);
		goto out;
	}

	remaining = block_cache.data_len - meta.oldest_offset;
	if (bytes_to_drop > remaining) {
		err = -EINVAL;
		goto out;
	}

	contacts_dropped = bytes_to_drop / NETWORK_STORAGE_CONTACT_SIZE;

	if (bytes_to_drop == remaining) {
		/* Commit the new queue head before deleting the retired block.
		 * A reset can therefore cause an orphaned block, but can never
		 * leave metadata pointing at a block that was already erased.
		 */
		previous_meta = meta;
		previous_meta_dirty = meta_dirty;
		retired_sequence = meta.oldest_seq;
		meta.oldest_seq++;
		meta.pending_blocks--;
		meta.oldest_offset = 0;
		if (meta.pending_blocks == 0) {
			meta.oldest_seq = meta.next_seq;
		}
		meta.pending_contacts -=
			MIN(meta.pending_contacts, contacts_dropped);
		meta_dirty = true;

		err = save_meta();
		if (err) {
			meta = previous_meta;
			meta_dirty = previous_meta_dirty;
			printk("Failed to save contact NVM metadata before block retirement (err %d)\n",
			       err);
			goto out;
		}

		err = delete_block(retired_sequence);
		if (err && err != -ENOENT) {
			/* The metadata is already durable. Treat deletion as cleanup:
			 * reporting failure to the sender would resend acknowledged
			 * contacts. The orphan is harmless and can be reclaimed later.
			 */
			printk("Failed to delete retired contact NVM block %u (err %d)\n",
			       (unsigned int)retired_sequence, err);
			device_set_storage_fault(STORAGE_FAULT_CONTACT_DELETE, true);
		} else {
			device_set_storage_fault(STORAGE_FAULT_CONTACT_DELETE, false);
		}
		err = 0;
	} else {
		meta.oldest_offset += bytes_to_drop;
		meta.pending_contacts -=
			MIN(meta.pending_contacts, contacts_dropped);
		meta_dirty = true;
	}

	err = 0;
out:
	k_mutex_unlock(&storage_lock);
	return err;
}

int network_storage_get_contact_count(uint32_t *count)
{
	int err;

	if (!count) {
		return -EINVAL;
	}
	*count = 0;

	err = network_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	*count = meta.pending_contacts;
	k_mutex_unlock(&storage_lock);

	return 0;
}

int network_storage_sync(void)
{
	int err = 0;

	err = network_storage_init();
	if (err) {
		return err;
	}

	k_mutex_lock(&storage_lock, K_FOREVER);
	if (meta_dirty) {
		err = save_meta();
	}
	k_mutex_unlock(&storage_lock);

	return err;
}
