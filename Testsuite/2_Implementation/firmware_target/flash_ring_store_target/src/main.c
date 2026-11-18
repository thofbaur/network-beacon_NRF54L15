/*
 * F1 - flash_ring_store.c on the real nRF54L15 flash/NVS (DK A).
 *
 * Mirrors the key C1 cases on hardware so the host fake (fake_nvs.c) is
 * validated against real NVS semantics, especially nvs_clear() leaving the
 * file system unmounted. Uses the beacon's contact_storage (production
 * contact configuration) and eco_log_storage (fixed-batch configuration)
 * partitions; their content is erased at the start of every test.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/ztest.h>

#include "flash_ring_store.h"

/* device.h's fault hook: record instead of advertising. */
static uint32_t faults;

void device_set_storage_fault(uint32_t fault, bool active)
{
	if (active) {
		faults |= fault;
	} else {
		faults &= ~fault;
	}
}

#define CONTACT_SIZE 5
#define CONTACT_CAPACITY 804
#define CONTACT_BLOCKS ((FIXED_PARTITION_SIZE(contact_storage) / 4096) - 2)

static const struct flash_ring_store_config contact_cfg = {
	.flash_device = FIXED_PARTITION_DEVICE(contact_storage),
	.offset = FIXED_PARTITION_OFFSET(contact_storage),
	.partition_size = FIXED_PARTITION_SIZE(contact_storage),
	.magic = 0x44534143U,
	.version = 5U,
	.entry_size = CONTACT_SIZE,
	.block_entry_capacity = CONTACT_CAPACITY,
	.block_count = CONTACT_BLOCKS,
	.flush_batch = 0,
	.storage_fault_init = BIT(3),
	.storage_fault_read = BIT(4),
	.storage_fault_write = BIT(5),
	.storage_fault_meta = BIT(7),
	.storage_fault_delete = BIT(6),
	.domain_name = "contact",
};

static const struct flash_ring_store_config batch_cfg = {
	.flash_device = FIXED_PARTITION_DEVICE(eco_log_storage),
	.offset = FIXED_PARTITION_OFFSET(eco_log_storage),
	.partition_size = FIXED_PARTITION_SIZE(eco_log_storage),
	.magic = 0x44534145U,
	.version = 1U,
	.entry_size = 6,
	.block_entry_capacity = 16,
	.block_count = FLASH_RING_STORE_BLOCK_COUNT(FIXED_PARTITION_SIZE(eco_log_storage)),
	.flush_batch = 16,
	.storage_fault_init = BIT(13),
	.storage_fault_read = BIT(14),
	.storage_fault_write = BIT(15),
	.storage_fault_meta = BIT(17),
	.storage_fault_delete = BIT(16),
	.domain_name = "eco log",
};

static uint8_t cache[FLASH_RING_STORE_BLOCK_SIZE(CONTACT_CAPACITY, CONTACT_SIZE)] __aligned(4);
static K_MUTEX_DEFINE(lock);
static struct flash_ring_store store;
static uint8_t entries[CONTACT_CAPACITY * CONTACT_SIZE];

/* Simulated reboot: fresh RAM state, flash content kept. */
static struct flash_ring_store *boot(const struct flash_ring_store_config *cfg)
{
	memset(&store, 0, sizeof(store));
	store.cfg = cfg;
	store.lock = &lock;
	store.block_cache = cache;
	store.block_cache_size = FLASH_RING_STORE_BLOCK_SIZE(cfg->block_entry_capacity,
							     cfg->entry_size);
	return &store;
}

static void erase(const struct flash_ring_store_config *cfg)
{
	zassert_ok(flash_erase(cfg->flash_device, cfg->offset, cfg->partition_size));
}

static int append(struct flash_ring_store *s, uint32_t first, uint16_t count)
{
	for (uint16_t i = 0; i < count; i++) {
		for (uint16_t b = 0; b < s->cfg->entry_size; b++) {
			entries[i * s->cfg->entry_size + b] = (uint8_t)((first + i) >> (8 * (b % 3)));
		}
	}
	return flash_ring_store_append(s, entries, count);
}

static uint32_t count_of(struct flash_ring_store *s)
{
	uint32_t count = 0;

	zassert_ok(flash_ring_store_get_count(s, &count));
	return count;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	erase(&contact_cfg);
	erase(&batch_cfg);
	faults = 0;
}

ZTEST(flash_ring_store_target, test_f1_fixed_batch_entries_survive_reboot)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	zassert_ok(append(s, 0, 16));
	zassert_ok(append(s, 16, 16));
	s = boot(&batch_cfg);
	zassert_ok(flash_ring_store_init(s));
	zassert_equal(count_of(s), 32);
}

ZTEST(flash_ring_store_target, test_f1_contacts_survive_reboot)
{
	struct flash_ring_store *s = boot(&contact_cfg);

	zassert_ok(append(s, 0, 10));
	zassert_ok(append(s, 10, 20));
	s = boot(&contact_cfg);
	zassert_ok(flash_ring_store_init(s), "init after reset failed (contacts wiped)");
	zassert_equal(count_of(s), 30, "contacts lost across reset");
}

ZTEST(flash_ring_store_target, test_f1_nvs_clear_leaves_fs_unmounted)
{
	/* Validates the host fake's model of Zephyr NVS. */
	struct nvs_fs fs = {
		.flash_device = batch_cfg.flash_device,
		.offset = batch_cfg.offset,
		.sector_size = 4096,
		.sector_count = batch_cfg.partition_size / 4096,
	};
	uint8_t value = 1;

	zassert_ok(nvs_mount(&fs));
	zassert_equal(nvs_write(&fs, 1, &value, 1), 1);
	zassert_ok(nvs_clear(&fs));
	zassert_equal(nvs_write(&fs, 1, &value, 1), -EACCES);
	zassert_ok(nvs_mount(&fs));
	zassert_equal(nvs_read(&fs, 1, &value, 1), -ENOENT);
}

ZTEST(flash_ring_store_target, test_f1_format_change_clears_and_store_usable)
{
	struct flash_ring_store_config old_cfg = batch_cfg;
	struct flash_ring_store *s;

	old_cfg.version = batch_cfg.version - 1;
	s = boot(&old_cfg);
	zassert_ok(append(s, 0, 16));
	s = boot(&batch_cfg);
	zassert_ok(flash_ring_store_init(s), "init after format change failed");
	zassert_equal(count_of(s), 0);
	zassert_ok(append(s, 0, 16));
}

ZTEST(flash_ring_store_target, test_f1_full_store_discards_oldest_block)
{
	struct flash_ring_store *s = boot(&batch_cfg);

	for (uint32_t b = 0; b < batch_cfg.block_count + 2; b++) {
		zassert_ok(append(s, b * 16, 16));
	}
	zassert_equal(count_of(s), batch_cfg.block_count * 16U);
}

ZTEST(flash_ring_store_target, test_f1_full_production_contact_capacity)
{
	struct flash_ring_store *s = boot(&contact_cfg);

	for (uint32_t b = 0; b < CONTACT_BLOCKS; b++) {
		zassert_ok(append(s, b * CONTACT_CAPACITY, CONTACT_CAPACITY), "block %u", b);
	}
	zassert_equal(count_of(s), CONTACT_BLOCKS * CONTACT_CAPACITY);
	zassert_ok(append(s, 0, 1)); /* discards the oldest block */
	zassert_equal(count_of(s), (CONTACT_BLOCKS - 1) * CONTACT_CAPACITY + 1);
}

ZTEST_SUITE(flash_ring_store_target, NULL, NULL, before, NULL, NULL);
