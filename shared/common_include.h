#ifndef COMMON_INCLUDES_H_
#define COMMON_INCLUDES_H_

/* NUS packet type flags. */
#define DSA_NUS_FLAG_TIME			0x01
#define DSA_NUS_FLAG_DATA			0x02
#define DSA_NUS_FLAG_VOLTAGE			0x03
#define DSA_NUS_FLAG_CONTROL			0x04
#define DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE	0x05
#define DSA_NUS_FLAG_SELF_REPORT		0x06
#define DSA_NUS_FLAG_ECO_LOG			0x07

/* Synthesized by Network_Base_nrf54 itself on connect, from the advertised
 * fault/error status byte (ADV_POS_RADIO_STATUS) rather than a beacon-sent
 * NUS notification. Framed like a regular NUS message so the raw log parser
 * (dsa_logger.py) can decode it the same way.
 */
#define DSA_NUS_FLAG_CONNECT_STATUS		0x08

/* Manufacturer-data byte positions. */
#define ADV_POS_ID 0
#define ADV_POS_RADIO_STATUS 1
#define ADV_POS_NETWORK_STATUS 2

/* Radio and advertising status bits.
 * Bit 0: scanning isn't tracking contacts right now, whether because a
 *   runtime start/stop failed or the accept-list configuration failed at
 *   boot - both look the same to anyone who can only reach the tag over
 *   BLE, so they share one advertised bit (see radio.c's scan_runtime_fault
 *   / scan_config_fault).
 * Bit 1: NUS unavailable - the tag can never be read out over BLE.
 * Bit 5: motion sensor unavailable - inactivity detection can't run, so the
 *   tag is stuck in high-activity mode (see motion.c).
 * Bits 2-4 are STORAGE_STATUS_* (device.h); bits 6-7 are reserved.
 */
#define RADIO_STATUS_SCAN_ERROR		BIT(0)
#define RADIO_STATUS_NUS_ERROR		BIT(1)
#define RADIO_STATUS_MOTION_UNAVAILABLE	BIT(5)

#define BLE_UPDATE_ADV_ERROR		BIT(0)
#define BLE_UPDATE_SCAN_ERROR		BIT(1)
#define BLE_UPDATE_STATUS_ERROR		BIT(2)

/* Network data-level encoding.*/
#define DATA_LEVEL_1	0
#define DATA_LEVEL_2	1
#define DATA_LEVEL_3	5000
#define DATA_LEVEL_4	40000
#define DATA_LEVEL_5	100000
#define DATA_LEVEL_6	150000
#define DATA_LEVEL_7	210000

#define DATA_LEVEL_MASK		0x0F
#define P_SHIFT_STATUS_DATA	0

/* Battery status only ever uses values 0-3; the mask was narrowed from
 * 0xF0 to free bit 7 for ECO_MODE_MASK. See DECISIONS.md.
 */
#define BATTERY_LEVEL_MASK	0x70
#define P_SHIFT_STATUS_BATTERY	4

#define ECO_MODE_MASK		BIT(7)

#define BATTERY_LEVEL_1_THRESHOLD_MV	3000
#define BATTERY_LEVEL_2_THRESHOLD_MV	2800
#define BATTERY_LEVEL_3_THRESHOLD_MV	2600

/* Runtime command parameter encoding. */
#define P_NULL				0
#define P_BASE_MASK			0xE0
#define P_BASE_MAIN			0x20
#define P_BASE_NETWORK		0x60
#define P_BASE_RADIO		0x80
#define P_BASE_MOTION		0xA0

/* Main parameters. */
#define P_MAIN_LED_ACTIVE		(P_BASE_MAIN + 1)
#define P_MAIN_LED_INTERVAL_S		(P_BASE_MAIN + 2)
#define P_MAIN_RESET_PARAMS		(P_BASE_MAIN + 12)

/* Network parameters. */
#define P_RSSI_NETWORK			(P_BASE_NETWORK + 4)
#define P_RSSI_LOCATION			(P_BASE_NETWORK + 5)
#define P_NETWORK_RESET_PARAMS		(P_BASE_NETWORK + 12)
#define P_TRACKING_ACTIVE		(P_BASE_NETWORK + 13)

/* Radio parameters.
 * Low-activity mode was replaced by eco mode (see DECISIONS.md); +4
 * (formerly P_SCAN_INTERVAL_LOWACTIVITY_MS, a millisecond BLE scan
 * interval) is retired rather than reinterpreted, since eco's scan burst
 * period is a plain-seconds duration with no native BLE representation.
 * Do not reuse +4 for an unrelated parameter.
 */
#define P_ADV_INTERVAL_MS			(P_BASE_RADIO + 1)
#define P_ADV_INTERVAL_ECO_MS			(P_BASE_RADIO + 2)
#define P_SCAN_INTERVAL_MS			(P_BASE_RADIO + 3)
#define P_SCAN_WINDOW_MS			(P_BASE_RADIO + 5)
#define P_ECO_SCAN_WINDOW_MS			(P_BASE_RADIO + 6)
#define P_ECO_SCAN_PERIOD_S			(P_BASE_RADIO + 7)
#define P_RADIO_RESET_PARAMS			(P_BASE_RADIO + 12)
#define P_SET_RAD_ACTIVE			(P_BASE_RADIO + 13)

/* Motion / inactivity parameters. */
#define P_MOTION_ACTIVE				(P_BASE_MOTION + 1)
#define P_MOTION_INACTIVITY_TIMEOUT_S		(P_BASE_MOTION + 2)
#define P_MOTION_RESET_PARAMS			(P_BASE_MOTION + 12)

#endif /* COMMON_INCLUDES_H_ */
