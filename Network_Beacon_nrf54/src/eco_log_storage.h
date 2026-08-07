#ifndef ECO_LOG_STORAGE_H
#define ECO_LOG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#define ECO_LOG_STORAGE_BLOCK_ENTRIES 16U

int eco_log_storage_init(void);
int eco_log_storage_append(const uint8_t *entries, uint16_t entry_count);
int eco_log_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			 uint16_t *bytes_written);
int eco_log_storage_drop(uint16_t entry_count, bool *block_retired);
int eco_log_storage_sync(void);
int eco_log_storage_get_count(uint32_t *count);

#endif /* ECO_LOG_STORAGE_H */
