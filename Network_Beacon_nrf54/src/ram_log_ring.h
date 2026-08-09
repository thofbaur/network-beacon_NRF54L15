#ifndef RAM_LOG_RING_H
#define RAM_LOG_RING_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/*
 * Generic RAM ring buffer of fixed-size entries, flushed in fixed-size
 * batches to a flash-backed domain once a threshold is reached, with
 * single-owner export (flash-then-RAM) begin/commit/abort semantics.
 *
 * One struct ram_log_ring instance per domain (self-report, eco log, ...).
 * Each domain owns its own entries/flush_buffer storage, sized to its own
 * ring_count/entry_size/flush_batch, and supplies its flash storage calls,
 * Kconfig-derived thresholds, and log text via a const ram_log_ring_config.
 * All the ring-management, flush-scheduling, and export logic lives once in
 * ram_log_ring.c.
 */

struct ram_log_ring_config {
	uint16_t ring_count;
	uint16_t entry_size;
	uint16_t flush_threshold;
	uint16_t flush_batch;
	uint32_t flush_retry_ms;
	int (*storage_init)(void);
	int (*storage_append)(const uint8_t *entries, uint16_t entry_count);
	int (*storage_peek)(uint8_t *buffer, uint16_t buffer_len,
			    uint16_t *bytes_written);
	int (*storage_drop)(uint16_t entry_count, bool *block_retired);
	int (*storage_sync)(void);
	int (*storage_get_count)(uint32_t *count);
	/* Passed straight to device_set_storage_full() (STORAGE_FULL_* in
	 * device.h) whenever the ring's nvm_full state changes.
	 */
	uint32_t storage_full_bit;
	/* Used only in printk() text, e.g. "eco log", "self-report". */
	const char *domain_name;
};

enum ram_log_ring_export_source {
	RAM_LOG_RING_EXPORT_NONE,
	RAM_LOG_RING_EXPORT_FLASH,
	RAM_LOG_RING_EXPORT_RAM,
};

struct ram_log_ring {
	const struct ram_log_ring_config *cfg;
	struct k_mutex *lock;
	/* Domain-owned buffers: ring_count * entry_size and
	 * flush_batch * entry_size bytes respectively.
	 */
	uint8_t *entries;
	uint8_t *flush_buffer;
	uint16_t read_index;
	uint16_t write_index;
	uint16_t count;
	uint16_t export_entries;
	uint16_t flush_read_index;
	bool export_active;
	bool flush_active;
	bool nvm_full;
	enum ram_log_ring_export_source export_source;
	struct k_work_delayable flush_work;
};

void ram_log_ring_init(struct ram_log_ring *ring);
/* Returns true if the entry was stored, false if it was dropped because the
 * ring is full and its oldest entry is pinned by an in-progress export or
 * flush (the caller should skip any "stored" side effect, e.g. an LED
 * flash, in that case).
 */
bool ram_log_ring_push(struct ram_log_ring *ring, const uint8_t *entry);
int ram_log_ring_export_begin(struct ram_log_ring *ring, uint8_t *buffer,
			      uint16_t buffer_len, uint16_t *bytes_written);
int ram_log_ring_export_commit(struct ram_log_ring *ring);
void ram_log_ring_export_abort(struct ram_log_ring *ring);
int ram_log_ring_sync(struct ram_log_ring *ring);
int ram_log_ring_get_count(struct ram_log_ring *ring, uint16_t *count);

#endif /* RAM_LOG_RING_H */
