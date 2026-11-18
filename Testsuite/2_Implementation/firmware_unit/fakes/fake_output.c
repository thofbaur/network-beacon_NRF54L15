#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "fake_output.h"
#include "output.h"

char fake_output_lines[FAKE_OUTPUT_MAX_LINES][FAKE_OUTPUT_LINE_LEN];
int fake_output_line_count;

void fake_output_reset(void)
{
	fake_output_line_count = 0;
}

int output_init(void)
{
	return 0;
}

void output_data(const uint8_t *data, size_t len)
{
	(void)data;
	(void)len;
}

void output_message(const char *message)
{
	if (fake_output_line_count < FAKE_OUTPUT_MAX_LINES) {
		/* Same 160-byte limit as output.c's OUTPUT_MESSAGE_MAX. */
		snprintf(fake_output_lines[fake_output_line_count],
			 FAKE_OUTPUT_LINE_LEN, "%.159s", message);
		fake_output_line_count++;
	}
}

void output_messagef(const char *fmt, ...)
{
	char message[160];
	va_list args;

	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);
	output_message(message);
}

bool output_queue_is_drained(void)
{
	return true;
}
