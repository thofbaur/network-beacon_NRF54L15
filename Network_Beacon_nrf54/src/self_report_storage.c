#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#include "device.h"
#include "flash_ring_store.h"
#include "self_report.h"
#include "self_report_storage.h"

#define SELF_REPORT_STORAGE_PARTITION self_report_storage
#define SELF_REPORT_STORAGE_MAGIC 0x44534152U
#define SELF_REPORT_STORAGE_VERSION 3U

BUILD_ASSERT(FIXED_PARTITION_SIZE(SELF_REPORT_STORAGE_PARTITION) ==
	     CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES,
	     "Self-report NVS partition size mismatch");
BUILD_ASSERT((CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES %
	      FLASH_RING_STORE_SECTOR_SIZE) == 0,
	     "Self-report NVS partition must contain whole sectors");
BUILD_ASSERT(CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES >=
	     (2U * FLASH_RING_STORE_SECTOR_SIZE),
	     "Self-report NVS partition must contain at least two sectors");
BUILD_ASSERT(CONFIG_DSA_SELF_REPORT_FLUSH_BATCH <=
	     SELF_REPORT_STORAGE_BLOCK_ENTRIES,
	     "Self-report flush batch exceeds flash block");

static const struct flash_ring_store_config self_report_cfg = {
	.flash_device = FIXED_PARTITION_DEVICE(SELF_REPORT_STORAGE_PARTITION),
	.offset = FIXED_PARTITION_OFFSET(SELF_REPORT_STORAGE_PARTITION),
	.partition_size = CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES,
	.magic = SELF_REPORT_STORAGE_MAGIC,
	.version = SELF_REPORT_STORAGE_VERSION,
	.entry_size = SELF_REPORT_ENTRY_SIZE,
	.block_entry_capacity = SELF_REPORT_STORAGE_BLOCK_ENTRIES,
	.block_count = FLASH_RING_STORE_BLOCK_COUNT(CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES),
	.flush_batch = CONFIG_DSA_SELF_REPORT_FLUSH_BATCH,
	.storage_fault_init = STORAGE_FAULT_SELF_REPORT_INIT,
	.storage_fault_read = STORAGE_FAULT_SELF_REPORT_READ,
	.storage_fault_write = STORAGE_FAULT_SELF_REPORT_WRITE,
	.storage_fault_meta = STORAGE_FAULT_SELF_REPORT_META,
	.storage_fault_delete = STORAGE_FAULT_SELF_REPORT_DELETE,
	.domain_name = "self-report",
};

/* Aligned so flash_ring_store's block_header()/block_entries() casts (which
 * dereference uint32_t/uint16_t fields at the start of this buffer) are
 * well-defined rather than relying on incidental static-storage alignment.
 */
static uint8_t self_report_block_cache[FLASH_RING_STORE_BLOCK_SIZE(
	SELF_REPORT_STORAGE_BLOCK_ENTRIES, SELF_REPORT_ENTRY_SIZE)]
	__aligned(4);

static K_MUTEX_DEFINE(self_report_lock);

static struct flash_ring_store self_report_store = {
	.cfg = &self_report_cfg,
	.lock = &self_report_lock,
	.block_cache = self_report_block_cache,
	.block_cache_size = sizeof(self_report_block_cache),
};

int self_report_storage_init(void)
{
	return flash_ring_store_init(&self_report_store);
}

int self_report_storage_append(const uint8_t *reports, uint16_t report_count)
{
	return flash_ring_store_append(&self_report_store, reports, report_count);
}

int self_report_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			     uint16_t *bytes_written)
{
	return flash_ring_store_peek(&self_report_store, buffer, buffer_len,
				     bytes_written);
}

int self_report_storage_drop(uint16_t report_count, bool *block_retired)
{
	return flash_ring_store_drop(&self_report_store, report_count,
				     block_retired);
}

int self_report_storage_sync(void)
{
	return flash_ring_store_sync(&self_report_store);
}

int self_report_storage_get_count(uint32_t *count)
{
	return flash_ring_store_get_count(&self_report_store, count);
}
