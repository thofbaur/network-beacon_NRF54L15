#ifndef LED_H_
#define LED_H_

#include <stdbool.h>

int led_init(void);
void led_set_running(bool active);
void led_set_scanning(bool active);
void led_set_connected(bool active);

#endif /* LED_H_ */
