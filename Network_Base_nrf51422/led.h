#ifndef LED_H_
#define LED_H_

#include <stdbool.h>

void led_init(void);
void led_set_connection_available(bool on);
void led_set_connected(bool on);

#endif /* LED_H_ */
