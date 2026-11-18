/*
 * C6 - device.c: identity lookup against the real radio_ids.c table and the
 * advertised status-byte helpers (storage fault / storage full / network
 * status bits). See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>

#include "common_include.h"
#include "device.h"
#include "fake_peers.h"
#include "minitest.h"
#include "radio_ids.h"

static void setup(void)
{
	shim_bt_reset();
	fake_peers_reset();
	device_set_radio_status(0);
	device_set_network_status(0);
	device_set_storage_fault(0xFFFFFFFFU, false);
	device_set_storage_full(0xFFFFFFFFU, false);
	fake_peers_reset();
}

static bt_addr_le_t addr(uint8_t type, uint8_t b0, uint8_t b1, uint8_t b2,
			 uint8_t b3, uint8_t b4, uint8_t b5)
{
	bt_addr_le_t a = { .type = type, .a = { .val = { b0, b1, b2, b3, b4, b5 } } };

	return a;
}

MT_TEST(c6_lookup_known_development_kit_id)
{
	/* First table entry: development kit with id 254. */
	bt_addr_le_t a = addr(BT_ADDR_LE_RANDOM, 0xB1, 0x7D, 0x76, 0x1a, 0x92, 0xd1);

	MT_ASSERT_EQ(254, lookup_device_id(&a));
}

MT_TEST(c6_lookup_known_tag_id)
{
	bt_addr_le_t a = addr(BT_ADDR_LE_RANDOM, 0x97, 0x57, 0xAF, 0xEF, 0xE9, 0xF7);

	MT_ASSERT_EQ(1, lookup_device_id(&a));
}

MT_TEST(c6_lookup_every_table_entry_roundtrips)
{
	for (size_t i = 0; i < known_device_table_len; i++) {
		MT_ASSERT_EQ(known_device_table[i].id,
			     lookup_device_id(&known_device_table[i].addr));
	}
}

MT_TEST(c6_lookup_unknown_address_returns_0xff)
{
	bt_addr_le_t a = addr(BT_ADDR_LE_RANDOM, 1, 2, 3, 4, 5, 6);

	MT_ASSERT_EQ(0xff, lookup_device_id(&a));
}

MT_TEST(c6_lookup_requires_matching_address_type)
{
	bt_addr_le_t a = addr(BT_ADDR_LE_PUBLIC, 0xB1, 0x7D, 0x76, 0x1a, 0x92, 0xd1);

	MT_ASSERT_EQ(0xff, lookup_device_id(&a));
}

MT_TEST(c6_get_device_id_uses_bluetooth_identity)
{
	shim_bt.identity = addr(BT_ADDR_LE_RANDOM, 0x97, 0x57, 0xAF, 0xEF, 0xE9, 0xF7);
	shim_bt.identity_count = 1;
	MT_ASSERT_EQ(1, get_device_id());
}

MT_TEST(c6_storage_fault_param_sets_param_error_bit)
{
	device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, true);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_PARAM_ERROR);
	MT_ASSERT(!(device_get_radio_status() & STORAGE_STATUS_STORAGE_ERROR));
	device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, false);
	MT_ASSERT(!(device_get_radio_status() & STORAGE_STATUS_PARAM_ERROR));
}

MT_TEST(c6_every_storage_fault_sets_storage_error_bit)
{
	static const uint32_t faults[] = {
		STORAGE_FAULT_CONTACT_INIT, STORAGE_FAULT_CONTACT_READ,
		STORAGE_FAULT_CONTACT_WRITE, STORAGE_FAULT_CONTACT_DELETE,
		STORAGE_FAULT_CONTACT_META, STORAGE_FAULT_SELF_REPORT_INIT,
		STORAGE_FAULT_SELF_REPORT_READ, STORAGE_FAULT_SELF_REPORT_WRITE,
		STORAGE_FAULT_SELF_REPORT_DELETE, STORAGE_FAULT_SELF_REPORT_META,
		STORAGE_FAULT_ECO_LOG_INIT, STORAGE_FAULT_ECO_LOG_READ,
		STORAGE_FAULT_ECO_LOG_WRITE, STORAGE_FAULT_ECO_LOG_DELETE,
		STORAGE_FAULT_ECO_LOG_META,
	};

	for (size_t i = 0; i < ARRAY_SIZE(faults); i++) {
		device_set_storage_fault(faults[i], true);
		MT_ASSERT_MSG(device_get_radio_status() & STORAGE_STATUS_STORAGE_ERROR,
			      "fault bit 0x%x did not set the storage error bit",
			      (unsigned int)faults[i]);
		device_set_storage_fault(faults[i], false);
		MT_ASSERT(!(device_get_radio_status() & STORAGE_STATUS_STORAGE_ERROR));
	}
}

MT_TEST(c6_storage_error_bit_stays_while_any_fault_remains)
{
	device_set_storage_fault(STORAGE_FAULT_CONTACT_READ, true);
	device_set_storage_fault(STORAGE_FAULT_ECO_LOG_WRITE, true);
	device_set_storage_fault(STORAGE_FAULT_CONTACT_READ, false);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_STORAGE_ERROR);
	device_set_storage_fault(STORAGE_FAULT_ECO_LOG_WRITE, false);
	MT_ASSERT(!(device_get_radio_status() & STORAGE_STATUS_STORAGE_ERROR));
}

MT_TEST(c6_storage_full_bit_tracks_any_full_domain)
{
	device_set_storage_full(STORAGE_FULL_CONTACT, true);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_STORAGE_FULL);
	device_set_storage_full(STORAGE_FULL_ECO_LOG, true);
	device_set_storage_full(STORAGE_FULL_CONTACT, false);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_STORAGE_FULL);
	device_set_storage_full(STORAGE_FULL_ECO_LOG, false);
	MT_ASSERT(!(device_get_radio_status() & STORAGE_STATUS_STORAGE_FULL));
}

MT_TEST(c6_storage_bits_do_not_touch_radio_bits)
{
	device_set_radio_status_bit(RADIO_STATUS_SCAN_ERROR, true);
	device_set_radio_status_bit(RADIO_STATUS_MOTION_UNAVAILABLE, true);
	device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, true);
	device_set_storage_full(STORAGE_FULL_CONTACT, true);
	device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, false);
	device_set_storage_full(STORAGE_FULL_CONTACT, false);
	MT_ASSERT_EQ(RADIO_STATUS_SCAN_ERROR | RADIO_STATUS_MOTION_UNAVAILABLE,
		     device_get_radio_status());
}

MT_TEST(c6_status_update_scheduled_only_on_change)
{
	device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, true);
	MT_ASSERT_EQ(1, fake_peers.radio_status_updates);
	device_set_storage_fault(STORAGE_FAULT_CONTACT_READ, true); /* bit already set */
	MT_ASSERT_EQ(1, fake_peers.radio_status_updates);
	device_set_storage_fault(STORAGE_FAULT_CONTACT_INIT, false);
	device_set_storage_fault(STORAGE_FAULT_CONTACT_READ, false);
	MT_ASSERT_EQ(2, fake_peers.radio_status_updates);
}

MT_TEST(c6_network_status_bits_respect_mask)
{
	device_set_network_status(0xFF);
	fake_peers_reset();
	device_set_network_status_bits(DATA_LEVEL_MASK, 0x03);
	MT_ASSERT_EQ(0xF3, device_get_network_status());
	device_set_network_status_bits(BATTERY_LEVEL_MASK, 0x00);
	MT_ASSERT_EQ(0x83, device_get_network_status());
	device_set_network_status_bits(ECO_MODE_MASK, 0x00);
	MT_ASSERT_EQ(0x03, device_get_network_status());
	MT_ASSERT_EQ(3, fake_peers.radio_status_updates);
	device_set_network_status_bits(DATA_LEVEL_MASK, 0x13); /* bit 4 outside mask */
	MT_ASSERT_EQ(0x03, device_get_network_status());
	MT_ASSERT_EQ(3, fake_peers.radio_status_updates);
}

MT_TEST(c6_network_status_field_layout_matches_decision_2026_08_05)
{
	/* Data level bits 3..0, battery bits 6..4, eco bit 7 - disjoint. */
	MT_ASSERT_EQ(0x0F, DATA_LEVEL_MASK);
	MT_ASSERT_EQ(0x70, BATTERY_LEVEL_MASK);
	MT_ASSERT_EQ(0x80, ECO_MODE_MASK);
	MT_ASSERT_EQ(0, DATA_LEVEL_MASK & BATTERY_LEVEL_MASK);
	MT_ASSERT_EQ(0, BATTERY_LEVEL_MASK & ECO_MODE_MASK);
	MT_ASSERT_EQ(4, P_SHIFT_STATUS_BATTERY);
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c6_lookup_known_development_kit_id),
	MT_CASE(c6_lookup_known_tag_id),
	MT_CASE(c6_lookup_every_table_entry_roundtrips),
	MT_CASE(c6_lookup_unknown_address_returns_0xff),
	MT_CASE(c6_lookup_requires_matching_address_type),
	MT_CASE(c6_get_device_id_uses_bluetooth_identity),
	MT_CASE(c6_storage_fault_param_sets_param_error_bit),
	MT_CASE(c6_every_storage_fault_sets_storage_error_bit),
	MT_CASE(c6_storage_error_bit_stays_while_any_fault_remains),
	MT_CASE(c6_storage_full_bit_tracks_any_full_domain),
	MT_CASE(c6_storage_bits_do_not_touch_radio_bits),
	MT_CASE(c6_status_update_scheduled_only_on_change),
	MT_CASE(c6_network_status_bits_respect_mask),
	MT_CASE(c6_network_status_field_layout_matches_decision_2026_08_05))
