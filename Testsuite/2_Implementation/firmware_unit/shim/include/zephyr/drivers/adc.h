#ifndef SHIM_ZEPHYR_DRIVERS_ADC_H
#define SHIM_ZEPHYR_DRIVERS_ADC_H

#include "shim_common.h"
#include <zephyr/device.h>

enum adc_gain {
	ADC_GAIN_1_6,
	ADC_GAIN_1_5,
	ADC_GAIN_1_4,
	ADC_GAIN_1_3,
	ADC_GAIN_1_2,
	ADC_GAIN_1,
	ADC_GAIN_2,
};

enum adc_reference {
	ADC_REF_VDD_1,
	ADC_REF_INTERNAL,
	ADC_REF_EXTERNAL0,
};

#define ADC_ACQ_TIME_DEFAULT 0

struct adc_channel_cfg {
	enum adc_gain gain;
	enum adc_reference reference;
	uint16_t acquisition_time;
	uint8_t channel_id;
	uint8_t differential;
	uint8_t input_positive;
	uint8_t input_negative;
};

struct adc_sequence {
	const void *options;
	uint32_t channels;
	void *buffer;
	size_t buffer_size;
	uint8_t resolution;
	uint8_t oversampling;
	bool calibrate;
};

int adc_channel_setup(const struct device *dev,
		      const struct adc_channel_cfg *channel_cfg);
int adc_read(const struct device *dev, const struct adc_sequence *sequence);

/* Same arithmetic as Zephyr's adc_raw_to_millivolts(): multiply by the
 * reference, invert the gain, shift by the resolution.
 */
static inline int adc_raw_to_millivolts(int32_t ref_mv, enum adc_gain gain,
					uint8_t resolution, int32_t *valp)
{
	int64_t adc_mv = (int64_t)*valp * (int64_t)ref_mv;

	switch (gain) {
	case ADC_GAIN_1_6:
		adc_mv *= 6;
		break;
	case ADC_GAIN_1_5:
		adc_mv *= 5;
		break;
	case ADC_GAIN_1_4:
		adc_mv *= 4;
		break;
	case ADC_GAIN_1_3:
		adc_mv *= 3;
		break;
	case ADC_GAIN_1_2:
		adc_mv *= 2;
		break;
	case ADC_GAIN_1:
		break;
	default:
		return -EINVAL;
	}
	*valp = (int32_t)(adc_mv >> resolution);
	return 0;
}

/* ---- test control ------------------------------------------------------ */
void shim_adc_reset(void);
void shim_adc_set_raw(int16_t raw);
void shim_adc_set_setup_error(int err);
void shim_adc_set_read_error(int err);
int shim_adc_read_count(void);

#endif /* SHIM_ZEPHYR_DRIVERS_ADC_H */
