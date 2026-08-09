#ifndef FLASH_RING_STORE_H
#define FLASH_RING_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>

/*
 * Generic ring buffer of fixed-size entries, flushed in fixed-size blocks to
 * a dedicated NVS-backed flash partition, with boot-time recovery and a
 * mount-failure self-heal (erase the partition once and retry, in case it
 * holds stale data left over from before it existed - see the
 * eco_log_storage.c commit history for why this matters).
 *
 * One struct flash_ring_store instance per domain (contacts, self-reports,
 * eco sessions, ...). Each domain owns its own partition, magic/version,
 * entry size, and fault-status bits, supplied via a const
 * flash_ring_store_config, and its own block cache buffer sized to fit
 * exactly one block. All the actual NVS/recovery logic lives once in
 * flash_ring_store.c.
 */

#define FLASH_RING_STORE_SECTOR_SIZE 4096U
#define FLASH_RING_STORE_META_ID 1U
#define FLASH_RING_STORE_BLOCK_ID_BASE 0x100U

/* Convenience for domains that pack several small fixed-size blocks per
 * sector (one reserved sector, N block slots per remaining sector) - e.g.
 * self-report, eco log. Domains with a different packing scheme (e.g.
 * contact storage, which packs one large block per sector with its own
 * reserved-sector count) compute their own block_count instead and supply
 * it directly via flash_ring_store_config::block_count.
 */
#define FLASH_RING_STORE_RESERVED_SECTORS 1U
#define FLASH_RING_STORE_BLOCKS_PER_SECTOR 4U
#define FLASH_RING_STORE_BLOCK_COUNT(total_size) \
	((((total_size) / FLASH_RING_STORE_SECTOR_SIZE) - \
	  FLASH_RING_STORE_RESERVED_SECTORS) * \
	 FLASH_RING_STORE_BLOCKS_PER_SECTOR)

/* On-flash block record header, followed by entry_count * entry_size bytes
 * of raw entry data. Domains provide a block_cache buffer sized via
 * FLASH_RING_STORE_BLOCK_SIZE() below; the generic code owns everything in
 * that buffer, including this header.
 */
struct flash_ring_store_block_header {
	uint32_t magic;
	uint16_t version;
	uint16_t entry_count;
	uint32_t sequence;
} __packed;

#define FLASH_RING_STORE_BLOCK_SIZE(entry_capacity, entry_size) \
	(sizeof(struct flash_ring_store_block_header) + \
	 ((size_t)(entry_capacity) * (entry_size)))

/* Domains declare their block_cache buffer with this size and MUST also
 * mark it `__aligned(4)`: flash_ring_store casts the buffer's start to
 * struct flash_ring_store_block_header * to read/write its uint32_t/uint16_t
 * fields directly, which is undefined behavior on a buffer without at least
 * 4-byte alignment. A plain `static uint8_t buf[N];` is not guaranteed to be
 * aligned that way.
 */

struct flash_ring_store_meta {
	uint32_t magic;
	uint16_t version;
	uint32_t partition_size;
	uint16_t pending_blocks;
	uint32_t pending_entries;
	uint32_t oldest_seq;
	uint32_t next_seq;
	uint16_t oldest_offset;
};

struct flash_ring_store_config {
	const struct device *flash_device;
	off_t offset;
	size_t partition_size;
	uint32_t magic;
	uint16_t version;
	uint16_t entry_size;
	uint16_t block_entry_capacity;
	/* Number of block ID slots (0x100..0x100+block_count-1) reserved in
	 * the partition. Domains that pack several fixed-size blocks per
	 * sector can derive this via FLASH_RING_STORE_BLOCK_COUNT() above;
	 * domains with a different packing scheme (e.g. one large block per
	 * sector) compute it their own way and supply it directly.
	 */
	uint16_t block_count;
	/* If nonzero, flash_ring_store_append() rejects any entry_count other
	 * than this exact value, matching a domain's "always flush a full,
	 * fixed-size batch" caller contract (self-report, eco log). Must be
	 * <= block_entry_capacity.
	 *
	 * If zero, any entry_count in 1..block_entry_capacity is accepted,
	 * for domains whose caller flushes a variable amount up to capacity
	 * (contacts).
	 */
	uint16_t flush_batch;
	uint32_t storage_fault_init;
	uint32_t storage_fault_read;
	uint32_t storage_fault_write;
	uint32_t storage_fault_meta;
	uint32_t storage_fault_delete;
	/* Used only in printk() text, e.g. "eco log", "self-report". */
	const char *domain_name;
};

struct flash_ring_store {
	const struct flash_ring_store_config *cfg;
	struct k_mutex *lock;
	struct nvs_fs fs;
	struct flash_ring_store_meta meta;
	uint8_t *block_cache;
	size_t block_cache_size;
	bool initialized;
	bool meta_dirty;
};

int flash_ring_store_init(struct flash_ring_store *store);
int flash_ring_store_append(struct flash_ring_store *store,
			    const uint8_t *entries, uint16_t entry_count);
int flash_ring_store_peek(struct flash_ring_store *store, uint8_t *buffer,
			  uint16_t buffer_len, uint16_t *bytes_written);
int flash_ring_store_drop(struct flash_ring_store *store,
			  uint16_t entry_count, bool *block_retired);
int flash_ring_store_sync(struct flash_ring_store *store);
int flash_ring_store_get_count(struct flash_ring_store *store,
			       uint32_t *count);

#endif /* FLASH_RING_STORE_H */
