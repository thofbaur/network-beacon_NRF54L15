#ifndef UART_OUTPUT_H_
#define UART_OUTPUT_H_

#include <stddef.h>
#include <stdint.h>

int uart_output_init(void);
void uart_output_data(const uint8_t *data, size_t len);
void uart_output_message(const char *message);

#endif /* UART_OUTPUT_H_ */
