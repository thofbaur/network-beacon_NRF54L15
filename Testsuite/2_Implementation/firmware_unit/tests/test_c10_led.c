/*
 * C10 - led.c: status blink timing, self-report flash, firework effect,
 * eco suspend/resume (non-persisted), command validation, persistence and
 * migration of older stored records. Host board pins: status LED
 * led1_green = 10, self-report LED led1_red = 11, eco LED led1_blue = 12.
 * See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>

#include "common_include.h"
#include "device.h"
#include "fake_peers.h"
#include "fake_settings.h"
#include "led.h"
#include "minitest.h"

#define STATUS_PIN 10
#define REPORT_PIN 11
#define ECO_PIN 12
#define INTERVAL_MS CONFIG_DSA_LED_BLINK_INTERVAL_MS
#define ON_MS CONFIG_DSA_LED_BLINK_ON_MS

static void setup(void)
{
	fake_settings_reset();
	fake_peers_reset();
	shim_gpio_reset();
	shim_work_reset();
	shim_set_uptime_ms(0);
	shim_log_clear();
	shim_rand_seed(1);
	device_set_storage_fault(0xFFFFFFFFU, false);
}

static void command(uint8_t parameter, uint16_t value)
{
	led_command_begin();
	led_apply_command(parameter, value);
	led_command_commit();
}

static void put_record(const uint8_t *payload, size_t len)
{
	uint8_t rec[12 + 8];

	sys_put_be32(0x44534150U, &rec[0]);
	sys_put_be16(1U, &rec[4]);
	sys_put_be16((uint16_t)len, &rec[6]);
	sys_put_be32(crc32_ieee(payload, len), &rec[8]);
	memcpy(&rec[12], payload, len);
	fake_settings_put_raw("dsa/main", rec, 12 + len);
}

static bool stored(uint8_t out[4])
{
	uint8_t raw[16];

	if (fake_settings_get_raw("dsa/main", raw, sizeof(raw)) != 16) {
		return false;
	}
	memcpy(out, &raw[12], 4);
	return true;
}

MT_TEST(c10_default_is_no_blinking)
{
	led_init();
	MT_ASSERT_EQ(0, shim_gpio_output(STATUS_PIN));
	shim_advance_ms(3 * INTERVAL_MS);
	MT_ASSERT_EQ(0, shim_gpio_set_count(STATUS_PIN));
}

MT_TEST(c10_enabled_status_led_blinks_once_per_interval)
{
	led_init();
	command(P_MAIN_LED_ACTIVE, 1);
	shim_advance_ms(INTERVAL_MS - 1);
	MT_ASSERT_EQ(0, shim_gpio_output(STATUS_PIN));
	shim_advance_ms(1);
	MT_ASSERT_EQ(1, shim_gpio_output(STATUS_PIN));
	shim_advance_ms(ON_MS);
	MT_ASSERT_EQ(0, shim_gpio_output(STATUS_PIN));
	shim_advance_ms(INTERVAL_MS);
	MT_ASSERT_EQ(1, shim_gpio_output(STATUS_PIN));
}

MT_TEST(c10_interval_command_changes_blink_period)
{
	led_init();
	command(P_MAIN_LED_INTERVAL_S, 5);
	command(P_MAIN_LED_ACTIVE, 1);
	shim_advance_ms(5000);
	MT_ASSERT_EQ(1, shim_gpio_output(STATUS_PIN));
}

MT_TEST(c10_command_validation)
{
	led_init();
	command(P_MAIN_LED_ACTIVE, 2);
	command(P_MAIN_LED_INTERVAL_S, 0);
	command(P_MAIN_FIREWORK_ACTIVE, 2);
	MT_ASSERT_EQ(0, fake_settings_save_count());
}

MT_TEST(c10_parameters_persisted_in_v3_layout)
{
	uint8_t rec[4];

	led_init();
	command(P_MAIN_LED_INTERVAL_S, 300);
	MT_ASSERT(stored(rec));
	MT_ASSERT_EQ(0, rec[0]);
	MT_ASSERT_EQ(300, sys_get_be16(&rec[1]));
	MT_ASSERT_EQ(0, rec[3]);
}

MT_TEST(c10_saved_parameters_loaded_at_init)
{
	uint8_t payload[4] = { 1, 0, 7, 0 };

	put_record(payload, 4);
	led_init();
	shim_advance_ms(7000);
	MT_ASSERT_EQ(1, shim_gpio_output(STATUS_PIN));
}

MT_TEST(c10_v2_record_migrates_keeping_default_firework)
{
	uint8_t v2[3] = { 1, 0, 9 };
	uint8_t rec[4];

	put_record(v2, 3);
	led_init();
	MT_ASSERT(stored(rec));
	MT_ASSERT_EQ(1, rec[0]);
	MT_ASSERT_EQ(9, sys_get_be16(&rec[1]));
	MT_ASSERT_EQ(IS_ENABLED(CONFIG_DSA_DEFAULT_FIREWORK_ACTIVE), rec[3]);
}

MT_TEST(c10_v1_record_migrates_with_default_interval)
{
	uint8_t v1[1] = { 1 };
	uint8_t rec[4];

	put_record(v1, 1);
	led_init();
	MT_ASSERT(stored(rec));
	MT_ASSERT_EQ(1, rec[0]);
	MT_ASSERT_EQ(INTERVAL_MS / 1000, sys_get_be16(&rec[1]));
}

MT_TEST(c10_unversioned_legacy_byte_migrates)
{
	uint8_t legacy = 1;
	uint8_t rec[4];

	fake_settings_put_raw("dsa/main", &legacy, 1);
	led_init();
	MT_ASSERT(stored(rec));
	MT_ASSERT_EQ(1, rec[0]);
}

MT_TEST(c10_invalid_saved_record_sets_param_fault)
{
	uint8_t payload[4] = { 1, 0, 0, 0 }; /* interval 0 */

	put_record(payload, 4);
	led_init();
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_LED_PARAMS);
}

MT_TEST(c10_self_report_flash_lasts_configured_time)
{
	led_init();
	led_signal_self_report();
	MT_ASSERT_EQ(1, shim_gpio_output(REPORT_PIN));
	shim_advance_ms(CONFIG_DSA_SELF_REPORT_LED_ON_MS - 1);
	MT_ASSERT_EQ(1, shim_gpio_output(REPORT_PIN));
	shim_advance_ms(1);
	MT_ASSERT_EQ(0, shim_gpio_output(REPORT_PIN));
}

MT_TEST(c10_suspend_stops_blinking_and_resume_restarts)
{
	led_init();
	command(P_MAIN_LED_ACTIVE, 1);
	led_suspend_blinking();
	shim_advance_ms(3 * INTERVAL_MS);
	MT_ASSERT_EQ(0, shim_gpio_output(STATUS_PIN));
	MT_ASSERT_EQ(0, shim_gpio_output(ECO_PIN)); /* dev indicator off in production */
	led_resume_blinking();
	shim_advance_ms(INTERVAL_MS);
	MT_ASSERT_EQ(1, shim_gpio_output(STATUS_PIN));
}

MT_TEST(c10_suspend_does_not_persist)
{
	led_init();
	command(P_MAIN_LED_ACTIVE, 1);
	MT_ASSERT_EQ(1, fake_settings_save_count());
	led_suspend_blinking();
	led_resume_blinking();
	MT_ASSERT_EQ(1, fake_settings_save_count());
}

MT_TEST(c10_firework_flickers_and_stops_cleanly)
{
	int toggles;

	led_init();
	command(P_MAIN_FIREWORK_ACTIVE, 1);
	shim_advance_ms(5000);
	toggles = shim_gpio_set_count(STATUS_PIN) + shim_gpio_set_count(REPORT_PIN) +
		  shim_gpio_set_count(ECO_PIN);
	MT_ASSERT(toggles > 20);
	command(P_MAIN_FIREWORK_ACTIVE, 0);
	MT_ASSERT_EQ(0, shim_gpio_output(STATUS_PIN));
	MT_ASSERT_EQ(0, shim_gpio_output(REPORT_PIN));
	MT_ASSERT_EQ(0, shim_gpio_output(ECO_PIN));
	toggles = shim_gpio_set_count(STATUS_PIN);
	shim_advance_ms(5000);
	MT_ASSERT_EQ(toggles, shim_gpio_set_count(STATUS_PIN));
}

MT_TEST(c10_firework_keeps_running_through_eco_suspend)
{
	int before;

	led_init();
	command(P_MAIN_FIREWORK_ACTIVE, 1);
	shim_advance_ms(1000);
	led_suspend_blinking();
	before = shim_gpio_set_count(STATUS_PIN) + shim_gpio_set_count(REPORT_PIN) +
		 shim_gpio_set_count(ECO_PIN);
	shim_advance_ms(2000);
	MT_ASSERT(shim_gpio_set_count(STATUS_PIN) + shim_gpio_set_count(REPORT_PIN) +
		  shim_gpio_set_count(ECO_PIN) > before + 5);
}

MT_TEST(c10_saved_firework_starts_at_boot)
{
	uint8_t payload[4] = { 0, 0, 20, 1 };

	put_record(payload, 4);
	led_init();
	shim_advance_ms(2000);
	MT_ASSERT(shim_gpio_set_count(STATUS_PIN) + shim_gpio_set_count(REPORT_PIN) +
		  shim_gpio_set_count(ECO_PIN) > 5);
}

MT_TEST(c10_reset_restores_defaults)
{
	uint8_t rec[4];

	led_init();
	command(P_MAIN_LED_ACTIVE, 1);
	command(P_MAIN_LED_INTERVAL_S, 3);
	command(P_MAIN_RESET_PARAMS, 0);
	MT_ASSERT(stored(rec));
	MT_ASSERT_EQ(IS_ENABLED(CONFIG_DSA_DEFAULT_LED_ACTIVE), rec[0]);
	MT_ASSERT_EQ(INTERVAL_MS / 1000, sys_get_be16(&rec[1]));
}

MT_TEST(c10_missing_status_led_degrades_gracefully)
{
	shim_gpio_set_configure_error(STATUS_PIN, -EIO);
	led_init();
	command(P_MAIN_LED_ACTIVE, 1);
	shim_advance_ms(2 * INTERVAL_MS);
	MT_ASSERT_EQ(0, shim_gpio_set_count(STATUS_PIN));
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c10_default_is_no_blinking),
	MT_CASE(c10_enabled_status_led_blinks_once_per_interval),
	MT_CASE(c10_interval_command_changes_blink_period),
	MT_CASE(c10_command_validation),
	MT_CASE(c10_parameters_persisted_in_v3_layout),
	MT_CASE(c10_saved_parameters_loaded_at_init),
	MT_CASE(c10_v2_record_migrates_keeping_default_firework),
	MT_CASE(c10_v1_record_migrates_with_default_interval),
	MT_CASE(c10_unversioned_legacy_byte_migrates),
	MT_CASE(c10_invalid_saved_record_sets_param_fault),
	MT_CASE(c10_self_report_flash_lasts_configured_time),
	MT_CASE(c10_suspend_stops_blinking_and_resume_restarts),
	MT_CASE(c10_suspend_does_not_persist),
	MT_CASE(c10_firework_flickers_and_stops_cleanly),
	MT_CASE(c10_firework_keeps_running_through_eco_suspend),
	MT_CASE(c10_saved_firework_starts_at_boot),
	MT_CASE(c10_reset_restores_defaults),
	MT_CASE(c10_missing_status_led_degrades_gracefully))
