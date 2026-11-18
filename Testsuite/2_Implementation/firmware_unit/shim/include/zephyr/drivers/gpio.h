#ifndef SHIM_ZEPHYR_DRIVERS_GPIO_H
#define SHIM_ZEPHYR_DRIVERS_GPIO_H

#include "shim_common.h"
#include <zephyr/device.h>
#include <zephyr/sys/util.h>

typedef uint32_t gpio_port_pins_t;
typedef uint8_t gpio_pin_t;
typedef uint32_t gpio_flags_t;

#define GPIO_INPUT BIT(16)
#define GPIO_OUTPUT BIT(17)
#define GPIO_OUTPUT_INACTIVE (GPIO_OUTPUT | BIT(18))
#define GPIO_INT_EDGE_BOTH BIT(20)
#define GPIO_INT_EDGE_TO_ACTIVE BIT(21)

struct gpio_dt_spec {
	const struct device *port;
	gpio_pin_t pin;
	gpio_flags_t dt_flags;
};

#define GPIO_DT_SPEC_GET(node, prop) \
	{ .port = &shim_generic_device, .pin = Z_SHIM_DT_PIN(node), .dt_flags = 0 }

struct gpio_callback;
typedef void (*gpio_callback_handler_t)(const struct device *port,
					struct gpio_callback *cb,
					gpio_port_pins_t pins);

struct gpio_callback {
	gpio_callback_handler_t handler;
	gpio_port_pins_t pin_mask;
	struct gpio_callback *next;
};

static inline void gpio_init_callback(struct gpio_callback *callback,
				      gpio_callback_handler_t handler,
				      gpio_port_pins_t pin_mask)
{
	callback->handler = handler;
	callback->pin_mask = pin_mask;
	callback->next = NULL;
}

int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, gpio_flags_t flags);
int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value);
int gpio_pin_get_dt(const struct gpio_dt_spec *spec);
int gpio_pin_toggle_dt(const struct gpio_dt_spec *spec);
int gpio_add_callback(const struct device *port, struct gpio_callback *callback);
int gpio_remove_callback(const struct device *port,
			 struct gpio_callback *callback);
int gpio_pin_interrupt_configure_dt(const struct gpio_dt_spec *spec,
				    gpio_flags_t flags);

static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
	return device_is_ready(spec->port);
}

/* ---- test control ------------------------------------------------------ */
#define SHIM_GPIO_PINS 64
void shim_gpio_reset(void);
int shim_gpio_output(unsigned int pin);	/* last value set, -1 never set */
int shim_gpio_set_count(unsigned int pin);
void shim_gpio_set_input(unsigned int pin, int value);
void shim_gpio_fire(unsigned int pin);	/* invoke matching callbacks */
void shim_gpio_set_configure_error(unsigned int pin, int err);
void shim_gpio_set_get_error(unsigned int pin, int err);

#endif /* SHIM_ZEPHYR_DRIVERS_GPIO_H */
