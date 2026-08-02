#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/dt-bindings/adc/nrf-saadc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "battery_voltage.h"

#define BATTERY_ADC_NODE DT_NODELABEL(adc)
#define BATTERY_ADC_CHANNEL 0
#define BATTERY_ADC_REFERENCE_MV 900
#define BATTERY_ADC_RESOLUTION 14
#define BATTERY_ADC_OVERSAMPLING 2

static const struct device *const battery_adc =
	DEVICE_DT_GET(BATTERY_ADC_NODE);
static bool battery_adc_ready;

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
	return 0;
}

int battery_voltage_read_mv(uint16_t *voltage_mv)
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
