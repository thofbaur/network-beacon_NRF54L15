#ifndef SHIM_ZEPHYR_BLUETOOTH_ADDR_H
#define SHIM_ZEPHYR_BLUETOOTH_ADDR_H
#include "shim_common.h"

#define BT_ADDR_LE_PUBLIC 0x00
#define BT_ADDR_LE_RANDOM 0x01
#define BT_ADDR_LE_STR_LEN 30

typedef struct {
	uint8_t val[6];
} bt_addr_t;

typedef struct {
	uint8_t type;
	bt_addr_t a;
} bt_addr_le_t;

static inline int bt_addr_le_cmp(const bt_addr_le_t *a, const bt_addr_le_t *b)
{
	return memcmp(a, b, sizeof(*a));
}
#endif
