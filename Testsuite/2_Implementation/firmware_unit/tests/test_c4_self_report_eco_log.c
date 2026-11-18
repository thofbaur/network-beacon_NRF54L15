/*
 * C4 - self_report.c and eco_log.c on top of the real ram_log_ring.c,
 * *_storage.c and flash_ring_store.c: button debounce/long-press, 24-bit
 * timestamps, eco session pairing, flush/export ordering and the
 * storage-full signal. See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "device.h"
#include "eco_log.h"
#include "fake_nvs.h"
#include "fake_peers.h"
#include "minitest.h"
#include "self_report.h"

#define BUTTON_PIN 20 /* button0 in the host devicetree shim */
#define DEBOUNCE_MS 40
#define LONG_PRESS_MS CONFIG_DSA_SELF_REPORT_LONG_PRESS_MS

static void setup(void)
{
	fake_nvs_reset_all();
	fake_peers_reset();
	shim_gpio_reset();
	shim_work_reset();
	shim_set_uptime_ms(0);
	shim_log_clear();
	shim_flash_set_page_info_error(0);
	device_set_radio_status(0);
	device_set_storage_fault(0xFFFFFFFFU, false);
	device_set_storage_full(0xFFFFFFFFU, false);
}

static uint16_t self_reports(void)
{
	uint16_t count = 0;

	MT_ASSERT_EQ(0, self_report_get_count(&count));
	return count;
}

static uint16_t eco_entries(void)
{
	uint16_t count = 0;

	MT_ASSERT_EQ(0, eco_log_get_count(&count));
	return count;
}

static void press(void)
{
	shim_gpio_set_input(BUTTON_PIN, 1);
	shim_gpio_fire(BUTTON_PIN);
}

static void release(void)
{
	shim_gpio_set_input(BUTTON_PIN, 0);
	shim_gpio_fire(BUTTON_PIN);
}

/* ---- self-report ------------------------------------------------------- */

MT_TEST(c4_long_press_records_one_self_report)
{
	uint8_t buf[30];
	uint16_t written;

	MT_ASSERT_EQ(0, self_report_init());
	shim_advance_ms(100000);
	press();
	shim_advance_ms(DEBOUNCE_MS + LONG_PRESS_MS);
	MT_ASSERT_EQ(1, self_reports());
	MT_ASSERT_EQ(1, fake_peers.led_self_report_signals);
	MT_ASSERT_EQ(0, self_report_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(3, written);
	MT_ASSERT_EQ((100000 + DEBOUNCE_MS + LONG_PRESS_MS) / 1000, sys_get_be24(buf));
}

MT_TEST(c4_short_press_records_nothing)
{
	MT_ASSERT_EQ(0, self_report_init());
	press();
	shim_advance_ms(DEBOUNCE_MS + LONG_PRESS_MS / 2);
	release();
	shim_advance_ms(DEBOUNCE_MS + 2 * LONG_PRESS_MS);
	MT_ASSERT_EQ(0, self_reports());
}

MT_TEST(c4_bouncing_contact_is_debounced)
{
	MT_ASSERT_EQ(0, self_report_init());
	for (int i = 0; i < 5; i++) {
		press();
		shim_advance_ms(5);
		release();
		shim_advance_ms(5);
	}
	press();
	shim_advance_ms(DEBOUNCE_MS + LONG_PRESS_MS);
	MT_ASSERT_EQ(1, self_reports());
}

MT_TEST(c4_holding_button_records_only_once)
{
	MT_ASSERT_EQ(0, self_report_init());
	press();
	shim_advance_ms(DEBOUNCE_MS + 5 * LONG_PRESS_MS);
	MT_ASSERT_EQ(1, self_reports());
}

MT_TEST(c4_button_held_at_boot_records_after_long_press)
{
	shim_gpio_set_input(BUTTON_PIN, 1);
	MT_ASSERT_EQ(0, self_report_init());
	shim_advance_ms(LONG_PRESS_MS);
	MT_ASSERT_EQ(1, self_reports());
}

MT_TEST(c4_self_report_time_is_low_24_bits)
{
	uint8_t buf[3];
	uint16_t written;

	MT_ASSERT_EQ(0, self_report_init());
	self_report_development_validation_append(0x01000010U);
	MT_ASSERT_EQ(0, self_report_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(0x10, sys_get_be24(buf));
}

MT_TEST(c4_self_reports_flush_to_flash_and_export_in_order)
{
	uint8_t buf[240];
	uint32_t next = 0;
	uint16_t written;

	MT_ASSERT_EQ(0, self_report_init());
	for (uint32_t i = 0; i < 300; i++) {
		self_report_development_validation_append(i);
		shim_run_ready_work();
	}
	MT_ASSERT_EQ(300, self_reports());
	for (;;) {
		MT_ASSERT_EQ(0, self_report_export_begin(buf, sizeof(buf), &written));
		if (written == 0) {
			break;
		}
		for (uint16_t off = 0; off < written; off += 3) {
			MT_ASSERT_EQ(next, sys_get_be24(&buf[off]));
			next++;
		}
		MT_ASSERT_EQ(0, self_report_export_commit());
		shim_run_ready_work();
	}
	MT_ASSERT_EQ(300, next);
	MT_ASSERT_EQ(0, self_reports());
}

MT_TEST(c4_report_during_full_export_is_not_signalled)
{
	uint8_t buf[3];
	uint16_t written;

	MT_ASSERT_EQ(0, self_report_init());
	for (uint32_t i = 0; i < CONFIG_DSA_SELF_REPORT_RING_COUNT; i++) {
		self_report_development_validation_append(i);
	}
	fake_peers.led_self_report_signals = 0;
	MT_ASSERT_EQ(0, self_report_export_begin(buf, sizeof(buf), &written));
	press();
	shim_advance_ms(DEBOUNCE_MS + LONG_PRESS_MS);
	MT_ASSERT_EQ(0, fake_peers.led_self_report_signals);
}

MT_TEST(c4_self_report_storage_full_is_signalled)
{
	MT_ASSERT_EQ(0, self_report_init());
	for (uint32_t i = 0; i < 4 * 64 + CONFIG_DSA_SELF_REPORT_RING_COUNT + 200; i++) {
		self_report_development_validation_append(i);
		shim_run_ready_work();
	}
	MT_ASSERT_MSG(device_get_radio_status() & STORAGE_STATUS_STORAGE_FULL,
		      "self-reports were discarded (%u kept of %u) but the storage-full "
		      "bit is not set", self_reports(),
		      4 * 64 + CONFIG_DSA_SELF_REPORT_RING_COUNT + 200);
}

MT_TEST(c4_self_report_storage_init_failure_is_reported)
{
	shim_flash_set_page_info_error(-EIO);
	MT_ASSERT_EQ(-EIO, self_report_init());
	MT_ASSERT(device_get_storage_faults() & STORAGE_FAULT_SELF_REPORT_INIT);
}

MT_TEST(c4_button_gpio_failure_keeps_storage_usable)
{
	shim_gpio_set_configure_error(BUTTON_PIN, -EIO);
	MT_ASSERT_EQ(0, self_report_init());
	self_report_development_validation_append(5);
	MT_ASSERT_EQ(1, self_reports());
}

/* ---- eco log ----------------------------------------------------------- */

MT_TEST(c4_eco_session_records_enter_and_leave_uptime)
{
	uint8_t buf[12];
	uint16_t written;

	MT_ASSERT_EQ(0, eco_log_init());
	shim_set_uptime_ms(100 * 1000);
	eco_log_enter();
	shim_set_uptime_ms(160 * 1000 + 500);
	eco_log_leave();
	MT_ASSERT_EQ(1, eco_entries());
	MT_ASSERT_EQ(0, eco_log_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(6, written);
	MT_ASSERT_EQ(100, sys_get_be24(&buf[0]));
	MT_ASSERT_EQ(160, sys_get_be24(&buf[3]));
}

MT_TEST(c4_leave_without_enter_records_nothing)
{
	MT_ASSERT_EQ(0, eco_log_init());
	eco_log_leave();
	MT_ASSERT_EQ(0, eco_entries());
}

MT_TEST(c4_second_enter_restarts_session)
{
	uint8_t buf[12];
	uint16_t written;

	MT_ASSERT_EQ(0, eco_log_init());
	shim_set_uptime_ms(10 * 1000);
	eco_log_enter();
	shim_set_uptime_ms(20 * 1000);
	eco_log_enter();
	shim_set_uptime_ms(30 * 1000);
	eco_log_leave();
	eco_log_leave();
	MT_ASSERT_EQ(1, eco_entries());
	MT_ASSERT_EQ(0, eco_log_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(20, sys_get_be24(&buf[0]));
}

MT_TEST(c4_eco_times_are_low_24_bits)
{
	uint8_t buf[6];
	uint16_t written;

	MT_ASSERT_EQ(0, eco_log_init());
	shim_set_uptime_ms(0x01000001LL * 1000);
	eco_log_enter();
	shim_set_uptime_ms(0x01000002LL * 1000);
	eco_log_leave();
	MT_ASSERT_EQ(0, eco_log_export_begin(buf, sizeof(buf), &written));
	MT_ASSERT_EQ(1, sys_get_be24(&buf[0]));
	MT_ASSERT_EQ(2, sys_get_be24(&buf[3]));
}

MT_TEST(c4_eco_sessions_flush_and_export_in_order)
{
	uint8_t buf[240];
	uint32_t next = 0;
	uint16_t written;

	MT_ASSERT_EQ(0, eco_log_init());
	for (uint32_t i = 0; i < 100; i++) {
		shim_set_uptime_ms((int64_t)(2 * i) * 1000);
		eco_log_enter();
		shim_set_uptime_ms((int64_t)(2 * i + 1) * 1000);
		eco_log_leave();
		shim_run_ready_work();
	}
	MT_ASSERT_EQ(100, eco_entries());
	for (;;) {
		MT_ASSERT_EQ(0, eco_log_export_begin(buf, sizeof(buf), &written));
		if (written == 0) {
			break;
		}
		for (uint16_t off = 0; off < written; off += 6) {
			MT_ASSERT_EQ(2 * next, sys_get_be24(&buf[off]));
			MT_ASSERT_EQ(2 * next + 1, sys_get_be24(&buf[off + 3]));
			next++;
		}
		MT_ASSERT_EQ(0, eco_log_export_commit());
		shim_run_ready_work();
	}
	MT_ASSERT_EQ(100, next);
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c4_long_press_records_one_self_report),
	MT_CASE(c4_short_press_records_nothing),
	MT_CASE(c4_bouncing_contact_is_debounced),
	MT_CASE(c4_holding_button_records_only_once),
	MT_CASE(c4_button_held_at_boot_records_after_long_press),
	MT_CASE(c4_self_report_time_is_low_24_bits),
	MT_CASE(c4_self_reports_flush_to_flash_and_export_in_order),
	MT_CASE(c4_report_during_full_export_is_not_signalled),
	MT_CASE(c4_self_report_storage_full_is_signalled),
	MT_CASE(c4_self_report_storage_init_failure_is_reported),
	MT_CASE(c4_button_gpio_failure_keeps_storage_usable),
	MT_CASE(c4_eco_session_records_enter_and_leave_uptime),
	MT_CASE(c4_leave_without_enter_records_nothing),
	MT_CASE(c4_second_enter_restarts_session),
	MT_CASE(c4_eco_times_are_low_24_bits),
	MT_CASE(c4_eco_sessions_flush_and_export_in_order))
