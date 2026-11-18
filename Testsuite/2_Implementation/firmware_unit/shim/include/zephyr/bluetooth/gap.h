#ifndef SHIM_ZEPHYR_BLUETOOTH_GAP_H
#define SHIM_ZEPHYR_BLUETOOTH_GAP_H
#include <zephyr/sys/util.h>
/* Same conversions as zephyr/bluetooth/gap.h (0.625 ms units, rounded down). */
#define BT_GAP_US_TO_ADV_INTERVAL(_interval) ((uint16_t)((_interval) / 625U))
#define BT_GAP_MS_TO_ADV_INTERVAL(_interval) \
	(BT_GAP_US_TO_ADV_INTERVAL((_interval) * USEC_PER_MSEC))
#define BT_GAP_US_TO_SCAN_INTERVAL(_interval) ((uint16_t)((_interval) / 625U))
#define BT_GAP_MS_TO_SCAN_INTERVAL(_interval) \
	(BT_GAP_US_TO_SCAN_INTERVAL((_interval) * USEC_PER_MSEC))
#define BT_GAP_US_TO_SCAN_WINDOW(_window) ((uint16_t)((_window) / 625U))
#define BT_GAP_MS_TO_SCAN_WINDOW(_window) \
	(BT_GAP_US_TO_SCAN_WINDOW((_window) * USEC_PER_MSEC))
#endif
