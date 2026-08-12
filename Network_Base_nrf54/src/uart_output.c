#include "uart_output.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(network_base);

#define UART_OUTPUT_NODE DT_CHOSEN(zephyr_console)
/* Sized well above output.c's OUTPUT_DATA_MAX/OUTPUT_MESSAGE_MAX so a
 * single message normally fits in one DMA transfer; uart_output_message()
 * still flushes in chunks below so nothing is lost if a caller ever
 * exceeds this.
 */
#define UART_OUTPUT_CHUNK_LEN 128

static const struct device *const uart_output_dev =
	DEVICE_DT_GET(UART_OUTPUT_NODE);

K_MUTEX_DEFINE(uart_output_mutex);
K_SEM_DEFINE(uart_output_tx_done, 0, 1);

static void uart_output_callback(const struct device *dev,
				 struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		k_sem_give(&uart_output_tx_done);
		break;
	default:
		break;
	}
}

int uart_output_init(void)
{
	if (!device_is_ready(uart_output_dev)) {
		return -ENODEV;
	}

	return uart_callback_set(uart_output_dev, uart_output_callback, NULL);
}

/* Caller must hold uart_output_mutex. Blocks until this chunk is actually
 * on the wire (the next chunk can't reuse the buffer/hardware until then),
 * but - unlike the byte-by-byte uart_poll_out() this replaces - the wait
 * is a semaphore take, not a busy spin. That lets other threads (notably
 * the BT stack processing incoming NUS notifications) run on the CPU while
 * this transfer is in flight instead of being shut out by it, which is
 * what let a big contact backlog stall BLE reception behind UART output.
 * No added inter-message delay either: the old fixed 5 ms sleep after
 * every write was pure overhead on top of this wait, not a real
 * constraint, so it's gone.
 */
static void uart_output_flush(const uint8_t *data, size_t len)
{
	if (len == 0) {
		return;
	}

	k_sem_reset(&uart_output_tx_done);

	int err = uart_tx(uart_output_dev, data, len, SYS_FOREVER_US);

	if (err) {
		LOG_WRN("uart_tx failed (err %d); dropping %zu byte(s)", err, len);
		return;
	}

	k_sem_take(&uart_output_tx_done, K_FOREVER);
}

void uart_output_message(const char *message)
{
	uint8_t chunk[UART_OUTPUT_CHUNK_LEN];
	size_t chunk_len = 0;
	bool ends_with_newline = false;

	k_mutex_lock(&uart_output_mutex, K_FOREVER);

	for (const char *p = message; *p != '\0'; p++) {
		ends_with_newline = (*p == '\n');

		if (chunk_len + 2 > sizeof(chunk)) {
			uart_output_flush(chunk, chunk_len);
			chunk_len = 0;
		}

		if (*p == '\n') {
			chunk[chunk_len++] = '\r';
		}
		chunk[chunk_len++] = *p;
	}

	if (!ends_with_newline) {
		if (chunk_len + 2 > sizeof(chunk)) {
			uart_output_flush(chunk, chunk_len);
			chunk_len = 0;
		}
		chunk[chunk_len++] = '\r';
		chunk[chunk_len++] = '\n';
	}

	uart_output_flush(chunk, chunk_len);

	k_mutex_unlock(&uart_output_mutex);
}

void uart_output_data(const uint8_t *data, size_t len)
{
	k_mutex_lock(&uart_output_mutex, K_FOREVER);

	while (len > 0) {
		size_t n = MIN(len, UART_OUTPUT_CHUNK_LEN);

		uart_output_flush(data, n);
		data += n;
		len -= n;
	}

	k_mutex_unlock(&uart_output_mutex);
}
