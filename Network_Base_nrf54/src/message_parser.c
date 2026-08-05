#include "message_parser.h"

#include <stddef.h>
#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

#include "common_include.h"
#include "output.h"

LOG_MODULE_DECLARE(network_base);

#define DSA_TIME_LEN 4
#define DSA_DATA_COUNT_LEN 1
#define DSA_DATA_SET_LEN 5
#define DSA_SELF_REPORT_SET_LEN 3
#define DSA_VOLTAGE_LEN 2
#define DSA_CONTROL_LEN 8
#define DSA_TIME_CONTACT_VOLTAGE_LEN 9

static message_parser_finished_cb_t parser_finished_cb;

static size_t expected_len_for_flag(uint8_t flag)
{
	switch (flag) {
	case DSA_NUS_FLAG_TIME:
		return DSA_TIME_LEN;
	case DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE:
		return DSA_TIME_CONTACT_VOLTAGE_LEN;
	case DSA_NUS_FLAG_DATA:
		return DSA_DATA_COUNT_LEN;
	case DSA_NUS_FLAG_VOLTAGE:
		return DSA_VOLTAGE_LEN;
	case DSA_NUS_FLAG_CONTROL:
		return DSA_CONTROL_LEN;
	case DSA_NUS_FLAG_SELF_REPORT:
		return DSA_SELF_REPORT_SET_LEN;
	default:
		return 0;
	}
}

static bool is_known_flag(uint8_t byte)
{
	return expected_len_for_flag(byte) != 0;
}

static uint32_t uint32_be_decode(const uint8_t *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) | data[3];
}

static uint32_t uint24_be_decode(const uint8_t *data)
{
	return ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) |
	       data[2];
}

static uint32_t uint16_be_decode(const uint8_t *data)
{
	return ((uint32_t)data[0] << 8) | data[1];
}

static void handle_time_package(uint8_t beacon_id, const uint8_t *data)
{
	uint32_t timer = uint32_be_decode(data);

	output_messagef("ID:%u Current Timer:%u", beacon_id, timer);
}

static void handle_time_contact_voltage_package(uint8_t beacon_id,
						const uint8_t *data)
{
	uint32_t timer = uint32_be_decode(data);
	uint32_t contact_count = uint24_be_decode(&data[4]);
	uint16_t voltage = uint16_be_decode(&data[7]);

	output_messagef("ID:%u Current Timer:%u Contact Count:%u Voltage:%u",
			beacon_id, timer, contact_count, voltage);
}

static void handle_data_package(uint8_t beacon_id, const uint8_t *data)
{
	uint8_t contact_id = data[0];
	uint32_t timer = uint24_be_decode(&data[1]);
	uint8_t negative_rssi = data[4];

	output_messagef("ID:%u Contact-ID:%u Timer:%u RSSI:-%u", beacon_id,
			contact_id, timer, negative_rssi);
}

static void handle_data_block(uint8_t beacon_id, const uint8_t *data,
			      size_t len)
{
	uint8_t set_count = data[0];
	size_t expected_len = DSA_DATA_COUNT_LEN +
			      ((size_t)set_count * DSA_DATA_SET_LEN);

	if (len != expected_len) {
		LOG_WRN("Invalid DATA block length %u for %u contacts",
			(unsigned int)len, set_count);
		return;
	}

	for (size_t i = 0; i < set_count; i++) {
		handle_data_package(beacon_id,
				    &data[DSA_DATA_COUNT_LEN +
					  (i * DSA_DATA_SET_LEN)]);
	}
}

static void handle_voltage_package(uint8_t beacon_id, const uint8_t *data)
{
	uint16_t voltage = uint16_be_decode(data);

	output_messagef("ID:%u VOLTAGE:%u", beacon_id, voltage);
}

static void handle_self_report_block(uint8_t beacon_id, const uint8_t *data,
				     size_t len)
{
	size_t report_count = len / DSA_SELF_REPORT_SET_LEN;

	for (size_t i = 0; i < report_count; i++) {
		uint32_t timer = uint24_be_decode(
			&data[i * DSA_SELF_REPORT_SET_LEN]);

		output_messagef("ID:%u  Self-report time: %u", beacon_id,
				timer);
	}
}

static void handle_control_package(uint8_t beacon_id, const uint8_t *data)
{
	if (memcmp(data, "finished", DSA_CONTROL_LEN) == 0) {
		output_messagef("ID:%u Transfer complete. Disconnecting",
				beacon_id);

		LOG_INF("Received finished control package");

		if (parser_finished_cb) {
			parser_finished_cb();
		}
	}
}

static void handle_default_package(uint8_t beacon_id, const uint8_t *data,
				   size_t len)
{
	char message[160];
	size_t pos;

	if (len == 0) {
		output_messagef("ID:%u DEFAULT uint8=<empty>", beacon_id);
		return;
	}

	pos = snprintk(message, sizeof(message), "ID:%u DEFAULT uint8=",
		       beacon_id);

	for (size_t i = 0; (i < len) && (pos < sizeof(message)); i++) {
		int written = snprintk(&message[pos], sizeof(message) - pos,
				       "%u%s", data[i],
				       (i + 1 == len) ? "" : " ");

		if (written < 0) {
			break;
		}

		pos += (size_t)written;
	}

	message[sizeof(message) - 1] = '\0';
	output_message(message);
}

static void handle_complete_package(uint8_t beacon_id, uint8_t flag,
				    const uint8_t *data, size_t len)
{
	switch (flag) {
	case DSA_NUS_FLAG_TIME:
		if (len != DSA_TIME_LEN) {
			LOG_WRN("Invalid TIME package length %u",
				(unsigned int)len);
			break;
		}
		handle_time_package(beacon_id, data);
		break;
	case DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE:
		if (len != DSA_TIME_CONTACT_VOLTAGE_LEN) {
			LOG_WRN("Invalid TIME+CONTACT+VOLTAGE package length %u",
				(unsigned int)len);
			break;
		}
		handle_time_contact_voltage_package(beacon_id, data);
		break;
	case DSA_NUS_FLAG_DATA:
		if (len < DSA_DATA_COUNT_LEN) {
			LOG_WRN("Invalid DATA block length %u",
				(unsigned int)len);
			break;
		}
		handle_data_block(beacon_id, data, len);
		break;
	case DSA_NUS_FLAG_VOLTAGE:
		if (len != DSA_VOLTAGE_LEN) {
			LOG_WRN("Invalid VOLTAGE package length %u",
				(unsigned int)len);
			break;
		}
		handle_voltage_package(beacon_id, data);
		break;
	case DSA_NUS_FLAG_CONTROL:
		if (len != DSA_CONTROL_LEN) {
			LOG_WRN("Invalid CONTROL package length %u",
				(unsigned int)len);
			break;
		}
		handle_control_package(beacon_id, data);
		break;
	case DSA_NUS_FLAG_SELF_REPORT:
		if ((len == 0) || ((len % DSA_SELF_REPORT_SET_LEN) != 0)) {
			LOG_WRN("Invalid SELF_REPORT block length %u",
				(unsigned int)len);
			break;
		}
		handle_self_report_block(beacon_id, data, len);
		break;
	default:
		handle_default_package(beacon_id, data, len);
		break;
	}
}

void message_parser_init(message_parser_finished_cb_t finished_cb)
{
	parser_finished_cb = finished_cb;
	message_parser_reset();
}

void message_parser_reset(void)
{
	/* Stateless parser: each NUS notification carries one complete package. */
}

void message_parser_feed(uint8_t beacon_id, const uint8_t *data, uint16_t len)
{
	uint8_t flag;

	if (len < 1) {
		LOG_WRN("Ignoring empty NUS package");
		return;
	}

	flag = data[0];
	if (!is_known_flag(flag)) {
		LOG_INF("No known NUS flag matched; using default data package");
		handle_default_package(beacon_id, data, len);
		return;
	}

	handle_complete_package(beacon_id, flag, &data[1], len - 1);
}
