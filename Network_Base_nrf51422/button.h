#ifndef BUTTON_H_
#define BUTTON_H_

#include "ble.h"

void button_init(void);
void button_on_ble_evt(ble_evt_t *p_ble_evt);

#endif /* BUTTON_H_ */
