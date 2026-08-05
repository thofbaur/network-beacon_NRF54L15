#include "rtt_output.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SEGGER_RTT.h>
#include <zephyr/kernel.h>

#define RTT_OUTPUT_MESSAGE_GAP K_MSEC(5)

int rtt_output_init(void)
{
	SEGGER_RTT_Init();
	SEGGER_RTT_ConfigUpBuffer(0, "Terminal", NULL, 0,
				  SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);

	return 0;
}

void rtt_output_message(const char *message)
{
	bool ends_with_newline = false;

	for (const char *p = message; *p != '\0'; p++) {
		ends_with_newline = (*p == '\n');

		if (*p == '\n') {
			SEGGER_RTT_PutChar(0, '\r');
		}

		SEGGER_RTT_PutChar(0, *p);
	}

	if (!ends_with_newline) {
		SEGGER_RTT_WriteString(0, "\r\n");
	}

	k_sleep(RTT_OUTPUT_MESSAGE_GAP);
}

void rtt_output_data(const uint8_t *data, size_t len)
{
	SEGGER_RTT_Write(0, data, len);
	k_sleep(RTT_OUTPUT_MESSAGE_GAP);
}
