#ifndef APP_H_
#define APP_H_

#include "ble.h"

void app_init(void);
void app_run(void);
void app_ble_evt_dispatch(ble_evt_t *p_ble_evt);
void app_transfer_finished(void);

#endif /* APP_H_ */
