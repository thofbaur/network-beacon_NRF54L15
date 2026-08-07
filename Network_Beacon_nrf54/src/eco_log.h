#ifndef ECO_LOG_H
#define ECO_LOG_H

#include <stdint.h>

#define ECO_LOG_ENTRY_SIZE 6U

int eco_log_init(void);
void eco_log_enter(void);
void eco_log_leave(void);
int eco_log_export_begin(uint8_t *buffer, uint16_t buffer_len,
			 uint16_t *bytes_written);
int eco_log_export_commit(void);
void eco_log_export_abort(void);
int eco_log_sync_storage(void);
int eco_log_get_count(uint16_t *count);

#endif /* ECO_LOG_H */
