#include "output.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#if defined(DSA_OUTPUT_RTT)
#include "rtt_output.h"
#elif defined(DSA_OUTPUT_UART)
#include "uart_output.h"
#endif

#define OUTPUT_RAW_PREFIX_MAX 3
#define OUTPUT_RAW_SUFFIX_MAX 2
#define OUTPUT_DATA_MAX \
	(OUTPUT_RAW_PREFIX_MAX + CONFIG_BT_L2CAP_TX_MTU + OUTPUT_RAW_SUFFIX_MAX)
#define OUTPUT_MESSAGE_MAX 160
#define OUTPUT_QUEUE_LEN 16
#define OUTPUT_THREAD_STACK_SIZE 1024
#define OUTPUT_THREAD_PRIORITY 7

LOG_MODULE_DECLARE(network_base);

struct output_item {
	uint8_t data[OUTPUT_DATA_MAX];
	size_t len;
	bool text;
};

K_MSGQ_DEFINE(output_queue, sizeof(struct output_item), OUTPUT_QUEUE_LEN, 4);
K_THREAD_STACK_DEFINE(output_thread_stack, OUTPUT_THREAD_STACK_SIZE);

static struct k_thread output_thread;
static bool output_ready;

static void output_thread_fn(void *p1, void *p2, void *p3)
{
	struct output_item item;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_msgq_get(&output_queue, &item, K_FOREVER);
#if defined(DSA_OUTPUT_RTT)
		if (item.text) {
			rtt_output_message((const char *)item.data);
		} else {
			rtt_output_data(item.data, item.len);
		}
#elif defined(DSA_OUTPUT_UART)
		if (item.text) {
			uart_output_message((const char *)item.data);
		} else {
			uart_output_data(item.data, item.len);
		}
#else
		LOG_WRN("No output backend selected");
#endif
	}
}

int output_init(void)
{
	int err;

#if defined(DSA_OUTPUT_RTT)
	err = rtt_output_init();
#elif defined(DSA_OUTPUT_UART)
	err = uart_output_init();
#else
	return -ENODEV;
#endif

	if (err) {
		return err;
	}

	output_ready = true;

	k_thread_create(&output_thread, output_thread_stack,
			K_THREAD_STACK_SIZEOF(output_thread_stack),
			output_thread_fn, NULL, NULL, NULL,
			OUTPUT_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&output_thread, "output");

	return 0;
}

static void output_item_submit(struct output_item *item)
{
	if (!output_ready) {
		return;
	}

	if (k_msgq_put(&output_queue, item, K_FOREVER) != 0) {
		LOG_ERR("Output queue put failed");
	}
}

void output_data(const uint8_t *data, size_t len)
{
	struct output_item item = {
		.len = MIN(len, sizeof(item.data)),
		.text = false,
	};

	if (item.len > 0) {
		memcpy(item.data, data, item.len);
	}
	output_item_submit(&item);
}

void output_message(const char *message)
{
	struct output_item item = {
		.text = true,
	};

	(void)snprintk((char *)item.data, sizeof(item.data), "%s", message);
	item.len = strlen((const char *)item.data);
	output_item_submit(&item);
}

void output_messagef(const char *fmt, ...)
{
	char message[OUTPUT_MESSAGE_MAX];
	va_list args;

	va_start(args, fmt);
	(void)vsnprintk(message, sizeof(message), fmt, args);
	va_end(args);

	output_message(message);
}
