/*
 * C8 - motion.c (host build without accelerometer, like the DK): command
 * validation, persistence and the "motion unavailable" status. Eco
 * transitions driven by the ADXL367 need the sensor and are covered by
 * H2 (manual eco) and I4 (tag). See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "common_include.h"
#include "device.h"
#include "fake_peers.h"
#include "fake_settings.h"
#include "minitest.h"
#include "motion.h"

static void setup(void)
{
	fake_settings_reset();
	fake_peers_reset();
	shim_work_reset();
	shim_set_uptime_ms(0);
	shim_log_clear();
	device_set_radio_status(0);
	device_set_network_status(0);
	device_set_storage_fault(0xFFFFFFFFU, false);
}

static void command(uint8_t parameter, uint16_t value)
{
	motion_command_begin();
	motion_apply_command(parameter, value);
	motion_command_commit();
}

static bool stored(uint8_t *active, uint16_t *timeout)
{
	uint8_t raw[15];

	if (fake_settings_get_raw("dsa/motion", raw, sizeof(raw)) != 15) {
		return false;
	}
	*active = raw[12];
	*timeout = sys_get_be16(&raw[13]);
	return true;
}

static void put_record(uint8_t active, uint16_t timeout)
{
	uint8_t payload[3] = { active, (uint8_t)(timeout >> 8), (uint8_t)timeout };
	uint8_t rec[15];

	sys_put_be32(0x44534150U, &rec[0]);
	sys_put_be16(1U, &rec[4]);
	sys_put_be16(3U, &rec[6]);
	sys_put_be32(crc32_ieee(payload, 3), &rec[8]);
	memcpy(&rec[12], payload, 3);
	fake_settings_put_raw("dsa/motion", rec, sizeof(rec));
}

MT_TEST(c8_init_without_sensor_reports_motion_unavailable)
{
	MT_ASSERT_EQ(0, motion_init());
	MT_ASSERT(device_get_radio_status() & RADIO_STATUS_MOTION_UNAVAILABLE);
}

MT_TEST(c8_active_command_accepts_0_and_1_only)
{
	uint8_t active;
	uint16_t timeout;

	MT_ASSERT_EQ(0, motion_init());
	command(P_MOTION_ACTIVE, 2);
	MT_ASSERT_EQ(0, fake_settings_save_count());
	command(P_MOTION_ACTIVE, 1);
	MT_ASSERT(stored(&active, &timeout));
	MT_ASSERT_EQ(1, active);
}

MT_TEST(c8_timeout_command_rejects_zero)
{
	uint8_t active;
	uint16_t timeout;

	MT_ASSERT_EQ(0, motion_init());
	command(P_MOTION_INACTIVITY_TIMEOUT_S, 0);
	MT_ASSERT_EQ(0, fake_settings_save_count());
	command(P_MOTION_INACTIVITY_TIMEOUT_S, 65535);
	MT_ASSERT(stored(&active, &timeout));
	MT_ASSERT_EQ(65535, timeout);
}

MT_TEST(c8_commit_saves_only_on_change)
{
	MT_ASSERT_EQ(0, motion_init());
	command(P_MOTION_INACTIVITY_TIMEOUT_S, CONFIG_DSA_MOTION_INACTIVITY_TIMEOUT_S);
	MT_ASSERT_EQ(0, fake_settings_save_count());
	command(P_MOTION_INACTIVITY_TIMEOUT_S, 60);
	MT_ASSERT_EQ(1, fake_settings_save_count());
}

MT_TEST(c8_reset_restores_defaults)
{
	uint8_t active;
	uint16_t timeout;

	MT_ASSERT_EQ(0, motion_init());
	command(P_MOTION_INACTIVITY_TIMEOUT_S, 60);
	command(P_MOTION_RESET_PARAMS, 0);
	MT_ASSERT(stored(&active, &timeout));
	MT_ASSERT_EQ(CONFIG_DSA_MOTION_INACTIVITY_TIMEOUT_S, timeout);
	MT_ASSERT_EQ(IS_ENABLED(CONFIG_DSA_MOTION_DEFAULT_ACTIVE), active);
}

MT_TEST(c8_saved_parameters_loaded_at_init)
{
	uint8_t active;
	uint16_t timeout;

	put_record(1, 120);
	MT_ASSERT_EQ(0, motion_init());
	MT_ASSERT(!(device_get_storage_faults() & STORAGE_FAULT_MOTION_PARAMS));
	/* Re-commit an unrelated no-op: stored record stays as loaded. */
	command(P_MOTION_INACTIVITY_TIMEOUT_S, 120);
	MT_ASSERT_EQ(0, fake_settings_save_count());
	MT_ASSERT(stored(&active, &timeout));
	MT_ASSERT_EQ(120, timeout);
}

MT_TEST(c8_invalid_saved_record_sets_param_fault)
{
	put_record(2, 120);
	MT_ASSERT_EQ(0, motion_init());
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_MOTION_PARAMS);
	MT_ASSERT(device_get_radio_status() & STORAGE_STATUS_PARAM_ERROR);
	put_record(1, 0);
	MT_ASSERT_EQ(-EBADMSG, motion_params_load());
}

MT_TEST(c8_no_eco_without_sensor_even_when_enabled)
{
	MT_ASSERT_EQ(0, motion_init());
	command(P_MOTION_INACTIVITY_TIMEOUT_S, 1);
	command(P_MOTION_ACTIVE, 1);
	shim_advance_ms(10 * 1000);
	MT_ASSERT_EQ(0, fake_peers.eco_override_calls);
	MT_ASSERT_EQ(0, fake_peers.led_suspend_calls);
	MT_ASSERT(!(device_get_network_status() & ECO_MODE_MASK));
}

MT_TEST(c8_save_failure_sets_param_fault)
{
	MT_ASSERT_EQ(0, motion_init());
	fake_settings_set_save_error(-EIO);
	command(P_MOTION_INACTIVITY_TIMEOUT_S, 77);
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_MOTION_PARAMS);
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c8_init_without_sensor_reports_motion_unavailable),
	MT_CASE(c8_active_command_accepts_0_and_1_only),
	MT_CASE(c8_timeout_command_rejects_zero),
	MT_CASE(c8_commit_saves_only_on_change),
	MT_CASE(c8_reset_restores_defaults),
	MT_CASE(c8_saved_parameters_loaded_at_init),
	MT_CASE(c8_invalid_saved_record_sets_param_fault),
	MT_CASE(c8_no_eco_without_sensor_even_when_enabled),
	MT_CASE(c8_save_failure_sets_param_fault))
