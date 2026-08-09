#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include "device.h"
#include "eco_log.h"
#include "eco_log_storage.h"
#include "flash_ring_store.h"

#define ECO_LOG_STORAGE_PARTITION eco_log_storage
#define ECO_LOG_STORAGE_MAGIC 0x44534145U
#define ECO_LOG_STORAGE_VERSION 1U

BUILD_ASSERT(FIXED_PARTITION_SIZE(ECO_LOG_STORAGE_PARTITION) ==
	     CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES,
	     "Eco log NVS partition size mismatch");
BUILD_ASSERT((CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES %
	      FLASH_RING_STORE_SECTOR_SIZE) == 0,
	     "Eco log NVS partition must contain whole sectors");
BUILD_ASSERT(CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES >=
	     (2U * FLASH_RING_STORE_SECTOR_SIZE),
	     "Eco log NVS partition must contain at least two sectors");
BUILD_ASSERT(CONFIG_DSA_ECO_LOG_FLUSH_BATCH <=
	     ECO_LOG_STORAGE_BLOCK_ENTRIES,
	     "Eco log flush batch exceeds flash block");

static const struct flash_ring_store_config eco_log_cfg = {
	.flash_device = FIXED_PARTITION_DEVICE(ECO_LOG_STORAGE_PARTITION),
	.offset = FIXED_PARTITION_OFFSET(ECO_LOG_STORAGE_PARTITION),
	.partition_size = CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES,
	.magic = ECO_LOG_STORAGE_MAGIC,
	.version = ECO_LOG_STORAGE_VERSION,
	.entry_size = ECO_LOG_ENTRY_SIZE,
	.block_entry_capacity = ECO_LOG_STORAGE_BLOCK_ENTRIES,
	.block_count = FLASH_RING_STORE_BLOCK_COUNT(CONFIG_DSA_ECO_LOG_FLASH_SIZE_BYTES),
	.flush_batch = CONFIG_DSA_ECO_LOG_FLUSH_BATCH,
	.storage_fault_init = STORAGE_FAULT_ECO_LOG_INIT,
	.storage_fault_read = STORAGE_FAULT_ECO_LOG_READ,
	.storage_fault_write = STORAGE_FAULT_ECO_LOG_WRITE,
	.storage_fault_meta = STORAGE_FAULT_ECO_LOG_META,
	.storage_fault_delete = STORAGE_FAULT_ECO_LOG_DELETE,
	.domain_name = "eco log",
};

/* Aligned so flash_ring_store's block_header()/block_entries() casts (which
 * dereference uint32_t/uint16_t fields at the start of this buffer) are
 * well-defined rather than relying on incidental static-storage alignment.
 */
static uint8_t eco_log_block_cache[FLASH_RING_STORE_BLOCK_SIZE(
	ECO_LOG_STORAGE_BLOCK_ENTRIES, ECO_LOG_ENTRY_SIZE)]
	__aligned(4);

static K_MUTEX_DEFINE(eco_log_lock);

static struct flash_ring_store eco_log_store = {
	.cfg = &eco_log_cfg,
	.lock = &eco_log_lock,
	.block_cache = eco_log_block_cache,
	.block_cache_size = sizeof(eco_log_block_cache),
};

int eco_log_storage_init(void)
{
	return flash_ring_store_init(&eco_log_store);
}

int eco_log_storage_append(const uint8_t *entries, uint16_t entry_count)
{
	return flash_ring_store_append(&eco_log_store, entries, entry_count);
}

int eco_log_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			 uint16_t *bytes_written)
{
	return flash_ring_store_peek(&eco_log_store, buffer, buffer_len,
				     bytes_written);
}

int eco_log_storage_drop(uint16_t entry_count, bool *block_retired)
{
	return flash_ring_store_drop(&eco_log_store, entry_count,
				     block_retired);
}

int eco_log_storage_sync(void)
{
	return flash_ring_store_sync(&eco_log_store);
}

int eco_log_storage_get_count(uint32_t *count)
{
	return flash_ring_store_get_count(&eco_log_store, count);
}
