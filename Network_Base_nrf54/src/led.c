#include "led.h"

#include <dk_buttons_and_leds.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(network_base);

int led_init(void)
{
	int err = dk_leds_init();

	if (err) {
		LOG_ERR("LED init failed (err %d)", err);
		return err;
	}

	(void)dk_set_leds(DK_NO_LEDS_MSK);
	return 0;
}

void led_set_running(bool active)
{
	(void)dk_set_led(DK_LED1, active ? 1 : 0);
}

void led_set_scanning(bool active)
{
	(void)dk_set_led(DK_LED2, active ? 1 : 0);
}

void led_set_connected(bool active)
{
	(void)dk_set_led(DK_LED3, active ? 1 : 0);
}
