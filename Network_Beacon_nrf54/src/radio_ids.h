#ifndef RADIO_IDS_H
#define RADIO_IDS_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/bluetooth.h>

struct known_device {
    bt_addr_le_t addr;
    uint8_t id;
};

extern const struct known_device known_device_table[];
extern const size_t known_device_table_len;

#endif /* RADIO_IDS_H */
