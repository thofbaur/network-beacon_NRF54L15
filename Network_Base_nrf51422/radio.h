#ifndef RADIO_H_
#define RADIO_H_

#include <stdbool.h>
#include <stdint.h>
#include "ble.h"

void radio_init(void);
void radio_scan_start(void);
void radio_connecting_set(bool enabled);
void radio_disconnect_current(void);
void radio_on_ble_evt(ble_evt_t *p_ble_evt);
uint8_t radio_current_id(void);
bool radio_is_connected(void);
bool radio_is_scanning(void);
bool radio_connecting_enabled(void);

#endif /* RADIO_H_ */
