#ifndef SHIM_ZEPHYR_DRIVERS_SENSOR_H
#define SHIM_ZEPHYR_DRIVERS_SENSOR_H
#include <zephyr/device.h>
struct sensor_trigger {
	int type;
	int chan;
};
#endif
