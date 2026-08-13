#include <dk_buttons_and_leds.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "led.h"
#include "nus.h"
#include "output.h"
#include "radio.h"

LOG_MODULE_REGISTER(network_base, LOG_LEVEL_INF);

/* radio_start_scanning_with_level() takes the raw readout-level nibble
 * (0-7, per should_connect_to_adv()) advertised by a beacon, not the
 * shared DATA_LEVEL_* contact-count thresholds beacons use internally to
 * derive that nibble.
 */
static void button_handler(uint32_t button_state, uint32_t has_changed)
{
	if ((has_changed & DK_BTN1_MSK) && (button_state & DK_BTN1_MSK)) {
		LOG_INF("Button0 pressed: start scanning at data level 3");
		(void)radio_start_scanning_with_level(3);
	}

	if ((has_changed & DK_BTN2_MSK) && (button_state & DK_BTN2_MSK)) {
		LOG_INF("Button1 pressed: wait for finished, then stop");
		radio_request_stop_after_finished();
	}

	if ((has_changed & DK_BTN3_MSK) && (button_state & DK_BTN3_MSK)) {
		LOG_INF("Button2 pressed: start scanning at data level 2");
		(void)radio_start_scanning_with_level(2);
	}

	if ((has_changed & DK_BTN4_MSK) && (button_state & DK_BTN4_MSK)) {
		LOG_INF("Button3 pressed: start scanning at data level 1");
		(void)radio_start_scanning_with_level(1);
	}
}

static void on_radio_connected(struct bt_conn *conn, uint8_t beacon_id,
			       uint8_t status)
{
	nus_on_connected(conn, beacon_id, status);
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

	err = output_init();
	if (err) {
		LOG_ERR("Output init failed (err %d)", err);
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

	printk("Network base ready. Press button0 to connect at data level 3. Press button1 to stop connecting. Press button2 to connect at data level 2. Press button3 to connect at data level 1\n");

	for (;;) {
		k_sleep(K_FOREVER);
	}
}
