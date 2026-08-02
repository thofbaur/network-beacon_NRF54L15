#ifndef SELF_REPORT_H
#define SELF_REPORT_H

#include <stdint.h>

#define SELF_REPORT_ENTRY_SIZE 3U

int self_report_init(void);
int self_report_export_begin(uint8_t *buffer, uint16_t buffer_len,
			     uint16_t *bytes_written);
int self_report_export_commit(void);
void self_report_export_abort(void);
int self_report_get_count(uint16_t *count);

#endif /* SELF_REPORT_H */
