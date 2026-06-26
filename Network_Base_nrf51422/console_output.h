#ifndef CONSOLE_OUTPUT_H_
#define CONSOLE_OUTPUT_H_

#include <stdbool.h>
#include <stdint.h>

void console_output_init(void);
void console_output_packet(uint8_t current_id, const uint8_t *p_data, uint8_t data_len);
void console_output_process(void);
bool console_output_busy(void);

#endif /* CONSOLE_OUTPUT_H_ */
