/* Recording fake of Network_Base_nrf54/src/output.c. */
#ifndef FAKE_OUTPUT_H
#define FAKE_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#define FAKE_OUTPUT_MAX_LINES 2048
#define FAKE_OUTPUT_LINE_LEN 192

extern char fake_output_lines[FAKE_OUTPUT_MAX_LINES][FAKE_OUTPUT_LINE_LEN];
extern int fake_output_line_count;

void fake_output_reset(void);

#endif
