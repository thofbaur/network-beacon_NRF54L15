#ifndef OUTPUT_H_
#define OUTPUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int output_init(void);
void output_data(const uint8_t *data, size_t len);
void output_message(const char *message);
void output_messagef(const char *fmt, ...);

/* True once the output queue has fully drained (everything handed to
 * output_data()/output_message() so far has been written out). Used to hold
 * off connecting to the next beacon until the previous one's backlog is no
 * longer competing for the same limited queue slots - see radio.c's
 * scan_recv().
 */
bool output_queue_is_drained(void);

#endif /* OUTPUT_H_ */
