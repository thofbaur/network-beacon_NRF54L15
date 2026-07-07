#include <dk_buttons_and_leds.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "led.h"
#include "nus.h"
#include "radio.h"

LOG_MODULE_REGISTER(network_base, LOG_LEVEL_INF);

static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	if ((has_changed & DK_BTN1_MSK) && (button_state & DK_BTN1_MSK)) {
		LOG_INF("Button0 pressed: start scanning");
		(void)radio_start_scanning();
	}

	if ((has_changed & DK_BTN2_MSK) && (button_state & DK_BTN2_MSK)) {
		LOG_INF("Button1 pressed: wait for finished, then stop");
		radio_request_stop_after_finished();
	}
}

static void on_radio_connected(struct bt_conn *conn, uint8_t beacon_id)
{
	nus_on_connected(conn, beacon_id);
}

static void on_radio_disconnected(void)
{
	nus_on_disconnected();
}

static void on_nus_finished(void)
{
	radio_transfer_finished();
}

int main(void)
{
	int err;
	const struct radio_callbacks callbacks = {
		.connected = on_radio_connected,
		.disconnected = on_radio_disconnected,
	};

	err = led_init();
	if (err) {
		return 0;
	}

	led_set_running(true);

	err = dk_buttons_init(button_handler);
	if (err) {
		LOG_ERR("Button init failed (err %d)", err);
		return 0;
	}

	err = radio_init(&callbacks);
	if (err) {
		return 0;
	}

	err = nus_init(on_nus_finished);
	if (err) {
		return 0;
	}

	printk("Network base ready. Press button0 to Start connecting. Press button1 to stop connecting\n");

	for (;;) {
		k_sleep(K_FOREVER);
	}
}
