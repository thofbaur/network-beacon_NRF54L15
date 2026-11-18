#ifndef SHIM_ZEPHYR_DEVICE_H
#define SHIM_ZEPHYR_DEVICE_H

#include "shim_common.h"
#include <zephyr/devicetree.h>
/* Like Zephyr, the device/driver headers make the kernel API visible. */
#include <zephyr/kernel.h>

struct device_state {
	bool initialized;
};

struct device {
	const char *name;
	bool ready;
	struct device_state *state;
};

static inline bool device_is_ready(const struct device *dev)
{
	return dev != NULL && dev->ready;
}

/* Every devicetree device that is not a flash partition (GPIO port, ADC,
 * I2C bus) resolves to one generic, toggleable host device.
 */
extern struct device shim_generic_device;
extern struct device shim_flash_device;

#define DEVICE_DT_GET(node) (&shim_generic_device)

#endif /* SHIM_ZEPHYR_DEVICE_H */
