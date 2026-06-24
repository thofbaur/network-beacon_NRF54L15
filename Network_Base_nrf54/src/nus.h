#ifndef NUS_H_
#define NUS_H_

#include <zephyr/bluetooth/conn.h>
#include <stdint.h>

typedef void (*nus_finished_cb_t)(void);

int nus_init(nus_finished_cb_t finished_cb);
void nus_on_connected(struct bt_conn *conn, uint8_t beacon_id);
void nus_on_disconnected(void);

#endif /* NUS_H_ */
