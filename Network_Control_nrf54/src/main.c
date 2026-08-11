/* main.c - Beacon control application
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Broadcasts one hardcoded command packet using the beacon command
 * protocol implemented by Network_Beacon_nrf54/src/radio.c (see
 * BeaconNRF54/shared/common_include.h for the authoritative parameter
 * list). A beacon recognizes a command advertisement by its device name
 * ("DSZ") and reads target/parameter/value from the manufacturer data
 * field, both of which must be in the same advertising PDU - not split
 * into a scan response - since a beacon parses each advertising report
 * independently.
 *
 * Button 3 starts advertising the command; Button 4 stops it.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>

/* Beacon command-protocol constants. Must match
 * BeaconNRF54/shared/common_include.h - the beacon firmware's own copy of
 * this protocol definition.
 */
#define COMMAND_TARGET_BROADCAST		0xFF

#define P_BASE_MAIN				0x20
#define P_MAIN_LED_ACTIVE			(P_BASE_MAIN + 1)
#define P_MAIN_LED_INTERVAL_S			(P_BASE_MAIN + 2)
#define P_MAIN_RESET_PARAMS			(P_BASE_MAIN + 12)

#define P_BASE_NETWORK				0x60
#define P_RSSI_NETWORK				(P_BASE_NETWORK + 4)
#define P_RSSI_LOCATION				(P_BASE_NETWORK + 5)
#define P_NETWORK_RESET_PARAMS			(P_BASE_NETWORK + 12)
#define P_TRACKING_ACTIVE			(P_BASE_NETWORK + 13)

#define P_BASE_RADIO				0x80
#define P_ADV_INTERVAL_MS			(P_BASE_RADIO + 1)
#define P_ADV_INTERVAL_ECO_MS			(P_BASE_RADIO + 2)
#define P_SCAN_INTERVAL_MS			(P_BASE_RADIO + 3)
#define P_SCAN_WINDOW_MS			(P_BASE_RADIO + 5)
#define P_ECO_SCAN_WINDOW_MS			(P_BASE_RADIO + 6)
#define P_ECO_SCAN_PERIOD_S			(P_BASE_RADIO + 7)
#define P_RADIO_RESET_PARAMS			(P_BASE_RADIO + 12)
#define P_SET_RAD_ACTIVE			(P_BASE_RADIO + 13)

#define P_BASE_MOTION				0xA0
#define P_MOTION_ACTIVE				(P_BASE_MOTION + 1)
#define P_MOTION_INACTIVITY_TIMEOUT_S		(P_BASE_MOTION + 2)
#define P_MOTION_RESET_PARAMS			(P_BASE_MOTION + 12)

/* Splits a 16-bit command value into the big-endian byte pair the beacon
 * expects (it decodes each value with sys_get_be16).
 */
#define HI(v)	(uint8_t)(((uint16_t)(v) >> 8) & 0xFF)
#define LO(v)	(uint8_t)((uint16_t)(v) & 0xFF)

#define DEVICE_NAME	"DSZ"

/* nRF54L15 radio TX power tops out at +8 dBm (RADIO_TXPOWER_TXPOWER_Pos8dBm). */
#define TX_POWER_MAX_DBM	8

/* Rapid blink while a command is being advertised, so it's visible that the
 * button press took effect.
 */
#define ADV_LED_BLINK_MS	100

/* Beacon id to command, or COMMAND_TARGET_BROADCAST to command every beacon
 * in range.
 */
#define CMD_TARGET	0xFF

/* Commands to advertise. Every beacon parameter is listed below, commented
 * out, with a placeholder value. Uncomment a line - and edit its value -
 * to include that command; multiple lines may be active at once, since the
 * beacon applies every (parameter, value) triple found in the packet.
 */
static const uint8_t mfg_data[] = {
	CMD_TARGET,

	/* Main */
	//   P_MAIN_LED_ACTIVE,		HI(1),	LO(1),		/* 1 = on, 0 = off */
	// P_MAIN_LED_INTERVAL_S,	HI(5),	LO(5),		/* blink interval, seconds */
	// P_MAIN_RESET_PARAMS,		HI(0),	LO(0),

	/* Network */
	 P_RSSI_NETWORK,		HI(100),	LO(100),		/* contact RSSI threshold */
	// P_RSSI_LOCATION,		HI(60),	LO(60),		/* location RSSI threshold */
	// P_NETWORK_RESET_PARAMS,	HI(0),	LO(0),
	 //P_TRACKING_ACTIVE,		HI(1),	LO(1),		/* 1 = on, 0 = off */

	/* Radio */
	// P_ADV_INTERVAL_MS,		HI(1000), LO(1000),
	// P_ADV_INTERVAL_ECO_MS,	HI(5000), LO(5000),
	// P_SCAN_INTERVAL_MS,		HI(1000), LO(1000),
	// P_SCAN_WINDOW_MS,		HI(300), LO(300),
	// P_ECO_SCAN_WINDOW_MS,	HI(200), LO(200),
	// P_ECO_SCAN_PERIOD_S,		HI(30),	LO(30),
	// P_RADIO_RESET_PARAMS,	HI(0),	LO(0),
	// P_SET_RAD_ACTIVE,		HI(1),	LO(1),		/* 1 = high activity, 0 = eco */

	/* Motion */
	// P_MOTION_ACTIVE,		HI(1),	LO(1),		/* 1 = on, 0 = off */
	// P_MOTION_INACTIVITY_TIMEOUT_S, HI(60), LO(60),
	// P_MOTION_RESET_PARAMS,	HI(0),	LO(0),
};

static const struct bt_data ad[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, sizeof(DEVICE_NAME) - 1),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
};

/* Button 3 starts advertising, Button 4 stops it. */
static const struct gpio_dt_spec button_start = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static const struct gpio_dt_spec button_stop = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios);

/* Blinks rapidly while a command is advertising. */
static const struct gpio_dt_spec adv_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static struct gpio_callback button_start_cb;
static struct gpio_callback button_stop_cb;

static void adv_led_blink_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_led_blink_work, adv_led_blink_handler);

static void adv_led_blink_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	gpio_pin_toggle_dt(&adv_led);
	k_work_reschedule(&adv_led_blink_work, K_MSEC(ADV_LED_BLINK_MS));
}

/* Sets the controller's advertising TX power. handle is always 0 for legacy
 * (non-extended) advertising, which only ever has one advertising set.
 */
static void set_adv_tx_power(int8_t tx_power_dbm)
{
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf, *rsp = NULL;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		printk("Unable to allocate command buffer\n");
		return;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle_type = BT_HCI_VS_LL_HANDLE_TYPE_ADV;
	cp->handle = sys_cpu_to_le16(0);
	cp->tx_power_level = tx_power_dbm;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err) {
		printk("Set TX power failed (err %d)\n", err);
		return;
	}

	rp = (void *)rsp->data;
	printk("TX power set to %d dBm\n", rp->selected_tx_power);
	net_buf_unref(rsp);
}

static void adv_start_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad), NULL, 0);

	if (err && err != -EALREADY) {
		printk("Advertising failed to start (err %d)\n", err);
		return;
	}
	printk("Advertising started\n");
	k_work_reschedule(&adv_led_blink_work, K_NO_WAIT);
}

static void adv_stop_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = bt_le_adv_stop();

	if (err) {
		printk("Advertising failed to stop (err %d)\n", err);
		return;
	}
	printk("Advertising stopped\n");
	k_work_cancel_delayable(&adv_led_blink_work);
	gpio_pin_set_dt(&adv_led, 0);
}

static K_WORK_DEFINE(adv_start_work, adv_start_work_handler);
static K_WORK_DEFINE(adv_stop_work, adv_stop_work_handler);

static void button_start_pressed(const struct device *dev, struct gpio_callback *cb,
				  uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&adv_start_work);
}

static void button_stop_pressed(const struct device *dev, struct gpio_callback *cb,
				 uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&adv_stop_work);
}

static int setup_button(const struct gpio_dt_spec *button, struct gpio_callback *cb,
			 gpio_callback_handler_t handler)
{
	int err;

	if (!gpio_is_ready_dt(button)) {
		printk("Button device not ready\n");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(button, GPIO_INPUT);
	if (err) {
		return err;
	}

	err = gpio_pin_interrupt_configure_dt(button, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		return err;
	}

	gpio_init_callback(cb, handler, BIT(button->pin));
	return gpio_add_callback(button->port, cb);
}

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");
	set_adv_tx_power(TX_POWER_MAX_DBM);
}

int main(void)
{
	int err;

	printk("Starting Beacon Control\n");

	if (!gpio_is_ready_dt(&adv_led)) {
		printk("Advertising LED device not ready\n");
	} else if (gpio_pin_configure_dt(&adv_led, GPIO_OUTPUT_INACTIVE)) {
		printk("Advertising LED configure failed\n");
	}

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	err = setup_button(&button_start, &button_start_cb, button_start_pressed);
	if (err) {
		printk("Failed to set up start button (err %d)\n", err);
		return 0;
	}

	err = setup_button(&button_stop, &button_stop_cb, button_stop_pressed);
	if (err) {
		printk("Failed to set up stop button (err %d)\n", err);
		return 0;
	}

	printk("Press Button 3 to start advertising, Button 4 to stop\n");
	return 0;
}
