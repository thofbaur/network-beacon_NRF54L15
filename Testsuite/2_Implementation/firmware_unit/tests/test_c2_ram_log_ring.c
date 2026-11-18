/*
 * C2 - ram_log_ring.c: RAM ring of fixed-size entries with threshold-driven
 * flash flushing and flash-then-RAM export (self-reports and eco log use it).
 * The flash side is an in-test fake with error injection so every branch of
 * the ring logic can be reached. See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/kernel.h>

#include "device.h"
#include "fake_peers.h"
#include "minitest.h"
#include "ram_log_ring.h"

#define ENTRY 3
#define RING 8
#define THRESHOLD 6
#define BATCH 4
#define RETRY_MS 5000

/* ---- fake flash domain ------------------------------------------------- */
static uint8_t flash_entries[4096 * ENTRY];
static uint32_t flash_count;
static uint32_t flash_capacity = 4096;
static int append_err;
static int append_err_once;
static int init_err;
static int peek_err;
static int count_err;
static int appends;
static bool drop_retires = true;
static void (*during_append)(void);

static int fs_init(void)
{
	return init_err;
}

static int fs_append(const uint8_t *entries, uint16_t count)
{
	appends++;
	if (during_append) {
		during_append();
	}
	if (append_err_once) {
		int err = append_err_once;

		append_err_once = 0;
		return err;
	}
	if (append_err) {
		return append_err;
	}
	if (flash_count + count > flash_capacity) {
		return -ENOSPC;
	}
	memcpy(&flash_entries[flash_count * ENTRY], entries, (size_t)count * ENTRY);
	flash_count += count;
	return 0;
}

static int fs_peek(uint8_t *buffer, uint16_t len, uint16_t *written)
{
	uint16_t n;

	if (peek_err) {
		return peek_err;
	}
	n = (uint16_t)MIN((uint32_t)(len / ENTRY), flash_count);
	memcpy(buffer, flash_entries, (size_t)n * ENTRY);
	*written = n * ENTRY;
	return 0;
}

static int fs_drop(uint16_t count, bool *retired)
{
	if (count > flash_count) {
		return -EINVAL;
	}
	memmove(flash_entries, &flash_entries[count * ENTRY],
		(flash_count - count) * ENTRY);
	flash_count -= count;
	*retired = drop_retires;
	return 0;
}

static int syncs;
static int fs_sync(void)
{
	syncs++;
	return 0;
}

static int fs_count(uint32_t *count)
{
	if (count_err) {
		return count_err;
	}
	*count = flash_count;
	return 0;
}

static const struct ram_log_ring_config cfg = {
	.ring_count = RING,
	.entry_size = ENTRY,
	.flush_threshold = THRESHOLD,
	.flush_batch = BATCH,
	.flush_retry_ms = RETRY_MS,
	.storage_init = fs_init,
	.storage_append = fs_append,
	.storage_peek = fs_peek,
	.storage_drop = fs_drop,
	.storage_sync = fs_sync,
	.storage_get_count = fs_count,
	.storage_full_bit = STORAGE_FULL_SELF_REPORT,
	.domain_name = "test",
};

static uint8_t ring_entries[RING * ENTRY];
static uint8_t ring_flush[BATCH * ENTRY];
static K_MUTEX_DEFINE(ring_lock);
static struct ram_log_ring ring;

static void setup(void)
{
	memset(&ring, 0, sizeof(ring));
	ring.cfg = &cfg;
	ring.lock = &ring_lock;
	ring.entries = ring_entries;
	ring.flush_buffer = ring_flush;
	ram_log_ring_init(&ring);
	flash_count = 0;
	flash_capacity = 4096;
	append_err = append_err_once = init_err = peek_err = count_err = 0;
	appends = 0;
	syncs = 0;
	drop_retires = true;
	during_append = NULL;
	shim_work_reset();
	shim_set_uptime_ms(0);
	fake_peers_reset();
	device_set_storage_full(0xFFFFFFFFU, false);
}

static bool push(uint32_t i)
{
	uint8_t e[ENTRY] = { (uint8_t)(i >> 16), (uint8_t)(i >> 8), (uint8_t)i };

	return ram_log_ring_push(&ring, e);
}

static uint32_t value_at(const uint8_t *buf, size_t k)
{
	return ((uint32_t)buf[k * ENTRY] << 16) | ((uint32_t)buf[k * ENTRY + 1] << 8) |
	       buf[k * ENTRY + 2];
}

static uint16_t total(void)
{
	uint16_t count = 0;

	MT_ASSERT_EQ(0, ram_log_ring_get_count(&ring, &count));
	return count;
}

/* ---------------------------------------------------------------------- */

MT_TEST(c2_push_then_export_from_ram_in_order)
{
	uint8_t buf[64];
	uint16_t written;

	MT_ASSERT(push(10) && push(11) && push(12));
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(3 * ENTRY, written);
	MT_ASSERT_EQ(10, value_at(buf, 0));
	MT_ASSERT_EQ(12, value_at(buf, 2));
	MT_ASSERT_EQ(0, ram_log_ring_export_commit(&ring));
	MT_ASSERT_EQ(0, total());
}

MT_TEST(c2_export_abort_keeps_entries)
{
	uint8_t buf[64];
	uint16_t written;

	push(1);
	push(2);
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	ram_log_ring_export_abort(&ring);
	MT_ASSERT_EQ(2, total());
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(1, value_at(buf, 0));
}

MT_TEST(c2_export_limited_by_buffer_size)
{
	uint8_t buf[8]; /* room for two entries (8 % 3 dropped) */
	uint16_t written;

	push(1);
	push(2);
	push(3);
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(2 * ENTRY, written);
	MT_ASSERT_EQ(0, ram_log_ring_export_commit(&ring));
	MT_ASSERT_EQ(1, total());
}

MT_TEST(c2_overflow_without_flush_overwrites_oldest)
{
	uint8_t buf[64];
	uint16_t written;

	for (uint32_t i = 0; i < 10; i++) {
		MT_ASSERT(push(i));
	}
	MT_ASSERT_EQ(RING, total());
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(RING * ENTRY, written);
	MT_ASSERT_EQ(2, value_at(buf, 0));
	MT_ASSERT_EQ(9, value_at(buf, RING - 1));
}

MT_TEST(c2_push_dropped_while_full_and_export_active)
{
	uint8_t buf[64];
	uint16_t written;

	for (uint32_t i = 0; i < RING; i++) {
		push(i);
	}
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT(!push(100));
	MT_ASSERT_EQ(0, ram_log_ring_export_commit(&ring));
	MT_ASSERT(push(101));
	MT_ASSERT_EQ(1, total());
}

MT_TEST(c2_flush_moves_one_batch_to_flash_at_threshold)
{
	for (uint32_t i = 0; i < THRESHOLD - 1; i++) {
		push(i);
	}
	shim_run_ready_work();
	MT_ASSERT_EQ(0, appends);
	push(THRESHOLD - 1);
	shim_run_ready_work();
	MT_ASSERT_EQ(1, appends);
	MT_ASSERT_EQ(BATCH, flash_count);
	MT_ASSERT_EQ(0, value_at(flash_entries, 0));
	MT_ASSERT_EQ(THRESHOLD, total());
}

MT_TEST(c2_export_drains_flash_before_ram)
{
	uint8_t buf[64];
	uint16_t written;

	for (uint32_t i = 0; i < THRESHOLD; i++) {
		push(i);
	}
	shim_run_ready_work();
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(BATCH * ENTRY, written);
	MT_ASSERT_EQ(0, value_at(buf, 0));
	MT_ASSERT_EQ(0, ram_log_ring_export_commit(&ring));
	MT_ASSERT_EQ(0, flash_count);
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ((THRESHOLD - BATCH) * ENTRY, written);
	MT_ASSERT_EQ(BATCH, value_at(buf, 0));
	MT_ASSERT_EQ(0, ram_log_ring_export_commit(&ring));
	MT_ASSERT_EQ(0, total());
}

MT_TEST(c2_flush_error_retries_after_retry_delay)
{
	append_err_once = -EIO;
	for (uint32_t i = 0; i < THRESHOLD; i++) {
		push(i);
	}
	shim_run_ready_work();
	MT_ASSERT_EQ(1, appends);
	MT_ASSERT_EQ(0, flash_count);
	MT_ASSERT_EQ(THRESHOLD, total());
	shim_advance_ms(RETRY_MS - 1);
	MT_ASSERT_EQ(1, appends);
	shim_advance_ms(1);
	MT_ASSERT_EQ(2, appends);
	MT_ASSERT_EQ(BATCH, flash_count);
}

MT_TEST(c2_enospc_marks_storage_full_and_keeps_ram)
{
	uint8_t buf[64];
	uint16_t written;

	flash_capacity = 0;
	for (uint32_t i = 0; i < THRESHOLD; i++) {
		push(i);
	}
	shim_run_ready_work();
	MT_ASSERT(device_get_storage_full() & STORAGE_FULL_SELF_REPORT);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_STORAGE_FULL);
	MT_ASSERT_EQ(THRESHOLD, total());
	/* While full, no further flush attempts are scheduled. */
	push(50);
	shim_advance_ms(10 * RETRY_MS);
	MT_ASSERT_EQ(1, appends);
	/* A flash export that retires a block clears the full state. */
	flash_capacity = 4096;
	flash_count = 1;
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(0, ram_log_ring_export_commit(&ring));
	MT_ASSERT(!(device_get_storage_full() & STORAGE_FULL_SELF_REPORT));
}

MT_TEST(c2_count_sums_flash_and_ram_and_saturates)
{
	push(1);
	push(2);
	flash_count = 10;
	MT_ASSERT_EQ(12, total());
	flash_count = 70000;
	MT_ASSERT_EQ(UINT16_MAX, total());
	flash_count = 0;
}

MT_TEST(c2_second_export_begin_is_busy)
{
	uint8_t buf[64];
	uint16_t written;

	push(1);
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(-EBUSY, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
}

MT_TEST(c2_commit_without_export_is_rejected)
{
	MT_ASSERT_EQ(-EINVAL, ram_log_ring_export_commit(&ring));
}

MT_TEST(c2_empty_export_leaves_no_active_reservation)
{
	uint8_t buf[64];
	uint16_t written = 99;

	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(0, written);
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, 2, &written));
	MT_ASSERT_EQ(0, written);
}

MT_TEST(c2_flush_deferred_while_export_active_then_resumes)
{
	uint8_t buf[3];
	uint16_t written;

	push(0);
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	for (uint32_t i = 1; i < THRESHOLD + 1; i++) {
		push(i);
	}
	shim_run_ready_work();
	MT_ASSERT_EQ(0, appends);
	MT_ASSERT_EQ(0, ram_log_ring_export_commit(&ring));
	shim_run_ready_work();
	MT_ASSERT_EQ(1, appends);
	MT_ASSERT_EQ(1, value_at(flash_entries, 0));
}

static void fill_ring_during_append(void)
{
	during_append = NULL;
	while (total() < RING) {
		MT_ASSERT(push(900));
	}
	MT_ASSERT(!push(901)); /* full while flush in progress: dropped */
}

MT_TEST(c2_push_dropped_when_full_during_flush)
{
	for (uint32_t i = 0; i < THRESHOLD; i++) {
		push(i);
	}
	during_append = fill_ring_during_append;
	shim_run_ready_work();
	MT_ASSERT_EQ(BATCH, flash_count);
	MT_ASSERT(shim_log_contains("dropping newest entry"));
}

MT_TEST(c2_storage_init_error_fails_export)
{
	uint8_t buf[64];
	uint16_t written;

	push(1);
	init_err = -EIO;
	MT_ASSERT_EQ(-EIO, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	init_err = 0;
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
}

MT_TEST(c2_flash_peek_error_aborts_export)
{
	uint8_t buf[64];
	uint16_t written;

	flash_count = 2;
	peek_err = -EIO;
	MT_ASSERT_EQ(-EIO, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	peek_err = 0;
	MT_ASSERT_EQ(0, ram_log_ring_export_begin(&ring, buf, sizeof(buf), &written));
	MT_ASSERT_EQ(2 * ENTRY, written);
}

MT_TEST(c2_sync_delegates_to_storage)
{
	MT_ASSERT_EQ(0, ram_log_ring_sync(&ring));
	MT_ASSERT_EQ(1, syncs);
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c2_push_then_export_from_ram_in_order),
	MT_CASE(c2_export_abort_keeps_entries),
	MT_CASE(c2_export_limited_by_buffer_size),
	MT_CASE(c2_overflow_without_flush_overwrites_oldest),
	MT_CASE(c2_push_dropped_while_full_and_export_active),
	MT_CASE(c2_flush_moves_one_batch_to_flash_at_threshold),
	MT_CASE(c2_export_drains_flash_before_ram),
	MT_CASE(c2_flush_error_retries_after_retry_delay),
	MT_CASE(c2_enospc_marks_storage_full_and_keeps_ram),
	MT_CASE(c2_count_sums_flash_and_ram_and_saturates),
	MT_CASE(c2_second_export_begin_is_busy),
	MT_CASE(c2_commit_without_export_is_rejected),
	MT_CASE(c2_empty_export_leaves_no_active_reservation),
	MT_CASE(c2_flush_deferred_while_export_active_then_resumes),
	MT_CASE(c2_push_dropped_when_full_during_flush),
	MT_CASE(c2_storage_init_error_fails_export),
	MT_CASE(c2_flash_peek_error_aborts_export),
	MT_CASE(c2_sync_delegates_to_storage))
