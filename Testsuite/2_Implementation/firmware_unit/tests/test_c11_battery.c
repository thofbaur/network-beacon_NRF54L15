/*
 * C11 - battery_voltage.c: ADC raw value -> millivolts (900 mV reference,
 * gain 1/4, 14 bit), voltage -> battery status 0..3, the status field in
 * bits 6..4 of the network status byte (decision 2026-08-05, mask 0x70),
 * and the periodic measurement. See 1_Specification/C_firmware_unit.md.
 */
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>

#include "battery_voltage.h"
#include "common_include.h"
#include "device.h"
#include "fake_peers.h"
#include "minitest.h"

/* raw = mV * 2^14 / (900 * 4), rounded up so the conversion floors to mV. */
static int16_t raw_for_mv(uint32_t mv)
{
	return (int16_t)((mv * 16384U + 3599U) / 3600U);
}

static void setup(void)
{
	shim_adc_reset();
	shim_work_reset();
	shim_set_uptime_ms(0);
	shim_generic_device.ready = true;
	fake_peers_reset();
	device_set_network_status(0);
}

static uint8_t battery_bits(void)
{
	return (device_get_network_status() & BATTERY_LEVEL_MASK) >> P_SHIFT_STATUS_BATTERY;
}

MT_TEST(c11_status_thresholds)
{
	MT_ASSERT_EQ(0, battery_voltage_status_from_mv(3300));
	MT_ASSERT_EQ(0, battery_voltage_status_from_mv(BATTERY_LEVEL_1_THRESHOLD_MV));
	MT_ASSERT_EQ(1, battery_voltage_status_from_mv(BATTERY_LEVEL_1_THRESHOLD_MV - 1));
	MT_ASSERT_EQ(1, battery_voltage_status_from_mv(BATTERY_LEVEL_2_THRESHOLD_MV));
	MT_ASSERT_EQ(2, battery_voltage_status_from_mv(BATTERY_LEVEL_2_THRESHOLD_MV - 1));
	MT_ASSERT_EQ(2, battery_voltage_status_from_mv(BATTERY_LEVEL_3_THRESHOLD_MV));
	MT_ASSERT_EQ(3, battery_voltage_status_from_mv(BATTERY_LEVEL_3_THRESHOLD_MV - 1));
	MT_ASSERT_EQ(3, battery_voltage_status_from_mv(0));
}

MT_TEST(c11_status_fits_three_bit_field)
{
	for (uint32_t mv = 0; mv <= 4000; mv += 10) {
		uint8_t status = battery_voltage_status_from_mv((uint16_t)mv);

		MT_ASSERT(status <= 3);
		MT_ASSERT_EQ(0, (status << P_SHIFT_STATUS_BATTERY) & ~BATTERY_LEVEL_MASK);
	}
}

MT_TEST(c11_init_measures_and_publishes_voltage)
{
	uint16_t mv = 0;

	shim_adc_set_raw(raw_for_mv(3000));
	MT_ASSERT_EQ(0, battery_voltage_init());
	MT_ASSERT_EQ(0, battery_voltage_read_mv(&mv));
	MT_ASSERT_EQ(3000, mv);
	MT_ASSERT_EQ(0, battery_bits());
}

MT_TEST(c11_low_voltage_sets_status_3_without_touching_other_bits)
{
	device_set_network_status(0x85); /* eco bit + data level 5 */
	shim_adc_set_raw(raw_for_mv(2500));
	MT_ASSERT_EQ(0, battery_voltage_init());
	MT_ASSERT_EQ(3, battery_bits());
	MT_ASSERT_EQ(0x85 | (3 << 4), device_get_network_status());
}

MT_TEST(c11_read_before_measurement_is_enodata)
{
	uint16_t mv;

	MT_ASSERT_EQ(-ENODATA, battery_voltage_read_mv(&mv));
	MT_ASSERT_EQ(-EINVAL, battery_voltage_read_mv(NULL));
}

MT_TEST(c11_adc_not_ready_fails_init)
{
	shim_generic_device.ready = false;
	MT_ASSERT_EQ(-ENODEV, battery_voltage_init());
	shim_generic_device.ready = true;
}

MT_TEST(c11_channel_setup_error_fails_init)
{
	shim_adc_set_setup_error(-EIO);
	MT_ASSERT_EQ(-EIO, battery_voltage_init());
}

MT_TEST(c11_periodic_measurement_updates_status)
{
	uint16_t mv;

	shim_adc_set_raw(raw_for_mv(3100));
	MT_ASSERT_EQ(0, battery_voltage_init());
	MT_ASSERT_EQ(1, shim_adc_read_count());
	shim_adc_set_raw(raw_for_mv(2600));
	shim_advance_ms(CONFIG_DSA_BATTERY_MEASURE_INTERVAL_MS - 1);
	MT_ASSERT_EQ(1, shim_adc_read_count());
	shim_advance_ms(1);
	MT_ASSERT_EQ(2, shim_adc_read_count());
	MT_ASSERT_EQ(0, battery_voltage_read_mv(&mv));
	MT_ASSERT_EQ(2600, mv);
	MT_ASSERT_EQ(2, battery_bits());
}

MT_TEST(c11_failed_read_keeps_last_value)
{
	uint16_t mv;

	shim_adc_set_raw(raw_for_mv(3100));
	MT_ASSERT_EQ(0, battery_voltage_init());
	shim_adc_set_read_error(-EIO);
	shim_advance_ms(CONFIG_DSA_BATTERY_MEASURE_INTERVAL_MS);
	MT_ASSERT_EQ(0, battery_voltage_read_mv(&mv));
	MT_ASSERT_EQ(3100, mv);
	shim_adc_set_read_error(0);
	shim_advance_ms(CONFIG_DSA_BATTERY_MEASURE_INTERVAL_MS);
	MT_ASSERT_EQ(3, shim_adc_read_count());
}

MT_TEST(c11_negative_sample_is_rejected)
{
	uint16_t mv;

	shim_adc_set_raw(-5);
	MT_ASSERT_EQ(0, battery_voltage_init());
	MT_ASSERT_EQ(-ENODATA, battery_voltage_read_mv(&mv));
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c11_status_thresholds),
	MT_CASE(c11_status_fits_three_bit_field),
	MT_CASE(c11_init_measures_and_publishes_voltage),
	MT_CASE(c11_low_voltage_sets_status_3_without_touching_other_bits),
	MT_CASE(c11_read_before_measurement_is_enodata),
	MT_CASE(c11_adc_not_ready_fails_init),
	MT_CASE(c11_channel_setup_error_fails_init),
	MT_CASE(c11_periodic_measurement_updates_status),
	MT_CASE(c11_failed_read_keeps_last_value),
	MT_CASE(c11_negative_sample_is_rejected))
