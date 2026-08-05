#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "battery_voltage.h"
#include "common_include.h"
#include "device.h"

#define BATTERY_ADC_NODE DT_NODELABEL(adc)
#define BATTERY_ADC_CHANNEL 0
#define BATTERY_ADC_REFERENCE_MV 900
#define BATTERY_ADC_RESOLUTION 14
#define BATTERY_ADC_OVERSAMPLING 2

BUILD_ASSERT(BATTERY_LEVEL_1_THRESHOLD_MV > BATTERY_LEVEL_2_THRESHOLD_MV,
	     "Battery status thresholds must be descending");
BUILD_ASSERT(BATTERY_LEVEL_2_THRESHOLD_MV > BATTERY_LEVEL_3_THRESHOLD_MV,
	     "Battery status thresholds must be descending");

static const struct device *const battery_adc =
	DEVICE_DT_GET(BATTERY_ADC_NODE);
static bool battery_adc_ready;
static atomic_t stored_voltage_mv = ATOMIC_INIT(UINT16_MAX);

static int battery_voltage_sample_mv(uint16_t *voltage_mv);
static int battery_voltage_measure_store(void);
static void battery_voltage_measure_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(battery_voltage_measure_work,
			       battery_voltage_measure_handler);

int battery_voltage_init(void)
{
	const struct adc_channel_cfg channel_cfg = {
		.gain = ADC_GAIN_1_4,
		.reference = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id = BATTERY_ADC_CHANNEL,
		.differential = false,
		.input_positive = NRF_SAADC_VDD,
	};
	int err;

	if (!device_is_ready(battery_adc)) {
		printk("Battery ADC device not ready\n");
		return -ENODEV;
	}

	err = adc_channel_setup(battery_adc, &channel_cfg);
	if (err) {
		printk("Battery ADC channel setup failed (err %d)\n", err);
		return err;
	}

	battery_adc_ready = true;
	printk("Battery voltage measurement initialized\n");

	err = battery_voltage_measure_store();
	if (err) {
		printk("Initial battery voltage measurement failed (err %d)\n",
		       err);
	}

	k_work_schedule(&battery_voltage_measure_work,
			K_MSEC(CONFIG_DSA_BATTERY_MEASURE_INTERVAL_MS));
	return 0;
}

int battery_voltage_read_mv(uint16_t *voltage_mv)
{
	atomic_val_t stored;

	if (!voltage_mv) {
		return -EINVAL;
	}

	stored = atomic_get(&stored_voltage_mv);
	if (stored == UINT16_MAX) {
		return -ENODATA;
	}

	*voltage_mv = (uint16_t)stored;
	return 0;
}

uint8_t battery_voltage_status_from_mv(uint16_t voltage_mv)
{
	if (voltage_mv < BATTERY_LEVEL_3_THRESHOLD_MV) {
		return 3U;
	}
	if (voltage_mv < BATTERY_LEVEL_2_THRESHOLD_MV) {
		return 2U;
	}
	if (voltage_mv < BATTERY_LEVEL_1_THRESHOLD_MV) {
		return 1U;
	}

	return 0U;
}

static int battery_voltage_sample_mv(uint16_t *voltage_mv)
{
	int16_t sample;
	int32_t sample_mv;
	int err;
	struct adc_sequence sequence = {
		.channels = BIT(BATTERY_ADC_CHANNEL),
		.buffer = &sample,
		.buffer_size = sizeof(sample),
		.resolution = BATTERY_ADC_RESOLUTION,
		.oversampling = BATTERY_ADC_OVERSAMPLING,
	};

	if (!voltage_mv) {
		return -EINVAL;
	}

	if (!battery_adc_ready) {
		return -ENODEV;
	}

	err = adc_read(battery_adc, &sequence);
	if (err) {
		printk("Battery ADC read failed (err %d)\n", err);
		return err;
	}

	sample_mv = sample;
	err = adc_raw_to_millivolts(BATTERY_ADC_REFERENCE_MV,
				    ADC_GAIN_1_4,
				    BATTERY_ADC_RESOLUTION,
				    &sample_mv);
	if (err) {
		printk("Battery ADC conversion failed (err %d)\n", err);
		return err;
	}

	if (sample_mv < 0 || sample_mv > UINT16_MAX) {
		return -ERANGE;
	}

	*voltage_mv = (uint16_t)sample_mv;
	return 0;
}

static int battery_voltage_measure_store(void)
{
	uint16_t voltage_mv;
	uint8_t battery_status;
	int err;

	err = battery_voltage_sample_mv(&voltage_mv);
	if (err) {
		return err;
	}

	atomic_set(&stored_voltage_mv, voltage_mv);
	battery_status = battery_voltage_status_from_mv(voltage_mv);
	device_set_network_status_bits(
		BATTERY_LEVEL_MASK,
		battery_status << P_SHIFT_STATUS_BATTERY);

	printk("Measured battery voltage %u mV, status %u\n",
	       voltage_mv, battery_status);
	return 0;
}

static void battery_voltage_measure_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	err = battery_voltage_measure_store();
	if (err) {
		printk("Periodic battery voltage measurement failed (err %d)\n",
		       err);
	}

	k_work_schedule(&battery_voltage_measure_work,
			K_MSEC(CONFIG_DSA_BATTERY_MEASURE_INTERVAL_MS));
}
