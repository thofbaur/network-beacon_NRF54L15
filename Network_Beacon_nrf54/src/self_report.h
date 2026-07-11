#ifndef SELF_REPORT_H
#define SELF_REPORT_H

#include <stdint.h>

#define SELF_REPORT_ENTRY_SIZE 3U

void self_report_init(void);
uint16_t self_report_peek(uint16_t entry_offset, uint8_t *buffer,
			  uint16_t buffer_len);
uint16_t self_report_get_count(void);

#endif /* SELF_REPORT_H */
