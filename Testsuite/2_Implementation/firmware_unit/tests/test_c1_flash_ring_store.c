/*
 * C1 - flash_ring_store.c: the generic flash-backed FIFO under contacts,
 * self-reports and eco sessions. Uses the in-RAM NVS fake, which persists
 * across simulated reboots and can cut power at any write.
 *
 * Three store configurations are exercised:
 *   - CONTACT: the production contact configuration (network_storage.c):
 *     variable-size blocks, flush_batch = 0, 239 blocks of 804 contacts.
 *   - BATCH:   the production self-report configuration: fixed 64-entry
 *     blocks, flush_batch = 64, 4 blocks.
 *   - SMALL:   a tiny variable-size ring (3 blocks of 4 entries) to reach
 *     wrap-around quickly.
 * See 1_Specification/C_firmware_unit.md for the specification of every case.
 */
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>

#include "device.h"
#include "fake_nvs.h"
#include "fake_peers.h"
#include "flash_ring_store.h"
#include "minitest.h"
#include "network_storage.h"
#include "self_report_storage.h"

#define SMALL_OFFSET 0x200000
#define SMALL_ENTRY 5
#define SMALL_CAPACITY 4
#define SMALL_BLOCKS 3

#define FAULTS(prefix)                                                        \
	.storage_fault_init = STORAGE_FAULT_##prefix##_INIT,                  \
	.storage_fault_read = STORAGE_FAULT_##prefix##_READ,                  \
	.storage_fault_write = STORAGE_FAULT_##prefix##_WRITE,                \
	.storage_fault_meta = STORAGE_FAULT_##prefix##_META,                  \
	.storage_fault_delete = STORAGE_FAULT_##prefix##_DELETE

static const struct flash_ring_store_config contact_cfg = {
	.flash_device = &shim_flash_device,
	.offset = SHIM_PARTITION_OFFSET_contact_storage,
	.partition_size = NETWORK_STORAGE_TOTAL_BYTES,
	.magic = 0x44534143U,
	.version = 5U,
	.entry_size = NETWORK_STORAGE_CONTACT_SIZE,
	.block_entry_capacity = NETWORK_STORAGE_BLOCK_CONTACTS,
	.block_count = NETWORK_STORAGE_BLOCK_COUNT,
	.flush_batch = 0,
	FAULTS(CONTACT),
	.domain_name = "contact",
};

static const struct flash_ring_store_config batch_cfg = {
	.flash_device = &shim_flash_device,
	.offset = SHIM_PARTITION_OFFSET_self_report_storage,
	.partition_size = CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES,
	.magic = 0x44534152U,
	.version = 3U,
	.entry_size = 3,
	.block_entry_capacity = SELF_REPORT_STORAGE_BLOCK_ENTRIES,
	.block_count = FLASH_RING_STORE_BLOCK_COUNT(CONFIG_DSA_SELF_REPORT_FLASH_SIZE_BYTES),
	.flush_batch = CONFIG_DSA_SELF_REPORT_FLUSH_BATCH,
	FAULTS(SELF_REPORT),
	.domain_name = "self-report",
};

static const struct flash_ring_store_config small_cfg = {
	.flash_device = &shim_flash_device,
	.offset = SMALL_OFFSET,
	.partition_size = 4 * FLASH_RING_STORE_SECTOR_SIZE,
	.magic = 0x54455354U,
	.version = 1U,
	.entry_size = SMALL_ENTRY,
	.block_entry_capacity = SMALL_CAPACITY,
	.block_count = SMALL_BLOCKS,
	.flush_batch = 0,
	FAULTS(ECO_LOG),
	.domain_name = "small",
};

struct test_store {
	struct flash_ring_store store;
	struct k_mutex lock;
	uint8_t cache[FLASH_RING_STORE_BLOCK_SIZE(NETWORK_STORAGE_BLOCK_CONTACTS,
						  NETWORK_STORAGE_CONTACT_SIZE)]
		__aligned(4);
};

static struct test_store ts;

/* Simulated (re)boot: fresh RAM state, flash content kept. */
static struct flash_ring_store *boot(const struct flash_ring_store_config *cfg)
{
	memset(&ts, 0, sizeof(ts));
	ts.store.cfg = cfg;
	ts.store.lock = &ts.lock;
	ts.store.block_cache = ts.cache;
	ts.store.block_cache_size = FLASH_RING_STORE_BLOCK_SIZE(
		cfg->block_entry_capacity, cfg->entry_size);
	return &ts.store;
}

static struct flash_ring_store *reboot(const struct flash_ring_store_config *cfg)
{
	fake_nvs_power_cycle();
	return boot(cfg);
}

/* Deterministic entry content for sequence index i. */
static void make_entry(uint8_t *out, uint16_t entry_size, uint32_t i)
{
	for (uint16_t b = 0; b < entry_size; b++) {
		out[b] = (uint8_t)((i >> (8 * (b % 3))) ^ (b * 0x5b));
	}
}

static uint32_t entry_index(const uint8_t *e, uint16_t entry_size)
{
	/* Bytes 0..2 hold i ^ pattern (entry_size >= 3). */
	return ((uint32_t)(e[0] ^ 0x00)) | ((uint32_t)(e[1] ^ 0x5b) << 8) |
	       ((uint32_t)(e[2] ^ (2 * 0x5b & 0xff)) << 16);
}

static bool entry_valid(const uint8_t *e, uint16_t entry_size)
{
	uint8_t expected[16];

	make_entry(expected, entry_size, entry_index(e, entry_size));
	return memcmp(expected, e, entry_size) == 0;
}

static int append_range(struct flash_ring_store *s, uint32_t first, uint16_t count)
{
	static uint8_t buf[NETWORK_STORAGE_BLOCK_DATA_LEN];
	uint16_t es = s->cfg->entry_size;

	for (uint16_t i = 0; i < count; i++) {
		make_entry(&buf[i * es], es, first + i);
	}
	return flash_ring_store_append(s, buf, count);
}

/* Drain everything via peek/drop, storing the sequence indices. */
static int drain(struct flash_ring_store *s, uint32_t *indices, size_t max,
		 size_t *n_out)
{
	static uint8_t buf[NETWORK_STORAGE_BLOCK_DATA_LEN];
	size_t n = 0;
	uint16_t es = s->cfg->entry_size;

	for (;;) {
		uint16_t written = 0;
		bool retired;
		int err = flash_ring_store_peek(s, buf, sizeof(buf) - sizeof(buf) % es,
						&written);
		if (err) {
			return err;
		}
		if (written == 0) {
			break;
		}
		for (uint16_t off = 0; off < written; off += es) {
			if (!entry_valid(&buf[off], es)) {
				return -EBADMSG;
			}
			if (n < max) {
				indices[n] = entry_index(&buf[off], es);
			}
			n++;
		}
		err = flash_ring_store_drop(s, written / es, &retired);
		if (err) {
			return err;
		}
	}
	*n_out = n;
	return 0;
}

static uint32_t count_of(struct flash_ring_store *s)
{
	uint32_t count = 0xdeadbeef;

	MT_ASSERT_EQ(0, flash_ring_store_get_count(s, &count));
	return count;
}

static void setup(void)
{
	fake_nvs_reset_all();
	fake_peers_reset();
	shim_flash_set_page_size(4096);
	shim_flash_set_page_info_error(0);
	shim_flash_set_erase_error(0);
	shim_flash_device.ready = true;
	device_set_storage_fault(0xFFFFFFFFU, false);
}

/* ---------------------------------------------------------------------- */

MT_TEST(c1_production_contact_geometry)
{
	/* 0xf1000 partition, 4 KiB sectors, 2 reserved: 239 blocks of 804. */
	MT_ASSERT_EQ(239, NETWORK_STORAGE_BLOCK_COUNT);
	MT_ASSERT_EQ(804, NETWORK_STORAGE_BLOCK_CONTACTS);
	MT_ASSERT(FLASH_RING_STORE_BLOCK_SIZE(NETWORK_STORAGE_BLOCK_CONTACTS,
					      NETWORK_STORAGE_CONTACT_SIZE) <=
		  FLASH_RING_STORE_SECTOR_SIZE - 64);
}

MT_TEST(c1_init_empty_partition_has_no_entries)
{
	struct flash_ring_store *s = boot(&contact_cfg);

	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(0, count_of(s));
	MT_ASSERT(!(device_get_storage_faults() & STORAGE_FAULT_CONTACT_INIT));
}

MT_TEST(c1_append_then_peek_returns_entries_in_order)
{
	struct flash_ring_store *s = boot(&contact_cfg);
	uint32_t idx[16];
	size_t n;

	MT_ASSERT_EQ(0, append_range(s, 0, 10));
	MT_ASSERT_EQ(10, count_of(s));
	MT_ASSERT_EQ(0, drain(s, idx, 16, &n));
	MT_ASSERT_EQ(10, n);
	for (size_t i = 0; i < n; i++) {
		MT_ASSERT_EQ(i, idx[i]);
	}
	MT_ASSERT_EQ(0, count_of(s));
}

MT_TEST(c1_append_rejects_invalid_entry_counts)
{
	struct flash_ring_store *s = boot(&contact_cfg);
	uint8_t one[5] = { 0 };

	MT_ASSERT_EQ(-EINVAL, flash_ring_store_append(s, NULL, 1));
	MT_ASSERT_EQ(-EINVAL, flash_ring_store_append(s, one, 0));
	MT_ASSERT_EQ(-EINVAL, append_range(s, 0, NETWORK_STORAGE_BLOCK_CONTACTS + 1));
	MT_ASSERT_EQ(0, append_range(s, 0, NETWORK_STORAGE_BLOCK_CONTACTS));
	MT_ASSERT_EQ(NETWORK_STORAGE_BLOCK_CONTACTS, count_of(s));
}

MT_TEST(c1_fixed_batch_store_rejects_other_batch_sizes)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	MT_ASSERT_EQ(-EINVAL, append_range(s, 0, CONFIG_DSA_SELF_REPORT_FLUSH_BATCH - 1));
	MT_ASSERT_EQ(0, append_range(s, 0, CONFIG_DSA_SELF_REPORT_FLUSH_BATCH));
	MT_ASSERT_EQ(CONFIG_DSA_SELF_REPORT_FLUSH_BATCH, count_of(s));
}

MT_TEST(c1_peek_returns_whole_entries_only)
{
	struct flash_ring_store *s = boot(&contact_cfg);
	uint8_t buf[16];
	uint16_t written = 0xffff;

	MT_ASSERT_EQ(0, append_range(s, 0, 3));
	MT_ASSERT_EQ(0, flash_ring_store_peek(s, buf, 4, &written));
	MT_ASSERT_EQ(0, written);
	MT_ASSERT_EQ(0, flash_ring_store_peek(s, buf, 12, &written));
	MT_ASSERT_EQ(10, written);
	MT_ASSERT_EQ(-EINVAL, flash_ring_store_peek(s, NULL, 12, &written));
}

MT_TEST(c1_partial_drop_advances_within_block)
{
	struct flash_ring_store *s = boot(&small_cfg);
	uint8_t buf[SMALL_CAPACITY * SMALL_ENTRY];
	uint16_t written;
	bool retired = true;

	MT_ASSERT_EQ(0, append_range(s, 0, 4));
	MT_ASSERT_EQ(0, flash_ring_store_drop(s, 2, &retired));
	MT_ASSERT(!retired);
	MT_ASSERT_EQ(2, count_of(s));
	MT_ASSERT_EQ(0, flash_ring_store_peek(s, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(2 * SMALL_ENTRY, written);
	MT_ASSERT_EQ(2, entry_index(buf, SMALL_ENTRY));
}

MT_TEST(c1_drop_more_than_remaining_in_oldest_block_is_rejected)
{
	struct flash_ring_store *s = boot(&small_cfg);
	bool retired;

	MT_ASSERT_EQ(0, append_range(s, 0, 4));
	MT_ASSERT_EQ(0, append_range(s, 4, 4));
	MT_ASSERT_EQ(-EINVAL, flash_ring_store_drop(s, 5, &retired));
	MT_ASSERT_EQ(8, count_of(s));
}

MT_TEST(c1_drop_on_empty_store_returns_enoent)
{
	struct flash_ring_store *s = boot(&small_cfg);
	bool retired;

	MT_ASSERT_EQ(-ENOENT, flash_ring_store_drop(s, 1, &retired));
	MT_ASSERT_EQ(0, flash_ring_store_drop(s, 0, &retired));
	MT_ASSERT_EQ(-EINVAL, flash_ring_store_drop(s, 1, NULL));
}

MT_TEST(c1_full_block_drop_retires_and_deletes_block_record)
{
	struct flash_ring_store *s = boot(&small_cfg);
	bool retired = false;

	MT_ASSERT_EQ(0, append_range(s, 0, 4));
	MT_ASSERT(fake_nvs_has(SMALL_OFFSET, FLASH_RING_STORE_BLOCK_ID_BASE + 0));
	MT_ASSERT_EQ(0, flash_ring_store_drop(s, 4, &retired));
	MT_ASSERT(retired);
	MT_ASSERT(!fake_nvs_has(SMALL_OFFSET, FLASH_RING_STORE_BLOCK_ID_BASE + 0));
	MT_ASSERT_EQ(0, count_of(s));
}

MT_TEST(c1_fifo_order_across_blocks)
{
	struct flash_ring_store *s = boot(&small_cfg);
	uint32_t idx[16];
	size_t n;

	MT_ASSERT_EQ(0, append_range(s, 0, 4));
	MT_ASSERT_EQ(0, append_range(s, 4, 3));
	MT_ASSERT_EQ(0, append_range(s, 7, 2));
	MT_ASSERT_EQ(9, count_of(s));
	MT_ASSERT_EQ(0, drain(s, idx, 16, &n));
	MT_ASSERT_EQ(9, n);
	for (size_t i = 0; i < n; i++) {
		MT_ASSERT_EQ(i, idx[i]);
	}
}

MT_TEST(c1_full_store_discards_oldest_block_on_append)
{
	struct flash_ring_store *s = boot(&small_cfg);
	uint32_t idx[32];
	size_t n;

	for (uint32_t b = 0; b < 5; b++) {
		MT_ASSERT_EQ(0, append_range(s, b * 4, 4));
	}
	MT_ASSERT_EQ(SMALL_BLOCKS * 4, count_of(s));
	MT_ASSERT_EQ(0, drain(s, idx, 32, &n));
	MT_ASSERT_EQ(12, n);
	MT_ASSERT_EQ(8, idx[0]); /* blocks 0 and 1 were discarded */
	MT_ASSERT_EQ(19, idx[11]);
}

MT_TEST(c1_long_run_wraps_block_ids_and_stays_fifo)
{
	struct flash_ring_store *s = boot(&small_cfg);
	uint32_t next_write = 0;
	uint32_t next_read = 0;

	for (int round = 0; round < 200; round++) {
		uint32_t idx[16];
		size_t n;
		uint16_t count = (uint16_t)(1 + round % SMALL_CAPACITY);

		MT_ASSERT_EQ(0, append_range(s, next_write, count));
		next_write += count;
		if (round % 3 == 2) {
			MT_ASSERT_EQ(0, drain(s, idx, 16, &n));
			for (size_t i = 0; i < n; i++) {
				MT_ASSERT_EQ(next_read + i, idx[i]);
			}
			next_read += (uint32_t)n;
		}
	}
	MT_ASSERT(fake_nvs_record_count(SMALL_OFFSET) <= SMALL_BLOCKS + 1);
}

MT_TEST(c1_fixed_batch_entries_survive_reboot)
{
	struct flash_ring_store *s = boot(&batch_cfg);
	uint32_t idx[256];
	size_t n;

	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	MT_ASSERT_EQ(0, append_range(s, 64, 64));
	s = reboot(&batch_cfg);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(128, count_of(s));
	MT_ASSERT_EQ(0, drain(s, idx, 256, &n));
	MT_ASSERT_EQ(128, n);
	MT_ASSERT_EQ(0, idx[0]);
	MT_ASSERT_EQ(127, idx[127]);
}

MT_TEST(c1_contacts_survive_reboot)
{
	/* Production contact configuration (flush_batch = 0). Contacts that
	 * were flushed to flash must still be there after a reset.
	 */
	struct flash_ring_store *s = boot(&contact_cfg);
	uint32_t idx[64];
	size_t n;

	MT_ASSERT_EQ(0, append_range(s, 0, 10));
	MT_ASSERT_EQ(0, append_range(s, 10, 20));
	s = reboot(&contact_cfg);
	{
		int err = flash_ring_store_init(s);

		MT_ASSERT_MSG(err == 0,
			      "init after reset returned %d: the metadata check "
			      "'pending_entries > pending_blocks * flush_batch' rejects "
			      "every non-empty contact store (flush_batch == 0), "
			      "nvs_clear() wipes the contacts and the following "
			      "metadata write fails with -EACCES", err);
	}
	MT_ASSERT_MSG(count_of(s) == 30,
		      "30 contacts stored before reset, %u after reset "
		      "(metadata check 'pending_entries > pending_blocks * flush_batch' "
		      "treats flush_batch == 0 as invalid and clears the store)",
		      (unsigned int)count_of(s));
	MT_ASSERT_EQ(0, drain(s, idx, 64, &n));
	MT_ASSERT_EQ(30, n);
}

MT_TEST(c1_partial_drop_survives_reboot_after_sync)
{
	struct flash_ring_store *s = boot(&batch_cfg);
	uint8_t buf[3];
	uint16_t written;
	bool retired;

	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	MT_ASSERT_EQ(0, append_range(s, 64, 64));
	MT_ASSERT_EQ(0, flash_ring_store_drop(s, 10, &retired));
	MT_ASSERT_EQ(0, flash_ring_store_sync(s));
	s = reboot(&batch_cfg);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(118, count_of(s));
	MT_ASSERT_EQ(0, flash_ring_store_peek(s, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(10, entry_index(buf, 3));
}

MT_TEST(c1_partial_drop_without_sync_is_resent_after_reboot)
{
	/* DECISIONS 2026-08-03: partial drops are only checkpointed by
	 * flash_ring_store_sync(); a reset before that resends them.
	 */
	struct flash_ring_store *s = boot(&batch_cfg);
	uint8_t buf[3];
	uint16_t written;
	bool retired;

	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	MT_ASSERT_EQ(0, flash_ring_store_drop(s, 10, &retired));
	MT_ASSERT_EQ(54, count_of(s));
	s = reboot(&batch_cfg);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(64, count_of(s));
	MT_ASSERT_EQ(0, flash_ring_store_peek(s, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(0, entry_index(buf, 3));
}

MT_TEST(c1_full_block_drop_is_durable_without_sync)
{
	struct flash_ring_store *s = boot(&batch_cfg);
	bool retired;

	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	MT_ASSERT_EQ(0, append_range(s, 64, 64));
	MT_ASSERT_EQ(0, flash_ring_store_drop(s, 64, &retired));
	MT_ASSERT(retired);
	s = reboot(&batch_cfg);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(64, count_of(s));
}

MT_TEST(c1_lost_metadata_is_rebuilt_from_blocks)
{
	struct flash_ring_store *s = boot(&batch_cfg);
	uint32_t idx[256];
	size_t n;

	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	MT_ASSERT_EQ(0, append_range(s, 64, 64));
	MT_ASSERT_EQ(0, append_range(s, 128, 64));
	/* Simulate a missing metadata record (e.g. never written). */
	{
		struct nvs_fs fs = { .offset = batch_cfg.offset, .sector_size = 4096,
				     .sector_count = 2, .ready = true };
		MT_ASSERT_EQ(0, nvs_delete(&fs, FLASH_RING_STORE_META_ID));
	}
	s = reboot(&batch_cfg);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(192, count_of(s));
	MT_ASSERT_EQ(0, drain(s, idx, 256, &n));
	MT_ASSERT_EQ(192, n);
	MT_ASSERT_EQ(0, idx[0]);
}

MT_TEST(c1_block_written_before_power_cut_is_recovered)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	/* Next append: block record lands, metadata write is cut. */
	fake_nvs_cut_power_after(1);
	MT_ASSERT(append_range(s, 64, 64) != 0);
	s = reboot(&batch_cfg);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(128, count_of(s));
}

MT_TEST(c1_incompatible_format_is_cleared_and_store_usable_in_same_boot)
{
	/* A firmware update that changes magic/version must clear old data
	 * and leave the store usable without another reboot.
	 */
	struct flash_ring_store *s = boot(&batch_cfg);
	struct flash_ring_store_config old_cfg = batch_cfg;

	old_cfg.version = batch_cfg.version - 1;
	s = boot(&old_cfg);
	MT_ASSERT_EQ(0, append_range(s, 0, 64));

	s = reboot(&batch_cfg);
	MT_ASSERT_MSG(flash_ring_store_init(s) == 0,
		      "init after format change failed: nvs_clear() leaves NVS "
		      "unmounted, so the following metadata write returns -EACCES");
	MT_ASSERT_EQ(0, count_of(s));
	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	MT_ASSERT_EQ(64, count_of(s));
}

MT_TEST(c1_unmountable_partition_is_erased_once_and_mounted)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	fake_nvs_corrupt_partition(batch_cfg.offset);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT_EQ(1, shim_flash_erase_count());
	MT_ASSERT_EQ(0, count_of(s));
	MT_ASSERT(!(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_INIT));
}

MT_TEST(c1_init_failure_sets_and_recovery_clears_init_fault)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	shim_flash_set_page_size(2048);
	MT_ASSERT_EQ(-ENOTSUP, flash_ring_store_init(s));
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_INIT);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_STORAGE_ERROR);
	shim_flash_set_page_size(4096);
	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	MT_ASSERT(!(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_INIT));
}

MT_TEST(c1_flash_device_not_ready_fails_init)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	shim_flash_device.ready = false;
	MT_ASSERT_EQ(-ENODEV, flash_ring_store_init(s));
	shim_flash_device.ready = true;
}

MT_TEST(c1_write_error_sets_write_fault_and_keeps_count)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	MT_ASSERT_EQ(0, flash_ring_store_init(s));
	fake_nvs_set_write_error(-EIO);
	MT_ASSERT_EQ(-EIO, append_range(s, 0, 64));
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_WRITE);
	MT_ASSERT_EQ(0, count_of(s));
	fake_nvs_set_write_error(0);
	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	MT_ASSERT(!(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_WRITE));
}

MT_TEST(c1_read_error_sets_read_fault)
{
	struct flash_ring_store *s = boot(&batch_cfg);
	uint8_t buf[12];
	uint16_t written;

	MT_ASSERT_EQ(0, append_range(s, 0, 64));
	fake_nvs_set_read_error(-EIO, FLASH_RING_STORE_BLOCK_ID_BASE);
	MT_ASSERT_EQ(-EIO, flash_ring_store_peek(s, buf, sizeof(buf), &written));
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_READ);
	fake_nvs_set_read_error(0, -1);
	MT_ASSERT_EQ(0, flash_ring_store_peek(s, buf, sizeof(buf), &written));
	MT_ASSERT(!(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_READ));
}

MT_TEST(c1_delete_error_is_cleanup_only)
{
	struct flash_ring_store *s = boot(&small_cfg);
	bool retired = false;

	MT_ASSERT_EQ(0, append_range(s, 0, 4));
	fake_nvs_set_delete_error(-EIO);
	MT_ASSERT_EQ(0, flash_ring_store_drop(s, 4, &retired));
	MT_ASSERT(retired);
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_ECO_LOG_DELETE);
	MT_ASSERT_EQ(0, count_of(s));
}

MT_TEST(c1_network_storage_wrapper_validates_byte_lengths)
{
	uint8_t buf[10] = { 0 };

	MT_ASSERT_EQ(-EINVAL, network_storage_append_block(buf, 0));
	MT_ASSERT_EQ(-EINVAL, network_storage_append_block(buf, 7));
	MT_ASSERT_EQ(0, network_storage_append_block(buf, 10));
	MT_ASSERT_EQ(0, network_storage_drop(4)); /* < one contact: no-op */
	{
		uint32_t count;

		MT_ASSERT_EQ(0, network_storage_get_contact_count(&count));
		MT_ASSERT_EQ(2, count);
	}
	MT_ASSERT_EQ(0, network_storage_drop(7)); /* rounds down to 1 contact */
	{
		uint32_t count;

		MT_ASSERT_EQ(0, network_storage_get_contact_count(&count));
		MT_ASSERT_EQ(1, count);
	}
}

/* Power-cut sweep: run a fixed script of appends/drops/syncs, cutting power
 * after every possible number of flash writes, then reboot and check that
 * the store is consistent: it initialises, its count matches what can be
 * drained, entries are uncorrupted, strictly consecutive and no successfully
 * appended, not-yet-dropped entry is lost.
 */
static void power_cut_sweep(const struct flash_ring_store_config *cfg,
			    uint16_t batch)
{
	int total_ops = -1;

	for (int cut = 0;; cut++) {
		struct flash_ring_store *s;
		uint32_t appended_ok_end = 0; /* index after last successful append */
		uint32_t dropped_attempted = 0;
		uint32_t next = 0;
		bool retired;
		uint32_t idx[1024];
		size_t n;
		int ops_before;

		fake_nvs_reset_all();
		device_set_storage_fault(0xFFFFFFFFU, false);
		s = boot(cfg);
		ops_before = fake_nvs_write_count() + fake_nvs_delete_count();
		if (total_ops >= 0) {
			fake_nvs_cut_power_after(cut);
		}

#define APPEND()                                                              \
	do {                                                                  \
		if (append_range(s, next, batch) == 0) {                      \
			appended_ok_end = next + batch;                       \
		}                                                             \
		next += batch;                                                \
	} while (0)
#define DROP(k)                                                               \
	do {                                                                  \
		if (flash_ring_store_drop(s, (k), &retired) == 0) {           \
			dropped_attempted += (k);                             \
		}                                                             \
	} while (0)

		APPEND();
		APPEND();
		DROP(10);
		(void)flash_ring_store_sync(s);
		DROP(batch - 10);
		APPEND();
		DROP(20);
		APPEND();
		APPEND();
		(void)flash_ring_store_sync(s);
		DROP(batch - 20);
#undef APPEND
#undef DROP

		if (total_ops < 0) {
			total_ops = fake_nvs_write_count() + fake_nvs_delete_count() -
				    ops_before;
			continue; /* dry run established the number of writes */
		}

		s = reboot(cfg);
		MT_ASSERT_MSG(flash_ring_store_init(s) == 0,
			      "cut after %d writes: init failed", cut);
		{
			uint32_t count = count_of(s);
			int err = drain(s, idx, 1024, &n);

			MT_ASSERT_MSG(err == 0, "cut after %d writes: drain error %d", cut, err);
			MT_ASSERT_MSG(count == n,
				      "cut after %d writes: count %u but %u drainable",
				      cut, (unsigned int)count, (unsigned int)n);
		}
		for (size_t i = 1; i < n; i++) {
			MT_ASSERT_MSG(idx[i] == idx[i - 1] + 1,
				      "cut after %d writes: gap/reorder at %u",
				      cut, (unsigned int)i);
		}
		if (n > 0) {
			MT_ASSERT_MSG(idx[0] <= dropped_attempted,
				      "cut after %d writes: undropped entries lost (first=%u, dropped=%u)",
				      cut, (unsigned int)idx[0], (unsigned int)dropped_attempted);
			MT_ASSERT_MSG(idx[n - 1] + 1 >= appended_ok_end,
				      "cut after %d writes: appended entries lost (last=%u, appended=%u)",
				      cut, (unsigned int)idx[n - 1], (unsigned int)appended_ok_end);
		} else {
			MT_ASSERT_MSG(appended_ok_end <= dropped_attempted,
				      "cut after %d writes: all entries lost (appended %u, dropped %u)",
				      cut, (unsigned int)appended_ok_end,
				      (unsigned int)dropped_attempted);
		}
		if (cut > total_ops + 1) {
			break;
		}
	}
}

MT_TEST(c1_power_cut_at_every_write_fixed_batch)
{
	power_cut_sweep(&batch_cfg, 64);
}

MT_TEST(c1_power_cut_at_every_write_contacts)
{
	power_cut_sweep(&contact_cfg, 100);
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c1_production_contact_geometry),
	MT_CASE(c1_init_empty_partition_has_no_entries),
	MT_CASE(c1_append_then_peek_returns_entries_in_order),
	MT_CASE(c1_append_rejects_invalid_entry_counts),
	MT_CASE(c1_fixed_batch_store_rejects_other_batch_sizes),
	MT_CASE(c1_peek_returns_whole_entries_only),
	MT_CASE(c1_partial_drop_advances_within_block),
	MT_CASE(c1_drop_more_than_remaining_in_oldest_block_is_rejected),
	MT_CASE(c1_drop_on_empty_store_returns_enoent),
	MT_CASE(c1_full_block_drop_retires_and_deletes_block_record),
	MT_CASE(c1_fifo_order_across_blocks),
	MT_CASE(c1_full_store_discards_oldest_block_on_append),
	MT_CASE(c1_long_run_wraps_block_ids_and_stays_fifo),
	MT_CASE(c1_fixed_batch_entries_survive_reboot),
	MT_CASE(c1_contacts_survive_reboot),
	MT_CASE(c1_partial_drop_survives_reboot_after_sync),
	MT_CASE(c1_partial_drop_without_sync_is_resent_after_reboot),
	MT_CASE(c1_full_block_drop_is_durable_without_sync),
	MT_CASE(c1_lost_metadata_is_rebuilt_from_blocks),
	MT_CASE(c1_block_written_before_power_cut_is_recovered),
	MT_CASE(c1_incompatible_format_is_cleared_and_store_usable_in_same_boot),
	MT_CASE(c1_unmountable_partition_is_erased_once_and_mounted),
	MT_CASE(c1_init_failure_sets_and_recovery_clears_init_fault),
	MT_CASE(c1_flash_device_not_ready_fails_init),
	MT_CASE(c1_write_error_sets_write_fault_and_keeps_count),
	MT_CASE(c1_read_error_sets_read_fault),
	MT_CASE(c1_delete_error_is_cleanup_only),
	MT_CASE(c1_network_storage_wrapper_validates_byte_lengths),
	MT_CASE(c1_power_cut_at_every_write_fixed_batch),
	MT_CASE(c1_power_cut_at_every_write_contacts))
