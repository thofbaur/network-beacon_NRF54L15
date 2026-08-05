#ifndef RTT_OUTPUT_H_
#define RTT_OUTPUT_H_

#include <stddef.h>
#include <stdint.h>

int rtt_output_init(void);
void rtt_output_data(const uint8_t *data, size_t len);
void rtt_output_message(const char *message);

#endif /* RTT_OUTPUT_H_ */
