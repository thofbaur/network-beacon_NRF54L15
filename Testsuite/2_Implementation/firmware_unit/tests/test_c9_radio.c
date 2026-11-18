/*
 * C9 - radio.c: advertisement content, accept list, the command path from
 * a raw advertising report through parse_advertisement_cb ->
 * evaluate_command_data -> domain routing, radio parameter validation and
 * persistence, and eco-mode scanning. Advertising reports are injected via
 * the scan callback captured by the Bluetooth shim; led/network/motion/eco
 * log/nus are recording fakes. See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "common_include.h"
#include "device.h"
#include "fake_peers.h"
#include "fake_settings.h"
#include "minitest.h"
#include "radio.h"
#include "radio_ids.h"

#define OWN_ID 254
#define ADV_MIN_UNITS 144	/* 90 ms */
#define ADV_MAX_UNITS 192	/* 120 ms */
#define SCAN_INTERVAL_UNITS 11200 /* 7000 ms */
#define SCAN_WINDOW_UNITS 192	/* 120 ms */
#define ECO_WINDOW_UNITS 160	/* 100 ms */
#define ECO_PERIOD_S (CONFIG_DSA_ECO_SCAN_INTERVAL_MS / 1000)

static const bt_addr_le_t own_addr = {
	.type = BT_ADDR_LE_RANDOM,
	.a = { .val = { 0xB1, 0x7D, 0x76, 0x1a, 0x92, 0xd1 } }, /* id 254 */
};
static const bt_addr_le_t peer_addr = {
	.type = BT_ADDR_LE_RANDOM,
	.a = { .val = { 0x97, 0x57, 0xAF, 0xEF, 0xE9, 0xF7 } },
};

static void reset_all(void)
{
	shim_bt_reset();
	shim_bt.identity = own_addr;
	fake_settings_reset();
	fake_peers_reset();
	shim_work_reset();
	shim_set_uptime_ms(0);
	shim_log_clear();
	device_set_radio_status(0);
	device_set_network_status(0);
	device_set_storage_fault(0xFFFFFFFFU, false);
}

static void boot(void)
{
	MT_ASSERT_EQ(0, radio_init());
	MT_ASSERT_EQ(0, radio_start());
	shim_run_ready_work();
}

static void setup(void)
{
	reset_all();
}

static void deliver(const char *name, const uint8_t *mfg, uint8_t mfg_len, int8_t rssi)
{
	uint8_t ad[64];
	uint16_t len = shim_bt_build_adv(ad, sizeof(ad), name, mfg, mfg_len);

	MT_ASSERT(len > 0);
	MT_ASSERT(shim_bt_deliver_adv(&peer_addr, rssi, ad, len));
}

/* Command advertisement as the Control tool builds it:
 * target, then (parameter, HI(value), LO(value)) triples.
 */
static void send_command(uint8_t target, uint8_t parameter, uint16_t value)
{
	uint8_t mfg[4] = { target, parameter, (uint8_t)(value >> 8), (uint8_t)value };

	deliver("DSZ", mfg, sizeof(mfg), -40);
	shim_run_ready_work();
}

static bool radio_saved(void)
{
	return fake_settings_has("dsa/radio2");
}

static void put_radio_record(const uint8_t stored[17])
{
	uint8_t rec[12 + 17];

	sys_put_be32(0x44534150U, &rec[0]);
	sys_put_be16(1U, &rec[4]);
	sys_put_be16(17U, &rec[6]);
	sys_put_be32(crc32_ieee(stored, 17), &rec[8]);
	memcpy(&rec[12], stored, 17);
	fake_settings_put_raw("dsa/radio2", rec, sizeof(rec));
}

/* ---- startup / advertising --------------------------------------------- */

MT_TEST(c9_init_adds_every_known_device_to_accept_list)
{
	boot();
	MT_ASSERT_EQ(known_device_table_len, shim_bt.accept_list_adds);
	MT_ASSERT(!(device_get_radio_status() & RADIO_STATUS_SCAN_ERROR));
}

MT_TEST(c9_accept_list_failure_sets_scan_error_but_keeps_advertising)
{
	shim_bt.accept_list_err = -ENOMEM;
	boot();
	MT_ASSERT(device_get_radio_status() & RADIO_STATUS_SCAN_ERROR);
	MT_ASSERT(shim_bt.advertising);
}

MT_TEST(c9_advertisement_carries_name_id_and_status_bytes)
{
	device_set_network_status(0x35);
	boot();
	MT_ASSERT_EQ(3, shim_bt.adv_name_len);
	MT_ASSERT_MEM_EQ("DSA", shim_bt.adv_name, 3);
	MT_ASSERT_EQ(3, shim_bt.adv_mfg_len);
	MT_ASSERT_EQ(OWN_ID, shim_bt.adv_mfg[ADV_POS_ID]);
	MT_ASSERT_EQ(device_get_radio_status(), shim_bt.adv_mfg[ADV_POS_RADIO_STATUS]);
	MT_ASSERT_EQ(0x35, shim_bt.adv_mfg[ADV_POS_NETWORK_STATUS]);
}

MT_TEST(c9_default_radio_timing_is_applied)
{
	boot();
	MT_ASSERT_EQ(ADV_MIN_UNITS, shim_bt.adv_param.interval_min);
	MT_ASSERT_EQ(ADV_MAX_UNITS, shim_bt.adv_param.interval_max);
	MT_ASSERT(shim_bt.adv_param.options & BT_LE_ADV_OPT_CONN);
	MT_ASSERT(shim_bt.scanning);
	MT_ASSERT_EQ(SCAN_INTERVAL_UNITS, shim_bt.scan_param.interval);
	MT_ASSERT_EQ(SCAN_WINDOW_UNITS, shim_bt.scan_param.window);
	MT_ASSERT_EQ(BT_LE_SCAN_TYPE_PASSIVE, shim_bt.scan_param.type);
	MT_ASSERT(shim_bt.scan_param.options & BT_LE_SCAN_OPT_FILTER_ACCEPT_LIST);
}

MT_TEST(c9_start_fails_when_advertising_fails)
{
	MT_ASSERT_EQ(0, radio_init());
	shim_bt.adv_start_err = -EIO;
	MT_ASSERT_EQ(-EIO, radio_start());
}

MT_TEST(c9_scan_failure_is_degraded_mode)
{
	MT_ASSERT_EQ(0, radio_init());
	shim_bt.scan_start_err = -EIO;
	MT_ASSERT_EQ(0, radio_start());
	MT_ASSERT(shim_bt.advertising);
	MT_ASSERT(device_get_radio_status() & RADIO_STATUS_SCAN_ERROR);
}

MT_TEST(c9_nus_init_failure_sets_nus_error_bit)
{
	fake_peers.nus_init_err = -EIO;
	boot();
	MT_ASSERT(device_get_radio_status() & RADIO_STATUS_NUS_ERROR);
}

MT_TEST(c9_status_change_updates_advertised_bytes)
{
	boot();
	device_set_storage_fault(STORAGE_FAULT_CONTACT_WRITE, true);
	shim_run_ready_work();
	MT_ASSERT(shim_bt.adv_mfg[ADV_POS_RADIO_STATUS] & STORAGE_STATUS_STORAGE_ERROR);
}

MT_TEST(c9_status_update_skipped_while_nus_connected)
{
	int before;

	boot();
	before = shim_bt.adv_update_calls;
	fake_peers.nus_connected = true;
	MT_ASSERT_EQ(0, adv_update());
	MT_ASSERT_EQ(before, shim_bt.adv_update_calls);
}

/* ---- contacts ---------------------------------------------------------- */

MT_TEST(c9_dsa_dst_dsl_advertisements_are_contacts)
{
	uint8_t mfg[3] = { 7, 0, 0 };

	boot();
	deliver("DSA", mfg, 3, -61);
	deliver("DST", mfg, 3, -62);
	deliver("DSL", mfg, 3, -63);
	MT_ASSERT_EQ(3, fake_peers.contact_count);
	MT_ASSERT_EQ(7, fake_peers.contacts[0].id);
	MT_ASSERT_EQ(-61, fake_peers.contacts[0].rssi);
	MT_ASSERT(!fake_peers.contacts[0].is_location_tag);
	MT_ASSERT(!fake_peers.contacts[1].is_location_tag);
	MT_ASSERT(fake_peers.contacts[2].is_location_tag);
}

MT_TEST(c9_other_names_and_missing_id_are_ignored)
{
	uint8_t mfg[3] = { 7, 0, 0 };

	boot();
	deliver("DSB", mfg, 3, -50);
	deliver("DSAX", mfg, 3, -50);
	deliver("DS", mfg, 3, -50);
	deliver("DSA", NULL, 0, -50);
	MT_ASSERT_EQ(0, fake_peers.contact_count);
}

MT_TEST(c9_malformed_ad_structures_do_not_crash)
{
	const uint8_t overlong[] = { 0x10, BT_DATA_NAME_COMPLETE, 'D', 'S', 'A' };
	const uint8_t zero_len[] = { 0x00, 0xff, 1, 2 };

	boot();
	MT_ASSERT(shim_bt_deliver_adv(&peer_addr, -50, overlong, sizeof(overlong)));
	MT_ASSERT(shim_bt_deliver_adv(&peer_addr, -50, zero_len, sizeof(zero_len)));
	MT_ASSERT_EQ(0, fake_peers.contact_count);
}

/* ---- command path ------------------------------------------------------ */

MT_TEST(c9_command_for_own_id_is_routed_by_parameter_base)
{
	uint8_t mfg[] = { OWN_ID,
			  P_MAIN_LED_ACTIVE, 0, 1,
			  P_RSSI_NETWORK, 0, 70,
			  P_MOTION_INACTIVITY_TIMEOUT_S, 0x02, 0x58 };

	boot();
	deliver("DSZ", mfg, sizeof(mfg), -40);
	shim_run_ready_work();
	MT_ASSERT_EQ(1, fake_peers.led_cmd_count);
	MT_ASSERT_EQ(P_MAIN_LED_ACTIVE, fake_peers.led_cmds[0].parameter);
	MT_ASSERT_EQ(1, fake_peers.led_cmds[0].value);
	MT_ASSERT_EQ(1, fake_peers.network_cmd_count);
	MT_ASSERT_EQ(70, fake_peers.network_cmds[0].value);
	MT_ASSERT_EQ(1, fake_peers.motion_cmd_count);
	MT_ASSERT_EQ(600, fake_peers.motion_cmds[0].value);
	MT_ASSERT_EQ(1, fake_peers.led_begin);
	MT_ASSERT_EQ(1, fake_peers.led_commit);
	MT_ASSERT_EQ(1, fake_peers.network_commit);
	MT_ASSERT_EQ(1, fake_peers.motion_commit);
}

MT_TEST(c9_broadcast_command_is_accepted)
{
	boot();
	send_command(0xFF, P_RSSI_LOCATION, 90);
	MT_ASSERT_EQ(1, fake_peers.network_cmd_count);
}

MT_TEST(c9_command_for_other_id_is_ignored)
{
	boot();
	send_command(1, P_RSSI_NETWORK, 90);
	MT_ASSERT_EQ(0, fake_peers.network_cmd_count);
	MT_ASSERT_EQ(0, fake_peers.network_begin);
}

MT_TEST(c9_command_with_partial_triple_is_rejected)
{
	uint8_t mfg[] = { OWN_ID, P_RSSI_NETWORK, 0, 70, P_RSSI_LOCATION, 0 };

	boot();
	deliver("DSZ", mfg, sizeof(mfg), -40);
	shim_run_ready_work();
	MT_ASSERT_EQ(0, fake_peers.network_cmd_count);
	MT_ASSERT_EQ(0, fake_peers.network_begin);
}

MT_TEST(c9_unknown_parameter_base_is_skipped)
{
	uint8_t mfg[] = { OWN_ID, 0x40, 0, 1, P_RSSI_NETWORK, 0, 70 };

	boot();
	deliver("DSZ", mfg, sizeof(mfg), -40);
	shim_run_ready_work();
	MT_ASSERT_EQ(0, fake_peers.led_cmd_count);
	MT_ASSERT_EQ(0, fake_peers.motion_cmd_count);
	MT_ASSERT_EQ(1, fake_peers.network_cmd_count);
}

MT_TEST(c9_command_without_manufacturer_data_is_ignored)
{
	boot();
	deliver("DSZ", NULL, 0, -40);
	shim_run_ready_work();
	MT_ASSERT_EQ(0, fake_peers.network_begin);
}

MT_TEST(c9_command_queue_holds_four_commands)
{
	boot();
	for (int i = 0; i < 5; i++) {
		uint8_t mfg[] = { OWN_ID, P_RSSI_NETWORK, 0, (uint8_t)(60 + i) };

		deliver("DSZ", mfg, sizeof(mfg), -40);
	}
	shim_run_ready_work();
	MT_ASSERT_EQ(4, fake_peers.network_cmd_count);
	MT_ASSERT(shim_log_contains("Command queue full"));
}

/* ---- radio parameters -------------------------------------------------- */

MT_TEST(c9_adv_interval_command_applies_and_persists)
{
	boot();
	send_command(OWN_ID, P_ADV_INTERVAL_MS, 1000);
	MT_ASSERT_EQ(1600, shim_bt.adv_param.interval_min);
	MT_ASSERT_EQ(1600, shim_bt.adv_param.interval_max);
	MT_ASSERT(radio_saved());
}

MT_TEST(c9_adv_interval_bounds)
{
	boot();
	send_command(OWN_ID, P_ADV_INTERVAL_MS, 19);	/* 30 units < 0x20 */
	MT_ASSERT_EQ(ADV_MIN_UNITS, shim_bt.adv_param.interval_min);
	send_command(OWN_ID, P_ADV_INTERVAL_MS, 20);	/* 32 units */
	MT_ASSERT_EQ(32, shim_bt.adv_param.interval_min);
	send_command(OWN_ID, P_ADV_INTERVAL_MS, 10240); /* 0x4000 */
	MT_ASSERT_EQ(0x4000, shim_bt.adv_param.interval_min);
	send_command(OWN_ID, P_ADV_INTERVAL_MS, 10241); /* > 0x4000 */
	MT_ASSERT_EQ(0x4000, shim_bt.adv_param.interval_min);
}

MT_TEST(c9_scan_window_above_interval_is_rejected)
{
	boot();
	send_command(OWN_ID, P_SCAN_WINDOW_MS, 8000);
	MT_ASSERT_EQ(SCAN_WINDOW_UNITS, shim_bt.scan_param.window);
	MT_ASSERT(!radio_saved());
	MT_ASSERT(shim_log_contains("Rejecting invalid radio parameters"));
}

MT_TEST(c9_scan_interval_bounds)
{
	boot();
	send_command(OWN_ID, P_SCAN_INTERVAL_MS, 10241); /* > 0x4000 units */
	MT_ASSERT_EQ(SCAN_INTERVAL_UNITS, shim_bt.scan_param.interval);
	send_command(OWN_ID, P_SCAN_INTERVAL_MS, 10240);
	MT_ASSERT_EQ(0x4000, shim_bt.scan_param.interval);
	send_command(OWN_ID, P_SCAN_INTERVAL_MS, 100);	/* below window 120 ms */
	MT_ASSERT_EQ(0x4000, shim_bt.scan_param.interval);
}

MT_TEST(c9_zero_eco_scan_period_is_rejected)
{
	boot();
	send_command(OWN_ID, P_ECO_SCAN_PERIOD_S, 0);
	MT_ASSERT(!radio_saved());
}

MT_TEST(c9_reset_command_restores_default_timing)
{
	boot();
	send_command(OWN_ID, P_ADV_INTERVAL_MS, 1000);
	send_command(OWN_ID, P_RADIO_RESET_PARAMS, 0);
	MT_ASSERT_EQ(ADV_MIN_UNITS, shim_bt.adv_param.interval_min);
}

MT_TEST(c9_saved_radio_parameters_are_applied_at_boot)
{
	uint8_t stored[17];

	sys_put_be16(1600, &stored[0]);
	sys_put_be16(1600, &stored[2]);
	sys_put_be16(ADV_MIN_UNITS, &stored[4]);
	sys_put_be16(ADV_MAX_UNITS, &stored[6]);
	sys_put_be16(SCAN_INTERVAL_UNITS, &stored[8]);
	sys_put_be16(SCAN_WINDOW_UNITS, &stored[10]);
	sys_put_be16(ECO_WINDOW_UNITS, &stored[12]);
	sys_put_be16(ECO_PERIOD_S, &stored[14]);
	stored[16] = 1;
	put_radio_record(stored);
	boot();
	MT_ASSERT_EQ(1600, shim_bt.adv_param.interval_min);
	MT_ASSERT(!(device_get_storage_faults() & STORAGE_FAULT_RADIO_PARAMS));
}

MT_TEST(c9_invalid_saved_radio_parameters_fall_back_to_defaults)
{
	uint8_t stored[17] = { 0 }; /* all-zero intervals are invalid */

	stored[16] = 1;
	put_radio_record(stored);
	boot();
	MT_ASSERT_EQ(ADV_MIN_UNITS, shim_bt.adv_param.interval_min);
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_RADIO_PARAMS);
}

/* ---- eco mode ---------------------------------------------------------- */

MT_TEST(c9_motion_eco_override_scans_in_bursts)
{
	int starts;

	boot();
	radio_set_eco_override(true);
	shim_run_ready_work();
	MT_ASSERT_EQ(1, fake_peers.eco_enter_calls);
	MT_ASSERT(shim_bt.scanning);
	MT_ASSERT_EQ(ECO_WINDOW_UNITS, shim_bt.scan_param.window);
	MT_ASSERT_EQ(ECO_WINDOW_UNITS, shim_bt.scan_param.interval);
	shim_advance_ms(100);
	MT_ASSERT(!shim_bt.scanning);
	starts = shim_bt.scan_start_calls;
	shim_advance_ms((int64_t)ECO_PERIOD_S * 1000 - 1);
	MT_ASSERT_EQ(starts, shim_bt.scan_start_calls);
	shim_advance_ms(1);
	MT_ASSERT_EQ(starts + 1, shim_bt.scan_start_calls);
	MT_ASSERT(shim_bt.scanning);
}

MT_TEST(c9_eco_session_logged_only_on_real_transitions)
{
	boot();
	radio_set_eco_override(true);
	radio_set_eco_override(true);
	radio_set_eco_override(false);
	radio_set_eco_override(false);
	MT_ASSERT_EQ(1, fake_peers.eco_enter_calls);
	MT_ASSERT_EQ(1, fake_peers.eco_leave_calls);
	MT_ASSERT_EQ(SCAN_WINDOW_UNITS, shim_bt.scan_param.window);
	MT_ASSERT(shim_bt.scanning);
}

MT_TEST(c9_eco_override_keeps_normal_advertising_interval)
{
	boot();
	radio_set_eco_override(true);
	MT_ASSERT_EQ(CONFIG_DSA_ECO_ADV_INTERVAL_MIN_MS * 8 / 5, shim_bt.adv_param.interval_min);
}

MT_TEST(c9_manual_eco_mode_keeps_periodic_scan_bursts)
{
	int starts;

	boot();
	send_command(OWN_ID, P_SET_RAD_ACTIVE, 0);
	MT_ASSERT_EQ(1, fake_peers.eco_enter_calls);
	MT_ASSERT(radio_saved());
	starts = shim_bt.scan_start_calls;
	shim_advance_ms((int64_t)(ECO_PERIOD_S + 5) * 1000);
	MT_ASSERT_MSG(shim_bt.scan_start_calls > starts,
		      "manual eco mode (P_SET_RAD_ACTIVE=0) never starts a scan burst: "
		      "eco_scan_handler() returns unless the motion override is "
		      "active, so the beacon stops scanning and can no longer "
		      "receive commands (the mode is persisted across reboot)");
}

MT_TEST(c9_manual_eco_mode_can_be_left_by_command)
{
	boot();
	send_command(OWN_ID, P_SET_RAD_ACTIVE, 0);
	shim_advance_ms((int64_t)(ECO_PERIOD_S + 5) * 1000);
	if (!shim_bt.scanning) {
		/* Give it the chance of one burst window; a command can only be
		 * received while scanning.
		 */
		MT_ASSERT_MSG(false, "no scan active in manual eco mode - a "
				     "P_SET_RAD_ACTIVE=1 command cannot be received");
	}
	send_command(OWN_ID, P_SET_RAD_ACTIVE, 1);
	MT_ASSERT_EQ(1, fake_peers.eco_leave_calls);
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c9_init_adds_every_known_device_to_accept_list),
	MT_CASE(c9_accept_list_failure_sets_scan_error_but_keeps_advertising),
	MT_CASE(c9_advertisement_carries_name_id_and_status_bytes),
	MT_CASE(c9_default_radio_timing_is_applied),
	MT_CASE(c9_start_fails_when_advertising_fails),
	MT_CASE(c9_scan_failure_is_degraded_mode),
	MT_CASE(c9_nus_init_failure_sets_nus_error_bit),
	MT_CASE(c9_status_change_updates_advertised_bytes),
	MT_CASE(c9_status_update_skipped_while_nus_connected),
	MT_CASE(c9_dsa_dst_dsl_advertisements_are_contacts),
	MT_CASE(c9_other_names_and_missing_id_are_ignored),
	MT_CASE(c9_malformed_ad_structures_do_not_crash),
	MT_CASE(c9_command_for_own_id_is_routed_by_parameter_base),
	MT_CASE(c9_broadcast_command_is_accepted),
	MT_CASE(c9_command_for_other_id_is_ignored),
	MT_CASE(c9_command_with_partial_triple_is_rejected),
	MT_CASE(c9_unknown_parameter_base_is_skipped),
	MT_CASE(c9_command_without_manufacturer_data_is_ignored),
	MT_CASE(c9_command_queue_holds_four_commands),
	MT_CASE(c9_adv_interval_command_applies_and_persists),
	MT_CASE(c9_adv_interval_bounds),
	MT_CASE(c9_scan_window_above_interval_is_rejected),
	MT_CASE(c9_scan_interval_bounds),
	MT_CASE(c9_zero_eco_scan_period_is_rejected),
	MT_CASE(c9_reset_command_restores_default_timing),
	MT_CASE(c9_saved_radio_parameters_are_applied_at_boot),
	MT_CASE(c9_invalid_saved_radio_parameters_fall_back_to_defaults),
	MT_CASE(c9_motion_eco_override_scans_in_bursts),
	MT_CASE(c9_eco_session_logged_only_on_real_transitions),
	MT_CASE(c9_eco_override_keeps_normal_advertising_interval),
	MT_CASE(c9_manual_eco_mode_keeps_periodic_scan_bursts),
	MT_CASE(c9_manual_eco_mode_can_be_left_by_command))
