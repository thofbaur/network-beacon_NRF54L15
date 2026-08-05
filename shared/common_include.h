#ifndef COMMON_INCLUDES_H_
#define COMMON_INCLUDES_H_

/* NUS packet type flags. */
#define DSA_NUS_FLAG_TIME			0x01
#define DSA_NUS_FLAG_DATA			0x02
#define DSA_NUS_FLAG_VOLTAGE			0x03
#define DSA_NUS_FLAG_CONTROL			0x04
#define DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE	0x05
#define DSA_NUS_FLAG_SELF_REPORT		0x06

/* Manufacturer-data byte positions. */
#define ADV_POS_ID 0
#define ADV_POS_RADIO_STATUS 1
#define ADV_POS_NETWORK_STATUS 2

/* Radio and advertising status bits. */
#define RADIO_STATUS_SCAN_RUNTIME_ERROR	BIT(0)
#define RADIO_STATUS_NUS_ERROR		BIT(1)
#define RADIO_STATUS_SCAN_CONFIG_ERROR	BIT(2)

#define BLE_UPDATE_ADV_ERROR		BIT(0)
#define BLE_UPDATE_SCAN_ERROR		BIT(1)
#define BLE_UPDATE_STATUS_ERROR		BIT(2)

/* Network data-level encoding. Threshold values remain provisional. */
#define READOUT_LEVEL	0

#define DATA_LEVEL_1	0
#define DATA_LEVEL_2	1
#define DATA_LEVEL_3	4
#define DATA_LEVEL_4	16
#define DATA_LEVEL_5	32
#define DATA_LEVEL_6	64
#define DATA_LEVEL_7	256

#define DATA_LEVEL_MASK		0x0F
#define P_SHIFT_STATUS_DATA	0

#define BATTERY_LEVEL_MASK	0xF0
#define P_SHIFT_STATUS_BATTERY	4

#define BATTERY_LEVEL_1_THRESHOLD_MV	3000
#define BATTERY_LEVEL_2_THRESHOLD_MV	2800
#define BATTERY_LEVEL_3_THRESHOLD_MV	2600

/* Runtime command parameter encoding. */
#define P_NULL				0
#define P_BASE_MASK			0xE0
#define P_BASE_MAIN			0x20
#define P_BASE_NETWORK		0x60
#define P_BASE_RADIO		0x80

/* Main parameters. */
#define P_MAIN_LED_ACTIVE		(P_BASE_MAIN + 1)
#define P_MAIN_LED_INTERVAL_S		(P_BASE_MAIN + 2)
#define P_MAIN_RESET_PARAMS		(P_BASE_MAIN + 12)

/* Network parameters. */
#define P_RSSI_NETWORK			(P_BASE_NETWORK + 4)
#define P_NETWORK_RESET_PARAMS		(P_BASE_NETWORK + 12)
#define P_TRACKING_ACTIVE		(P_BASE_NETWORK + 13)

/* Radio parameters. */
#define P_ADV_INTERVAL_MS			(P_BASE_RADIO + 1)
#define P_ADV_INTERVAL_LOWACTIVITY_MS	(P_BASE_RADIO + 2)
#define P_SCAN_INTERVAL_MS			(P_BASE_RADIO + 3)
#define P_SCAN_INTERVAL_LOWACTIVITY_MS	(P_BASE_RADIO + 4)
#define P_SCAN_WINDOW_MS			(P_BASE_RADIO + 5)
#define P_SCAN_WINDOW_LOWACTIVITY_MS	(P_BASE_RADIO + 6)
#define P_RADIO_RESET_PARAMS			(P_BASE_RADIO + 12)
#define P_SET_RAD_ACTIVE			(P_BASE_RADIO + 13)

#endif /* COMMON_INCLUDES_H_ */
