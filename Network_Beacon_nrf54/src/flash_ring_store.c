#include <errno.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "device.h"
#include "flash_ring_store.h"

static uint16_t block_id(const struct flash_ring_store *store,
			 uint32_t sequence)
{
	return FLASH_RING_STORE_BLOCK_ID_BASE +
	       (uint16_t)(sequence % store->cfg->block_count);
}

static struct flash_ring_store_block_header *
block_header(struct flash_ring_store *store)
{
	return (struct flash_ring_store_block_header *)store->block_cache;
}

static uint8_t *block_entries(struct flash_ring_store *store)
{
	return store->block_cache + sizeof(struct flash_ring_store_block_header);
}

static void reset_meta(struct flash_ring_store *store)
{
	store->meta.magic = store->cfg->magic;
	store->meta.version = store->cfg->version;
	store->meta.partition_size = store->cfg->partition_size;
	store->meta.pending_blocks = 0;
	store->meta.pending_entries = 0;
	store->meta.oldest_seq = 0;
	store->meta.next_seq = 0;
	store->meta.oldest_offset = 0;
}

static int save_meta(struct flash_ring_store *store)
{
	ssize_t written = nvs_write(&store->fs, FLASH_RING_STORE_META_ID,
				    &store->meta, sizeof(store->meta));

	if (written < 0) {
		printk("%s NVM meta write failed (err %d)\n",
		       store->cfg->domain_name, (int)written);
		device_set_storage_fault(store->cfg->storage_fault_meta, true);
		return (int)written;
	}
	if (written != 0 && written != sizeof(store->meta)) {
		device_set_storage_fault(store->cfg->storage_fault_meta, true);
		return -EIO;
	}

	device_set_storage_fault(store->cfg->storage_fault_meta, false);
	store->meta_dirty = false;
	return 0;
}

static int reset_storage(struct flash_ring_store *store)
{
	int err = nvs_clear(&store->fs);

	if (err) {
		return err;
	}

	reset_meta(store);
	return save_meta(store);
}

static bool block_record_valid(const struct flash_ring_store *store,
				const struct flash_ring_store_block_header *header,
				ssize_t read)
{
	return read == (ssize_t)store->block_cache_size &&
	       header->magic == store->cfg->magic &&
	       header->version == store->cfg->version &&
	       header->entry_count > 0 &&
	       header->entry_count <= store->cfg->block_entry_capacity;
}

static int load_block(struct flash_ring_store *store, uint32_t sequence)
{
	ssize_t read;

	read = nvs_read(&store->fs, block_id(store, sequence),
			store->block_cache, store->block_cache_size);
	if (read < 0) {
		device_set_storage_fault(store->cfg->storage_fault_read,
					 read != -ENOENT);
		return (int)read;
	}
	if (block_header(store)->sequence != sequence ||
	    !block_record_valid(store, block_header(store), read)) {
		device_set_storage_fault(store->cfg->storage_fault_read, true);
		return -EINVAL;
	}

	device_set_storage_fault(store->cfg->storage_fault_read, false);
	return 0;
}

static int recover_meta_from_blocks(struct flash_ring_store *store)
{
	uint32_t newest_sequence = 0;
	uint32_t pending_entries = 0;
	uint16_t pending_blocks = 0;
	bool found = false;
	ssize_t read;
	int scan_err = 0;

	for (uint16_t i = 0; i < store->cfg->block_count; i++) {
		read = nvs_read(&store->fs,
				FLASH_RING_STORE_BLOCK_ID_BASE + i,
				store->block_cache, store->block_cache_size);
		if (read < 0) {
			if (read != -ENOENT && scan_err == 0) {
				scan_err = (int)read;
			}
			continue;
		}
		if (!block_record_valid(store, block_header(store), read) ||
		    block_id(store, block_header(store)->sequence) !=
			    FLASH_RING_STORE_BLOCK_ID_BASE + i) {
			continue;
		}

		if (!found || (int32_t)(block_header(store)->sequence -
					newest_sequence) > 0) {
			newest_sequence = block_header(store)->sequence;
			found = true;
		}
	}

	/* Never replace metadata using a partial scan caused by an I/O error. */
	if (scan_err) {
		return scan_err;
	}

	reset_meta(store);
	if (!found) {
		return save_meta(store);
	}

	while (pending_blocks < store->cfg->block_count) {
		uint32_t sequence = newest_sequence - pending_blocks;

		read = nvs_read(&store->fs, block_id(store, sequence),
				store->block_cache, store->block_cache_size);
		if (read < 0 || block_header(store)->sequence != sequence ||
		    !block_record_valid(store, block_header(store), read)) {
			break;
		}

		pending_entries += block_header(store)->entry_count;
		pending_blocks++;
	}

	if (pending_blocks == 0) {
		reset_meta(store);
		return save_meta(store);
	}

	store->meta.pending_blocks = pending_blocks;
	store->meta.pending_entries = pending_entries;
	store->meta.oldest_seq = newest_sequence - pending_blocks + 1U;
	store->meta.next_seq = newest_sequence + 1U;
	store->meta.oldest_offset = 0;

	printk("Recovered %u %s NVM block(s), %u entrie(s)\n", pending_blocks,
	       store->cfg->domain_name, (unsigned int)pending_entries);
	return save_meta(store);
}

static int recover_uncommitted_blocks(struct flash_ring_store *store)
{
	bool recovered = false;
	int err;

	while (store->meta.pending_blocks < store->cfg->block_count) {
		err = load_block(store, store->meta.next_seq);
		if (err == -ENOENT || err == -EINVAL) {
			device_set_storage_fault(store->cfg->storage_fault_read,
						 false);
			break;
		}
		if (err) {
			return err;
		}

		if (store->meta.pending_blocks == 0) {
			store->meta.oldest_seq = store->meta.next_seq;
			store->meta.oldest_offset = 0;
		}
		store->meta.next_seq++;
		store->meta.pending_blocks++;
		store->meta.pending_entries += block_header(store)->entry_count;
		recovered = true;
	}

	if (!recovered) {
		return 0;
	}

	printk("Recovered uncommitted %s NVM block(s); %u entrie(s) pending\n",
	       store->cfg->domain_name, (unsigned int)store->meta.pending_entries);
	return save_meta(store);
}

int flash_ring_store_init(struct flash_ring_store *store)
{
	struct flash_pages_info info;
	ssize_t read;
	int err = 0;

	k_mutex_lock(store->lock, K_FOREVER);
	if (store->initialized) {
		k_mutex_unlock(store->lock);
		return 0;
	}

	store->fs.flash_device = store->cfg->flash_device;
	if (!device_is_ready(store->fs.flash_device)) {
		err = -ENODEV;
		goto out;
	}

	store->fs.offset = store->cfg->offset;
	err = flash_get_page_info_by_offs(store->fs.flash_device,
					  store->fs.offset, &info);
	if (err) {
		goto out;
	}
	if (info.size != FLASH_RING_STORE_SECTOR_SIZE) {
		err = -ENOTSUP;
		goto out;
	}

	store->fs.sector_size = info.size;
	store->fs.sector_count = store->cfg->partition_size / info.size;

	err = nvs_mount(&store->fs);
	if (err) {
		/* The partition may hold stale data left over from before this
		 * partition existed (e.g. a layout change without a full chip
		 * erase). Erase it once and retry before giving up.
		 */
		printk("%s NVM mount failed (err %d); erasing partition and retrying\n",
		       store->cfg->domain_name, err);
		if (flash_erase(store->fs.flash_device, store->fs.offset,
				store->cfg->partition_size) == 0) {
			err = nvs_mount(&store->fs);
		}
		if (err) {
			goto out;
		}
	}

	read = nvs_read(&store->fs, FLASH_RING_STORE_META_ID, &store->meta,
			sizeof(store->meta));
	if (read == -ENOENT) {
		err = recover_meta_from_blocks(store);
	} else if (read < 0) {
		err = (int)read;
	} else if (read != sizeof(store->meta) ||
		   store->meta.magic != store->cfg->magic ||
		   store->meta.version != store->cfg->version ||
		   store->meta.partition_size != store->cfg->partition_size) {
		printk("Incompatible %s NVM format or capacity; clearing stored entries\n",
		       store->cfg->domain_name);
		err = reset_storage(store);
	} else if (store->meta.pending_blocks > store->cfg->block_count ||
		   store->meta.pending_entries >
			   ((uint32_t)store->meta.pending_blocks *
			    store->cfg->flush_batch) ||
		   store->meta.oldest_offset >= store->cfg->block_entry_capacity ||
		   (store->meta.pending_blocks == 0 &&
		    (store->meta.pending_entries != 0 ||
		     store->meta.oldest_offset != 0))) {
		printk("Invalid %s NVM metadata; clearing stored entries\n",
		       store->cfg->domain_name);
		err = reset_storage(store);
	}

	if (!err) {
		err = recover_uncommitted_blocks(store);
	}

	if (!err) {
		store->initialized = true;
	}
out:
	device_set_storage_fault(store->cfg->storage_fault_init, err != 0);
	if (err) {
		printk("%s NVM initialization failed (err %d)\n",
		       store->cfg->domain_name, err);
	}
	k_mutex_unlock(store->lock);
	return err;
}

static int discard_oldest_block(struct flash_ring_store *store)
{
	struct flash_ring_store_meta previous_meta;
	uint32_t discarded_entries;
	uint32_t retired_sequence;
	int err;

	if (store->meta.pending_blocks == 0) {
		return 0;
	}

	err = load_block(store, store->meta.oldest_seq);
	if (err) {
		return err;
	}

	previous_meta = store->meta;
	retired_sequence = store->meta.oldest_seq;
	discarded_entries = block_header(store)->entry_count -
			    store->meta.oldest_offset;
	store->meta.oldest_seq++;
	store->meta.pending_blocks--;
	store->meta.oldest_offset = 0;
	store->meta.pending_entries -=
		MIN(store->meta.pending_entries, discarded_entries);
	if (store->meta.pending_blocks == 0) {
		store->meta.oldest_seq = store->meta.next_seq;
	}

	err = save_meta(store);
	if (err) {
		store->meta = previous_meta;
		return err;
	}

	err = nvs_delete(&store->fs, block_id(store, retired_sequence));
	if (err && err != -ENOENT) {
		printk("Failed to delete overwritten %s block %u (err %d)\n",
		       store->cfg->domain_name, (unsigned int)retired_sequence,
		       err);
		device_set_storage_fault(store->cfg->storage_fault_delete, true);
		return 0;
	}

	device_set_storage_fault(store->cfg->storage_fault_delete, false);
	return 0;
}

int flash_ring_store_append(struct flash_ring_store *store,
			    const uint8_t *entries, uint16_t entry_count)
{
	struct flash_ring_store_meta previous_meta;
	bool previous_meta_dirty;
	uint32_t sequence;
	ssize_t written;
	int err;

	if (!entries || entry_count == 0 ||
	    entry_count > store->cfg->block_entry_capacity ||
	    (store->cfg->flush_batch != 0 &&
	     entry_count != store->cfg->flush_batch)) {
		return -EINVAL;
	}

	err = flash_ring_store_init(store);
	if (err) {
		return err;
	}

	k_mutex_lock(store->lock, K_FOREVER);
	if (store->meta.pending_blocks >= store->cfg->block_count) {
		err = discard_oldest_block(store);
		if (err) {
			k_mutex_unlock(store->lock);
			return err;
		}
	}

	previous_meta = store->meta;
	previous_meta_dirty = store->meta_dirty;
	sequence = store->meta.next_seq;
	memset(store->block_cache, 0, store->block_cache_size);
	block_header(store)->magic = store->cfg->magic;
	block_header(store)->version = store->cfg->version;
	block_header(store)->entry_count = entry_count;
	block_header(store)->sequence = sequence;
	memcpy(block_entries(store), entries,
	       (size_t)entry_count * store->cfg->entry_size);

	written = nvs_write(&store->fs, block_id(store, sequence),
			    store->block_cache, store->block_cache_size);
	if (written < 0) {
		err = (int)written;
		goto out;
	}
	if (written != 0 && written != (ssize_t)store->block_cache_size) {
		err = -EIO;
		goto out;
	}

	if (store->meta.pending_blocks == 0) {
		store->meta.oldest_seq = sequence;
		store->meta.oldest_offset = 0;
	}
	store->meta.next_seq++;
	store->meta.pending_blocks++;
	store->meta.pending_entries += entry_count;

	err = save_meta(store);
	if (err) {
		store->meta = previous_meta;
		store->meta_dirty = previous_meta_dirty;
	}
out:
	device_set_storage_fault(store->cfg->storage_fault_write, err != 0);
	k_mutex_unlock(store->lock);
	return err;
}

int flash_ring_store_peek(struct flash_ring_store *store, uint8_t *buffer,
			  uint16_t buffer_len, uint16_t *bytes_written)
{
	uint16_t available;
	uint16_t entry_count;
	int err;

	if (!buffer || !bytes_written) {
		return -EINVAL;
	}
	*bytes_written = 0;
	if (buffer_len < store->cfg->entry_size) {
		return 0;
	}

	err = flash_ring_store_init(store);
	if (err) {
		return err;
	}

	k_mutex_lock(store->lock, K_FOREVER);
	if (store->meta.pending_blocks == 0) {
		k_mutex_unlock(store->lock);
		return 0;
	}

	err = load_block(store, store->meta.oldest_seq);
	if (err) {
		k_mutex_unlock(store->lock);
		return err;
	}

	available = block_header(store)->entry_count - store->meta.oldest_offset;
	entry_count = MIN(available,
			  (uint16_t)(buffer_len / store->cfg->entry_size));
	memcpy(buffer,
	       block_entries(store) +
		       ((size_t)store->meta.oldest_offset * store->cfg->entry_size),
	       (size_t)entry_count * store->cfg->entry_size);
	*bytes_written = entry_count * store->cfg->entry_size;
	k_mutex_unlock(store->lock);
	return 0;
}

int flash_ring_store_drop(struct flash_ring_store *store,
			  uint16_t entry_count, bool *block_retired)
{
	struct flash_ring_store_meta previous_meta;
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

	err = flash_ring_store_init(store);
	if (err) {
		return err;
	}

	k_mutex_lock(store->lock, K_FOREVER);
	if (store->meta.pending_blocks == 0) {
		err = -ENOENT;
		goto out;
	}

	err = load_block(store, store->meta.oldest_seq);
	if (err) {
		goto out;
	}

	remaining = block_header(store)->entry_count - store->meta.oldest_offset;
	if (entry_count > remaining) {
		err = -EINVAL;
		goto out;
	}

	previous_meta = store->meta;
	previous_meta_dirty = store->meta_dirty;
	store->meta.pending_entries -= entry_count;
	if (entry_count == remaining) {
		retired_sequence = store->meta.oldest_seq;
		store->meta.oldest_seq++;
		store->meta.pending_blocks--;
		store->meta.oldest_offset = 0;
		if (store->meta.pending_blocks == 0) {
			store->meta.oldest_seq = store->meta.next_seq;
		}
		store->meta_dirty = true;

		/* Commit the new queue head before deleting a retired block. */
		err = save_meta(store);
		if (err) {
			store->meta = previous_meta;
			store->meta_dirty = previous_meta_dirty;
			goto out;
		}
	} else {
		store->meta.oldest_offset += entry_count;
		retired_sequence = UINT32_MAX;
		store->meta_dirty = true;
	}

	if (retired_sequence != UINT32_MAX) {
		err = nvs_delete(&store->fs, block_id(store, retired_sequence));
		if (err && err != -ENOENT) {
			printk("Failed to delete retired %s block %u (err %d)\n",
			       store->cfg->domain_name,
			       (unsigned int)retired_sequence, err);
			device_set_storage_fault(store->cfg->storage_fault_delete,
						 true);
		} else {
			device_set_storage_fault(store->cfg->storage_fault_delete,
						 false);
		}
		/* Metadata is already committed; deletion is cleanup only. */
		*block_retired = true;
		err = 0;
	}
out:
	k_mutex_unlock(store->lock);
	return err;
}

int flash_ring_store_sync(struct flash_ring_store *store)
{
	int err;

	err = flash_ring_store_init(store);
	if (err) {
		return err;
	}

	k_mutex_lock(store->lock, K_FOREVER);
	if (store->meta_dirty) {
		err = save_meta(store);
	} else {
		err = 0;
	}
	k_mutex_unlock(store->lock);
	return err;
}

int flash_ring_store_get_count(struct flash_ring_store *store,
			       uint32_t *count)
{
	int err;

	if (!count) {
		return -EINVAL;
	}
	*count = 0;

	err = flash_ring_store_init(store);
	if (err) {
		return err;
	}

	k_mutex_lock(store->lock, K_FOREVER);
	*count = store->meta.pending_entries;
	k_mutex_unlock(store->lock);
	return 0;
}
