#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/gpio.h>

#include "fake_nvs.h"

static struct device_state generic_state = { .initialized = true };
struct device shim_generic_device = { .name = "shim-generic",
				      .ready = true,
				      .state = &generic_state };
struct device shim_flash_device = { .name = "shim-flash", .ready = true };

/* ---- GPIO -------------------------------------------------------------- */
static int gpio_out[SHIM_GPIO_PINS];
static int gpio_set_count[SHIM_GPIO_PINS];
static int gpio_in[SHIM_GPIO_PINS];
static int gpio_configure_err[SHIM_GPIO_PINS];
static int gpio_get_err[SHIM_GPIO_PINS];
static struct gpio_callback *gpio_callbacks;

void shim_gpio_reset(void)
{
	for (unsigned int i = 0; i < SHIM_GPIO_PINS; i++) {
		gpio_out[i] = -1;
		gpio_set_count[i] = 0;
		gpio_in[i] = 0;
		gpio_configure_err[i] = 0;
		gpio_get_err[i] = 0;
	}
	gpio_callbacks = NULL;
	shim_generic_device.ready = true;
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, gpio_flags_t flags)
{
	if (gpio_configure_err[spec->pin]) {
		return gpio_configure_err[spec->pin];
	}
	if (flags & GPIO_OUTPUT) {
		gpio_out[spec->pin] = 0;
	}
	return 0;
}

int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value)
{
	gpio_out[spec->pin] = value ? 1 : 0;
	gpio_set_count[spec->pin]++;
	return 0;
}

int gpio_pin_toggle_dt(const struct gpio_dt_spec *spec)
{
	gpio_out[spec->pin] = gpio_out[spec->pin] == 1 ? 0 : 1;
	gpio_set_count[spec->pin]++;
	return 0;
}

int gpio_pin_get_dt(const struct gpio_dt_spec *spec)
{
	if (gpio_get_err[spec->pin]) {
		return gpio_get_err[spec->pin];
	}
	return gpio_in[spec->pin];
}

int gpio_add_callback(const struct device *port, struct gpio_callback *callback)
{
	ARG_UNUSED(port);
	callback->next = gpio_callbacks;
	gpio_callbacks = callback;
	return 0;
}

int gpio_remove_callback(const struct device *port,
			 struct gpio_callback *callback)
{
	struct gpio_callback **it = &gpio_callbacks;

	ARG_UNUSED(port);
	while (*it) {
		if (*it == callback) {
			*it = callback->next;
			return 0;
		}
		it = &(*it)->next;
	}
	return -EINVAL;
}

int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *spec,
				    gpio_flags_t flags)
{
	ARG_UNUSED(spec);
	ARG_UNUSED(flags);
	return 0;
}

int shim_gpio_output(unsigned int pin)
{
	return gpio_out[pin];
}

int shim_gpio_set_count(unsigned int pin)
{
	return gpio_set_count[pin];
}

void shim_gpio_set_input(unsigned int pin, int value)
{
	gpio_in[pin] = value;
}

void shim_gpio_fire(unsigned int pin)
{
	for (struct gpio_callback *cb = gpio_callbacks; cb; cb = cb->next) {
		if (cb->pin_mask & BIT(pin)) {
			cb->handler(&shim_generic_device, cb, BIT(pin));
		}
	}
}

void shim_gpio_set_configure_error(unsigned int pin, int err)
{
	gpio_configure_err[pin] = err;
}

void shim_gpio_set_get_error(unsigned int pin, int err)
{
	gpio_get_err[pin] = err;
}

/* ---- ADC --------------------------------------------------------------- */
static int16_t adc_raw;
static int adc_setup_err;
static int adc_read_err;
static int adc_reads;

void shim_adc_reset(void)
{
	adc_raw = 0;
	adc_setup_err = 0;
	adc_read_err = 0;
	adc_reads = 0;
}

void shim_adc_set_raw(int16_t raw)
{
	adc_raw = raw;
}

void shim_adc_set_setup_error(int err)
{
	adc_setup_err = err;
}

void shim_adc_set_read_error(int err)
{
	adc_read_err = err;
}

int shim_adc_read_count(void)
{
	return adc_reads;
}

int adc_channel_setup(const struct device *dev,
		      const struct adc_channel_cfg *channel_cfg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_cfg);
	return adc_setup_err;
}

int adc_read(const struct device *dev, const struct adc_sequence *sequence)
{
	ARG_UNUSED(dev);
	adc_reads++;
	if (adc_read_err) {
		return adc_read_err;
	}
	if (sequence->buffer_size < sizeof(int16_t)) {
		return -ENOMEM;
	}
	memcpy(sequence->buffer, &adc_raw, sizeof(adc_raw));
	return 0;
}

/* ---- flash ------------------------------------------------------------- */
static size_t flash_page_size = 4096;
static int flash_page_info_err;
static int flash_erase_err;
static int flash_erases;

void shim_flash_set_page_size(size_t size)
{
	flash_page_size = size;
}

void shim_flash_set_page_info_error(int err)
{
	flash_page_info_err = err;
}

void shim_flash_set_erase_error(int err)
{
	flash_erase_err = err;
}

int shim_flash_erase_count(void)
{
	return flash_erases;
}

int flash_get_page_info_by_offs(const struct device *dev, off_t offset,
				struct flash_pages_info *info)
{
	ARG_UNUSED(dev);
	if (flash_page_info_err) {
		return flash_page_info_err;
	}
	info->size = flash_page_size;
	info->start_offset = offset - (offset % (off_t)flash_page_size);
	info->index = (uint32_t)(offset / (off_t)flash_page_size);
	return 0;
}

int flash_erase(const struct device *dev, off_t offset, size_t size)
{
	ARG_UNUSED(dev);
	flash_erases++;
	if (flash_erase_err) {
		return flash_erase_err;
	}
	fake_nvs_erase_range(offset, size);
	return 0;
}
