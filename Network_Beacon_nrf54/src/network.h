#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>

#include <zephyr/bluetooth/addr.h>

struct net_buf_simple;

void network_evaluate_contact(const bt_addr_le_t *addr, 
    int8_t rssi, uint8_t adv_type, struct net_buf_simple *buf);
void network_init(void);
void network_apply_command(uint8_t parameter, uint16_t value);
int network_params_load(void);
int network_params_save(void);
uint16_t network_peek_contact(uint8_t *buffer, uint16_t buffer_len);
void network_drop_contact_bytes(uint16_t bytes_to_drop);
uint16_t network_read_contact(uint8_t *buffer, uint16_t buffer_len);
/* TODO Remove: development-only contact buffer filler hooks. */
void network_dev_append_contact(uint8_t id, uint32_t uptime_s, uint8_t rssi);
void network_dev_fill_random_contacts(uint16_t count);
uint16_t network_get_contact_count();

#endif /* NETWORK_H */
