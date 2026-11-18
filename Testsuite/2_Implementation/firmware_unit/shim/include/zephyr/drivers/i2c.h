#ifndef SHIM_ZEPHYR_DRIVERS_I2C_H
#define SHIM_ZEPHYR_DRIVERS_I2C_H
#include <zephyr/device.h>
/* motion.c only uses I2C when a motion sensor alias exists; the host board
 * has none, so no I2C functions are needed.
 */
#endif
