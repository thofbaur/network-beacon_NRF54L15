#ifndef RADIO_H_
#define RADIO_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

typedef void (*radio_connected_cb_t)(struct bt_conn *conn, uint8_t beacon_id);
typedef void (*radio_disconnected_cb_t)(void);

struct radio_callbacks {
	radio_connected_cb_t connected;
	radio_disconnected_cb_t disconnected;
};

int radio_init(const struct radio_callbacks *callbacks);
int radio_start_scanning(void);
int radio_stop_scanning(void);
void radio_request_stop_after_finished(void);
void radio_transfer_finished(void);

#endif /* RADIO_H_ */
