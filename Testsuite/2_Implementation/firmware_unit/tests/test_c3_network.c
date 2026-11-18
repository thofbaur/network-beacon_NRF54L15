/*
 * C3 - network.c: contact policy, RAM ring + flash flush, export, network
 * parameters (commands, persistence, migration) and the advertised data
 * level. Linked with the real network_storage.c, flash_ring_store.c,
 * param_storage.c and device.c on top of the NVS/settings fakes.
 * See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "common_include.h"
#include "device.h"
#include "fake_nvs.h"
#include "fake_peers.h"
#include "fake_settings.h"
#include "flash_ring_store.h"
#include "minitest.h"
#include "network.h"
#include "network_storage.h"

#define RING CONFIG_DSA_NETWORK_RING_COUNT
#define THRESHOLD CONFIG_DSA_NETWORK_FLUSH_THRESHOLD
#define DEFAULT_RSSI CONFIG_DSA_NETWORK_DEFAULT_RSSI_THRESHOLD
#define DEFAULT_RSSI_LOC CONFIG_DSA_NETWORK_DEFAULT_RSSI_THRESHOLD_LOCATION
#define NUS_PAYLOAD 240 /* contact bytes per NUS packet at MTU 247 */

static void setup(void)
{
	fake_nvs_reset_all();
	fake_settings_reset();
	fake_peers_reset();
	shim_work_reset();
	shim_set_uptime_ms(0);
	shim_log_clear();
	device_set_network_status(0);
	device_set_radio_status(0);
	device_set_storage_fault(0xFFFFFFFFU, false);
	device_set_storage_full(0xFFFFFFFFU, false);
}

static uint32_t contacts(void)
{
	uint32_t count = 0;

	MT_ASSERT_EQ(0, network_get_contact_count(&count));
	return count;
}

static void command(uint8_t parameter, uint16_t value)
{
	network_command_begin();
	network_apply_command(parameter, value);
	network_command_commit();
}

/* Fast synthetic fill: id = i & 0xff, time = i, rssi = 40 + i % 50.
 * The storage work queue gets to run every 256 contacts - well inside the
 * 300-entry headroom between flush threshold and RAM ring size, as on the
 * device where the flush runs long before the next contacts arrive.
 */
static void fill(uint32_t first, uint32_t count)
{
	for (uint32_t i = first; i < first + count; i++) {
		network_development_validation_append_contact((uint8_t)i, i,
							      (uint8_t)(40 + i % 50));
		if ((i & 0xff) == 0) {
			shim_run_ready_work();
		}
	}
	shim_run_ready_work();
}

/* Export everything (flash first, then RAM) like nus.c does. */
static uint32_t export_all(uint32_t *first_time, uint32_t *last_time, bool *ordered)
{
	uint8_t buf[NUS_PAYLOAD];
	uint32_t n = 0;
	uint32_t prev = 0;

	*ordered = true;
	for (;;) {
		uint16_t written = 0;

		MT_ASSERT_EQ(0, network_contact_export_begin(buf, sizeof(buf), &written));
		if (written == 0) {
			break;
		}
		for (uint16_t off = 0; off < written; off += 5) {
			uint32_t t = sys_get_be24(&buf[off + 1]);

			if (n == 0) {
				*first_time = t;
			} else if (t != prev + 1) {
				*ordered = false;
			}
			prev = t;
			n++;
		}
		MT_ASSERT_EQ(0, network_contact_export_commit());
		shim_run_ready_work();
	}
	*last_time = prev;
	return n;
}

static uint8_t data_level(void)
{
	shim_advance_ms(CONFIG_DSA_NETWORK_STATUS_UPDATE_DELAY_MS + 1);
	return device_get_network_status() & DATA_LEVEL_MASK;
}

static void put_param_record(const char *key, const uint8_t *payload, size_t len)
{
	uint8_t rec[12 + 64];

	sys_put_be32(0x44534150U, &rec[0]);
	sys_put_be16(1U, &rec[4]);
	sys_put_be16((uint16_t)len, &rec[6]);
	sys_put_be32(crc32_ieee(payload, len), &rec[8]);
	memcpy(&rec[12], payload, len);
	fake_settings_put_raw(key, rec, 12 + len);
}

/* Write contacts straight into the contact partition, as a previous boot
 * would have left them.
 */
static void plant_flash_contacts(uint16_t count)
{
	static const struct flash_ring_store_config cfg = {
		.flash_device = &shim_flash_device,
		.offset = SHIM_PARTITION_OFFSET_contact_storage,
		.partition_size = NETWORK_STORAGE_TOTAL_BYTES,
		.magic = 0x44534143U,
		.version = 5U,
		.entry_size = NETWORK_STORAGE_CONTACT_SIZE,
		.block_entry_capacity = NETWORK_STORAGE_BLOCK_CONTACTS,
		.block_count = NETWORK_STORAGE_BLOCK_COUNT,
		.flush_batch = 0,
		.domain_name = "planted",
	};
	static uint8_t cache[FLASH_RING_STORE_BLOCK_SIZE(NETWORK_STORAGE_BLOCK_CONTACTS, 5)]
		__aligned(4);
	static uint8_t data[NETWORK_STORAGE_BLOCK_DATA_LEN];
	struct k_mutex lock = { 0 };
	struct flash_ring_store store = { .cfg = &cfg, .lock = &lock,
					  .block_cache = cache,
					  .block_cache_size = sizeof(cache) };

	for (uint16_t i = 0; i < count; i++) {
		data[i * 5] = (uint8_t)i;
		sys_put_be24(i, &data[i * 5 + 1]);
		data[i * 5 + 4] = 60;
	}
	MT_ASSERT_EQ(0, flash_ring_store_append(&store, data, count));
}

/* ---------------------------------------------------------------------- */

MT_TEST(c3_init_with_empty_storage_uses_defaults)
{
	MT_ASSERT_EQ(0, network_init());
	MT_ASSERT_EQ(0, contacts());
	network_evaluate_contact(7, -DEFAULT_RSSI, false);
	network_evaluate_contact(8, -(DEFAULT_RSSI + 1), false);
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_location_tag_uses_location_threshold)
{
	MT_ASSERT_EQ(0, network_init());
	network_evaluate_contact(9, -DEFAULT_RSSI_LOC, true);
	network_evaluate_contact(9, -(DEFAULT_RSSI_LOC + 1), true);
	network_evaluate_contact(9, -(DEFAULT_RSSI + 1), false);
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_tracking_off_records_nothing)
{
	MT_ASSERT_EQ(0, network_init());
	command(P_TRACKING_ACTIVE, 0);
	network_evaluate_contact(7, -30, false);
	MT_ASSERT_EQ(0, contacts());
	command(P_TRACKING_ACTIVE, 1);
	network_evaluate_contact(7, -30, false);
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_contact_record_encodes_id_uptime_and_rssi_magnitude)
{
	uint8_t buf[10];
	uint16_t written;

	MT_ASSERT_EQ(0, network_init());
	shim_set_uptime_ms(0x123456LL * 1000 + 999);
	network_evaluate_contact(0xAB, -67, false);
	MT_ASSERT_EQ(0, network_contact_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(5, written);
	MT_ASSERT_EQ(0xAB, buf[0]);
	MT_ASSERT_EQ(0x12, buf[1]);
	MT_ASSERT_EQ(0x34, buf[2]);
	MT_ASSERT_EQ(0x56, buf[3]);
	MT_ASSERT_EQ(67, buf[4]);
}

MT_TEST(c3_contact_uptime_is_low_24_bits)
{
	uint8_t buf[5];
	uint16_t written;

	MT_ASSERT_EQ(0, network_init());
	shim_set_uptime_ms(0x01000005LL * 1000);
	network_evaluate_contact(1, -50, false);
	MT_ASSERT_EQ(0, network_contact_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(5, sys_get_be24(&buf[1]));
}

MT_TEST(c3_rssi_command_validation)
{
	MT_ASSERT_EQ(0, network_init());
	command(P_RSSI_NETWORK, 256); /* rejected */
	network_evaluate_contact(1, -(DEFAULT_RSSI + 1), false);
	MT_ASSERT_EQ(0, contacts());
	command(P_RSSI_NETWORK, 90);
	network_evaluate_contact(1, -90, false);
	MT_ASSERT_EQ(1, contacts());
	command(P_RSSI_LOCATION, 300); /* rejected */
	network_evaluate_contact(1, -(DEFAULT_RSSI_LOC + 1), true);
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_tracking_command_rejects_values_above_1)
{
	MT_ASSERT_EQ(0, network_init());
	command(P_TRACKING_ACTIVE, 2);
	network_evaluate_contact(1, -30, false);
	MT_ASSERT_EQ(1, contacts());
	MT_ASSERT(shim_log_contains("Rejecting invalid tracking-active value 2"));
}

MT_TEST(c3_commit_saves_only_when_parameters_change)
{
	MT_ASSERT_EQ(0, network_init());
	command(P_RSSI_NETWORK, DEFAULT_RSSI);
	MT_ASSERT_EQ(0, fake_settings_save_count());
	command(P_RSSI_NETWORK, 70);
	MT_ASSERT_EQ(1, fake_settings_save_count());
	network_command_commit(); /* commit without begin: no-op */
	MT_ASSERT_EQ(1, fake_settings_save_count());
}

MT_TEST(c3_reset_command_restores_defaults)
{
	MT_ASSERT_EQ(0, network_init());
	command(P_RSSI_NETWORK, 40);
	command(P_TRACKING_ACTIVE, 0);
	command(P_NETWORK_RESET_PARAMS, 0);
	network_evaluate_contact(1, -DEFAULT_RSSI, false);
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_saved_parameters_are_loaded)
{
	uint8_t payload[3] = { 55, 1, 65 };

	put_param_record("dsa/network", payload, 3);
	MT_ASSERT_EQ(0, network_init());
	network_evaluate_contact(1, -56, false);
	network_evaluate_contact(1, -55, false);
	network_evaluate_contact(1, -66, true);
	network_evaluate_contact(1, -65, true);
	MT_ASSERT_EQ(2, contacts());
}

MT_TEST(c3_v2_record_is_migrated_with_default_location_threshold)
{
	uint8_t v2[2] = { 60, 1 };
	uint8_t raw[16];

	put_param_record("dsa/network", v2, 2);
	MT_ASSERT_EQ(0, network_init());
	network_evaluate_contact(1, -60, false);
	network_evaluate_contact(1, -DEFAULT_RSSI_LOC, true);
	MT_ASSERT_EQ(2, contacts());
	MT_ASSERT_EQ(15, fake_settings_get_raw("dsa/network", raw, sizeof(raw)));
	MT_ASSERT_EQ(DEFAULT_RSSI_LOC, raw[12 + 2]);
}

MT_TEST(c3_rssi_only_record_is_migrated_with_tracking_enabled)
{
	uint8_t v1[1] = { 45 };

	put_param_record("dsa/network", v1, 1);
	MT_ASSERT_EQ(0, network_init());
	network_evaluate_contact(1, -45, false);
	network_evaluate_contact(1, -46, false);
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_unversioned_legacy_byte_is_migrated)
{
	uint8_t legacy = 50;

	fake_settings_put_raw("dsa/network", &legacy, 1);
	MT_ASSERT_EQ(0, network_init());
	network_evaluate_contact(1, -50, false);
	network_evaluate_contact(1, -51, false);
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_corrupt_tracking_value_sets_param_fault)
{
	uint8_t payload[3] = { 55, 2, 65 };

	put_param_record("dsa/network", payload, 3);
	MT_ASSERT_EQ(0, network_init());
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_NETWORK_PARAMS);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_PARAM_ERROR);
	network_evaluate_contact(1, -DEFAULT_RSSI, false); /* defaults kept */
	MT_ASSERT_EQ(1, contacts());
}

MT_TEST(c3_ram_ring_overwrites_oldest_when_no_flush_ran)
{
	uint8_t buf[5];
	uint16_t written;

	MT_ASSERT_EQ(0, network_init());
	for (uint32_t i = 0; i < RING + 1; i++) {
		network_development_validation_append_contact((uint8_t)i, i, 50);
	}
	MT_ASSERT_EQ(RING, contacts());
	MT_ASSERT_EQ(0, network_contact_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(1, sys_get_be24(&buf[1]));
}

MT_TEST(c3_flush_moves_one_block_to_flash_at_threshold)
{
	uint32_t stored;

	MT_ASSERT_EQ(0, network_init());
	for (uint32_t i = 0; i < THRESHOLD; i++) {
		network_development_validation_append_contact((uint8_t)i, i, 50);
	}
	shim_run_ready_work();
	MT_ASSERT_EQ(0, network_storage_get_contact_count(&stored));
	MT_ASSERT_EQ(NETWORK_STORAGE_BLOCK_CONTACTS, stored);
	MT_ASSERT_EQ(THRESHOLD, contacts());
}

MT_TEST(c3_export_returns_every_contact_once_in_order)
{
	uint32_t first, last;
	bool ordered;
	uint32_t n;

	MT_ASSERT_EQ(0, network_init());
	fill(0, 30000);
	MT_ASSERT_EQ(30000, contacts());
	n = export_all(&first, &last, &ordered);
	MT_ASSERT_EQ(30000, n);
	MT_ASSERT_EQ(0, first);
	MT_ASSERT_EQ(29999, last);
	MT_ASSERT(ordered);
	MT_ASSERT_EQ(0, contacts());
}

MT_TEST(c3_export_abort_keeps_contacts)
{
	uint8_t buf[NUS_PAYLOAD];
	uint16_t written;

	MT_ASSERT_EQ(0, network_init());
	fill(0, 100);
	MT_ASSERT_EQ(0, network_contact_export_begin(buf, sizeof(buf), &written));
	network_contact_export_abort();
	MT_ASSERT_EQ(100, contacts());
	MT_ASSERT_EQ(-EINVAL, network_contact_export_commit());
}

MT_TEST(c3_second_export_begin_is_busy)
{
	uint8_t buf[NUS_PAYLOAD];
	uint16_t written;

	MT_ASSERT_EQ(0, network_init());
	fill(0, 10);
	MT_ASSERT_EQ(0, network_contact_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(-EBUSY, network_contact_export_begin(buf, sizeof(buf), &written));
}

MT_TEST(c3_contact_dropped_when_ram_full_during_export)
{
	uint8_t buf[NUS_PAYLOAD];
	uint16_t written;

	MT_ASSERT_EQ(0, network_init());
	for (uint32_t i = 0; i < RING; i++) {
		network_development_validation_append_contact((uint8_t)i, i, 50);
	}
	MT_ASSERT_EQ(0, network_contact_export_begin(buf, sizeof(buf), &written));
	network_evaluate_contact(1, -30, false);
	MT_ASSERT_EQ(RING, contacts());
	MT_ASSERT(shim_log_contains("dropping newest contact"));
}

MT_TEST(c3_data_level_thresholds_up_to_level_4)
{
	MT_ASSERT_EQ(0, network_init());
	MT_ASSERT_EQ(0, data_level());
	fill(0, 1);
	MT_ASSERT_EQ(1, data_level());
	fill(1, 1);
	MT_ASSERT_EQ(2, data_level());
	fill(2, DATA_LEVEL_3 - 2);
	MT_ASSERT_EQ(2, data_level());
	fill(DATA_LEVEL_3, 1);
	MT_ASSERT_EQ(3, data_level());
	fill(DATA_LEVEL_3 + 1, DATA_LEVEL_4 - DATA_LEVEL_3);
	MT_ASSERT_EQ(4, data_level());
}

MT_TEST(c3_data_level_5_reported_above_100000_contacts)
{
	MT_ASSERT_EQ(0, network_init());
	fill(0, DATA_LEVEL_5 + 1);
	MT_ASSERT_EQ(DATA_LEVEL_5 + 1, contacts());
	MT_ASSERT_MSG(data_level() == 5,
		      "data level is %u for %u contacts: the count is clamped to "
		      "UINT16_MAX (65535) before contact_status_from_count(), so "
		      "levels 5-7 (100000/150000/210000) can never be advertised",
		      device_get_network_status() & DATA_LEVEL_MASK,
		      (unsigned int)contacts());
}

MT_TEST(c3_capacity_allows_data_level_7)
{
	uint32_t capacity = (uint32_t)NETWORK_STORAGE_BLOCK_COUNT *
				    NETWORK_STORAGE_BLOCK_CONTACTS + RING;

	MT_ASSERT_MSG(capacity > DATA_LEVEL_7,
		      "flash + RAM capacity %u is not above DATA_LEVEL_7 (%u)",
		      (unsigned int)capacity, (unsigned int)DATA_LEVEL_7);
}

MT_TEST(c3_export_drain_lowers_data_level)
{
	uint32_t first, last;
	bool ordered;

	MT_ASSERT_EQ(0, network_init());
	fill(0, 6000);
	MT_ASSERT_EQ(3, data_level());
	export_all(&first, &last, &ordered);
	MT_ASSERT_EQ(0, data_level());
}

MT_TEST(c3_storage_full_bit_set_when_contacts_are_discarded)
{
	uint32_t capacity = (uint32_t)NETWORK_STORAGE_BLOCK_COUNT *
				    NETWORK_STORAGE_BLOCK_CONTACTS + RING;

	MT_ASSERT_EQ(0, network_init());
	fill(0, capacity + 2 * NETWORK_STORAGE_BLOCK_CONTACTS);
	MT_ASSERT_MSG(contacts() < capacity + 2 * NETWORK_STORAGE_BLOCK_CONTACTS,
		      "expected oldest contacts to be discarded");
	MT_ASSERT_MSG(device_get_radio_status() & STORAGE_STATUS_STORAGE_FULL,
		      "oldest contacts were discarded but the advertised "
		      "storage-full bit is not set (flash_ring_store discards "
		      "silently and never returns -ENOSPC)");
}

MT_TEST(c3_contacts_in_flash_survive_boot)
{
	plant_flash_contacts(50);
	(void)network_init();
	MT_ASSERT_MSG(contacts() == 50,
		      "50 contacts in flash before boot, %u after network_init()",
		      (unsigned int)contacts());
}

MT_TEST(c3_saved_parameters_loaded_when_contacts_pending_at_boot)
{
	uint8_t payload[3] = { 55, 1, 65 };
	int err;

	put_param_record("dsa/network", payload, 3);
	plant_flash_contacts(50);
	err = network_init();
	MT_ASSERT_MSG(err == 0,
		      "network_init() returned %d with contacts pending in flash; "
		      "it returns before network_params_load(), so the saved "
		      "RSSI threshold 55 is ignored for this boot", err);
	network_evaluate_contact(1, -60, false); /* above saved threshold 55 */
	MT_ASSERT_EQ(50, contacts());
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c3_init_with_empty_storage_uses_defaults),
	MT_CASE(c3_location_tag_uses_location_threshold),
	MT_CASE(c3_tracking_off_records_nothing),
	MT_CASE(c3_contact_record_encodes_id_uptime_and_rssi_magnitude),
	MT_CASE(c3_contact_uptime_is_low_24_bits),
	MT_CASE(c3_rssi_command_validation),
	MT_CASE(c3_tracking_command_rejects_values_above_1),
	MT_CASE(c3_commit_saves_only_when_parameters_change),
	MT_CASE(c3_reset_command_restores_defaults),
	MT_CASE(c3_saved_parameters_are_loaded),
	MT_CASE(c3_v2_record_is_migrated_with_default_location_threshold),
	MT_CASE(c3_rssi_only_record_is_migrated_with_tracking_enabled),
	MT_CASE(c3_unversioned_legacy_byte_is_migrated),
	MT_CASE(c3_corrupt_tracking_value_sets_param_fault),
	MT_CASE(c3_ram_ring_overwrites_oldest_when_no_flush_ran),
	MT_CASE(c3_flush_moves_one_block_to_flash_at_threshold),
	MT_CASE(c3_export_returns_every_contact_once_in_order),
	MT_CASE(c3_export_abort_keeps_contacts),
	MT_CASE(c3_second_export_begin_is_busy),
	MT_CASE(c3_contact_dropped_when_ram_full_during_export),
	MT_CASE(c3_data_level_thresholds_up_to_level_4),
	MT_CASE(c3_data_level_5_reported_above_100000_contacts),
	MT_CASE(c3_capacity_allows_data_level_7),
	MT_CASE(c3_export_drain_lowers_data_level),
	MT_CASE(c3_storage_full_bit_set_when_contacts_are_discarded),
	MT_CASE(c3_contacts_in_flash_survive_boot),
	MT_CASE(c3_saved_parameters_loaded_when_contacts_pending_at_boot))
