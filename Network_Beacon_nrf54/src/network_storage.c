#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include "device.h"
#include "flash_ring_store.h"
#include "network_storage.h"

#define NETWORK_STORAGE_PARTITION contact_storage
#define NETWORK_STORAGE_MAGIC 0x44534143U
#define NETWORK_STORAGE_VERSION 5U

BUILD_ASSERT((NETWORK_STORAGE_BLOCK_DATA_LEN % NETWORK_STORAGE_CONTACT_SIZE) == 0,
	     "Contact NVM block payload must fit whole contacts");
BUILD_ASSERT(FIXED_PARTITION_SIZE(NETWORK_STORAGE_PARTITION) ==
	     NETWORK_STORAGE_TOTAL_BYTES,
	     "Contact NVM reservation must match contact_storage partition size");
BUILD_ASSERT((NETWORK_STORAGE_TOTAL_BYTES % NETWORK_STORAGE_SECTOR_BYTES) == 0,
	     "Contact NVM reservation must contain whole flash sectors");
BUILD_ASSERT(NETWORK_STORAGE_BLOCK_COUNT > 0,
	     "Contact NVM reservation must contain more than the reserved sectors");
BUILD_ASSERT(FLASH_RING_STORE_BLOCK_SIZE(NETWORK_STORAGE_BLOCK_CONTACTS,
					 NETWORK_STORAGE_CONTACT_SIZE) <=
	     NETWORK_STORAGE_BLOCK_BYTES,
	     "Contact NVM block must leave room for NVS bookkeeping");

/* Unlike self-report/eco log, contacts pack one large variable-length block
 * per sector rather than several small fixed-size blocks per sector, so
 * block_count and the reserved-sector count are computed here (via
 * network_storage.h's own macros) rather than via
 * FLASH_RING_STORE_BLOCK_COUNT(). flush_batch is 0: network.c's flush
 * handler writes a variable amount up to capacity (MIN(capacity,
 * available)), not a fixed batch size.
 */
static const struct flash_ring_store_config network_cfg = {
	.flash_device = FIXED_PARTITION_DEVICE(NETWORK_STORAGE_PARTITION),
	.offset = FIXED_PARTITION_OFFSET(NETWORK_STORAGE_PARTITION),
	.partition_size = NETWORK_STORAGE_TOTAL_BYTES,
	.magic = NETWORK_STORAGE_MAGIC,
	.version = NETWORK_STORAGE_VERSION,
	.entry_size = NETWORK_STORAGE_CONTACT_SIZE,
	.block_entry_capacity = NETWORK_STORAGE_BLOCK_CONTACTS,
	.block_count = NETWORK_STORAGE_BLOCK_COUNT,
	.flush_batch = 0,
	.storage_fault_init = STORAGE_FAULT_CONTACT_INIT,
	.storage_fault_read = STORAGE_FAULT_CONTACT_READ,
	.storage_fault_write = STORAGE_FAULT_CONTACT_WRITE,
	.storage_fault_meta = STORAGE_FAULT_CONTACT_META,
	.storage_fault_delete = STORAGE_FAULT_CONTACT_DELETE,
	.domain_name = "contact",
};

/* Aligned so flash_ring_store's block_header()/block_entries() casts (which
 * dereference uint32_t/uint16_t fields at the start of this buffer) are
 * well-defined rather than relying on incidental static-storage alignment.
 */
static uint8_t network_block_cache[FLASH_RING_STORE_BLOCK_SIZE(
	NETWORK_STORAGE_BLOCK_CONTACTS, NETWORK_STORAGE_CONTACT_SIZE)]
	__aligned(4);

static K_MUTEX_DEFINE(network_storage_lock);

static struct flash_ring_store network_store = {
	.cfg = &network_cfg,
	.lock = &network_storage_lock,
	.block_cache = network_block_cache,
	.block_cache_size = sizeof(network_block_cache),
};

int network_storage_init(void)
{
	return flash_ring_store_init(&network_store);
}

int network_storage_append_block(const uint8_t *data, uint16_t len)
{
	if (len == 0 || (len % NETWORK_STORAGE_CONTACT_SIZE) != 0) {
		return -EINVAL;
	}

	return flash_ring_store_append(&network_store, data,
				       len / NETWORK_STORAGE_CONTACT_SIZE);
}

int network_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			 uint16_t *bytes_written)
{
	return flash_ring_store_peek(&network_store, buffer, buffer_len,
				     bytes_written);
}

int network_storage_drop(uint16_t bytes_to_drop)
{
	bool block_retired;

	bytes_to_drop -= bytes_to_drop % NETWORK_STORAGE_CONTACT_SIZE;
	if (bytes_to_drop == 0) {
		return 0;
	}

	return flash_ring_store_drop(&network_store,
				     bytes_to_drop / NETWORK_STORAGE_CONTACT_SIZE,
				     &block_retired);
}

int network_storage_get_contact_count(uint32_t *count)
{
	return flash_ring_store_get_count(&network_store, count);
}

int network_storage_sync(void)
{
	return flash_ring_store_sync(&network_store);
}
