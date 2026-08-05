#ifndef OUTPUT_H_
#define OUTPUT_H_

#include <stddef.h>
#include <stdint.h>

int output_init(void);
void output_data(const uint8_t *data, size_t len);
void output_message(const char *message);
void output_messagef(const char *fmt, ...);

#endif /* OUTPUT_H_ */
