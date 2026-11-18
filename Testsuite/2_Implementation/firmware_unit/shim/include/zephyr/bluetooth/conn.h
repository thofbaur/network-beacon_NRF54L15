#ifndef SHIM_ZEPHYR_BLUETOOTH_CONN_H
#define SHIM_ZEPHYR_BLUETOOTH_CONN_H
#include "shim_common.h"

struct bt_conn;

struct bt_conn_cb {
	void (*connected)(struct bt_conn *conn, uint8_t err);
	void (*disconnected)(struct bt_conn *conn, uint8_t reason);
	void (*recycled)(void);
};

/* Real Zephyr places these in an iterable section; the host shim keeps them
 * as unused statics (connection handling is covered by hardware tests).
 */
#define BT_CONN_CB_DEFINE(_name) \
	static const struct bt_conn_cb _name __maybe_unused
#endif
