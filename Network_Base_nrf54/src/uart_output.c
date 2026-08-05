#include "uart_output.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#define UART_OUTPUT_NODE DT_CHOSEN(zephyr_console)
#define UART_OUTPUT_MESSAGE_GAP K_MSEC(5)

static const struct device *const uart_output_dev =
	DEVICE_DT_GET(UART_OUTPUT_NODE);

K_MUTEX_DEFINE(uart_output_mutex);

int uart_output_init(void)
{
	if (!device_is_ready(uart_output_dev)) {
		return -ENODEV;
	}

	return 0;
}

void uart_output_message(const char *message)
{
	bool ends_with_newline = false;

	k_mutex_lock(&uart_output_mutex, K_FOREVER);

	for (const char *p = message; *p != '\0'; p++) {
		ends_with_newline = (*p == '\n');

		if (*p == '\n') {
			uart_poll_out(uart_output_dev, '\r');
		}

		uart_poll_out(uart_output_dev, *p);
	}

	if (!ends_with_newline) {
		uart_poll_out(uart_output_dev, '\r');
		uart_poll_out(uart_output_dev, '\n');
	}

	k_mutex_unlock(&uart_output_mutex);

	k_sleep(UART_OUTPUT_MESSAGE_GAP);
}

void uart_output_data(const uint8_t *data, size_t len)
{
	k_mutex_lock(&uart_output_mutex, K_FOREVER);

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_output_dev, data[i]);
	}

	k_mutex_unlock(&uart_output_mutex);

	k_sleep(UART_OUTPUT_MESSAGE_GAP);
}
