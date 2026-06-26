#ifndef NUS_H_
#define NUS_H_

#include <stdint.h>
#include "ble.h"

void nus_init(void);
void nus_start_discovery(uint16_t conn_handle);
void nus_on_ble_evt(ble_evt_t *p_ble_evt);

#endif /* NUS_H_ */
