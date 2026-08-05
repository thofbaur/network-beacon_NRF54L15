#ifndef BATTERY_VOLTAGE_H
#define BATTERY_VOLTAGE_H

#include <stdint.h>

int battery_voltage_init(void);
int battery_voltage_read_mv(uint16_t *voltage_mv);
uint8_t battery_voltage_status_from_mv(uint16_t voltage_mv);

#endif /* BATTERY_VOLTAGE_H */
