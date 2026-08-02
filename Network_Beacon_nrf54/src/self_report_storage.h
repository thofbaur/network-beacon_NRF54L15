#ifndef SELF_REPORT_STORAGE_H
#define SELF_REPORT_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#define SELF_REPORT_STORAGE_BLOCK_ENTRIES 64U

int self_report_storage_init(void);
int self_report_storage_append(const uint8_t *reports, uint16_t report_count);
int self_report_storage_peek(uint8_t *buffer, uint16_t buffer_len,
			     uint16_t *bytes_written);
int self_report_storage_drop(uint16_t report_count, bool *block_retired);
int self_report_storage_get_count(uint32_t *count);

#endif /* SELF_REPORT_STORAGE_H */
