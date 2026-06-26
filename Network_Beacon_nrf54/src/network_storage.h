#ifndef NETWORK_STORAGE_H
#define NETWORK_STORAGE_H

#include <stdint.h>

#define NETWORK_STORAGE_CONTACT_SIZE 5
#define NETWORK_STORAGE_TOTAL_BYTES 0xc7000 /* TODO: tune fixed NVM contact storage reservation. */
#define NETWORK_STORAGE_BLOCK_BYTES 2048 /* TODO: tune NVM contact block size. */
#define NETWORK_STORAGE_BLOCK_OVERHEAD 12
#define NETWORK_STORAGE_BLOCK_DATA_LEN \
	(((NETWORK_STORAGE_BLOCK_BYTES - NETWORK_STORAGE_BLOCK_OVERHEAD) / \
	  NETWORK_STORAGE_CONTACT_SIZE) * NETWORK_STORAGE_CONTACT_SIZE)
#define NETWORK_STORAGE_BLOCK_CONTACTS \
	(NETWORK_STORAGE_BLOCK_DATA_LEN / NETWORK_STORAGE_CONTACT_SIZE)
#define NETWORK_STORAGE_BLOCK_COUNT \
	(NETWORK_STORAGE_TOTAL_BYTES / NETWORK_STORAGE_BLOCK_BYTES)

int network_storage_init(void);
int network_storage_append_block(const uint8_t *data, uint16_t len);
uint16_t network_storage_peek(uint8_t *buffer, uint16_t buffer_len);
uint16_t network_storage_drop(uint16_t bytes_to_drop);
uint16_t network_storage_pending_bytes(void);

#endif /* NETWORK_STORAGE_H */
