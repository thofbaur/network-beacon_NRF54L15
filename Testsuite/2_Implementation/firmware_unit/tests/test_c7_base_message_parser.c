/*
 * C7 - Network_Base_nrf54/src/message_parser.c: decoding of one NUS
 * notification (flag byte + payload) into the Base's parsed text output
 * (CONFIG_DSA_OUTPUT_FORMAT_PARSED). Output is captured by fake_output.c.
 * See 1_Specification/C_firmware_unit.md.
 */
#include "common_include.h"
#include "fake_output.h"
#include "message_parser.h"
#include "minitest.h"

static int finished_calls;

static void on_finished(void)
{
	finished_calls++;
}

static void setup(void)
{
	fake_output_reset();
	finished_calls = 0;
	message_parser_init(on_finished);
}

static void feed(uint8_t id, const uint8_t *data, uint16_t len)
{
	message_parser_feed(id, data, len);
}

#define LINE(i) fake_output_lines[(i)]

MT_TEST(c7_time_package)
{
	const uint8_t d[] = { DSA_NUS_FLAG_TIME, 0x00, 0x01, 0x02, 0x03 };

	feed(5, d, sizeof(d));
	MT_ASSERT_EQ(1, fake_output_line_count);
	MT_ASSERT_STR_CONTAINS(LINE(0), "ID: 5 Current Timer:66051");
}

MT_TEST(c7_time_contacts_voltage_package)
{
	const uint8_t d[] = { DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE,
			      0x00, 0x00, 0x10, 0x00,	/* uptime 4096 */
			      0xFF, 0xFF, 0xFF,		/* 16777215 contacts */
			      0x0B, 0xB8 };		/* 3000 mV */

	feed(48, d, sizeof(d));
	MT_ASSERT_EQ(1, fake_output_line_count);
	MT_ASSERT_STR_CONTAINS(LINE(0),
		"ID: 48 Current Timer: 4096 Contact Count: 16777215 Voltage: 3000");
}

MT_TEST(c7_data_block_with_two_contacts)
{
	const uint8_t d[] = { DSA_NUS_FLAG_DATA, 2,
			      7, 0x01, 0x00, 0x00, 80,
			      9, 0xFF, 0xFF, 0xFF, 45 };

	feed(1, d, sizeof(d));
	MT_ASSERT_EQ(2, fake_output_line_count);
	MT_ASSERT_STR_CONTAINS(LINE(0), "ID: 1 Contact-ID: 7 Timer: 65536 RSSI: -80");
	MT_ASSERT_STR_CONTAINS(LINE(1), "ID: 1 Contact-ID: 9 Timer: 16777215 RSSI: -45");
}

MT_TEST(c7_data_block_count_mismatch_is_rejected)
{
	const uint8_t d[] = { DSA_NUS_FLAG_DATA, 2, 7, 0x01, 0x00, 0x00, 80 };

	feed(1, d, sizeof(d));
	MT_ASSERT_EQ(0, fake_output_line_count);
}

MT_TEST(c7_empty_data_block)
{
	const uint8_t d[] = { DSA_NUS_FLAG_DATA, 0 };

	feed(1, d, sizeof(d));
	MT_ASSERT_EQ(0, fake_output_line_count);
}

MT_TEST(c7_voltage_package)
{
	const uint8_t d[] = { DSA_NUS_FLAG_VOLTAGE, 0x0A, 0x8C };

	feed(3, d, sizeof(d));
	MT_ASSERT_STR_CONTAINS(LINE(0), "ID: 3 VOLTAGE: 2700");
}

MT_TEST(c7_self_report_block)
{
	const uint8_t d[] = { DSA_NUS_FLAG_SELF_REPORT, 2,
			      0x00, 0x00, 0x2A, 0x12, 0x34, 0x56 };

	feed(4, d, sizeof(d));
	MT_ASSERT_EQ(2, fake_output_line_count);
	MT_ASSERT_STR_CONTAINS(LINE(0), "Self-report time: 42");
	MT_ASSERT_STR_CONTAINS(LINE(1), "Self-report time: 1193046");
}

MT_TEST(c7_eco_log_block)
{
	const uint8_t d[] = { DSA_NUS_FLAG_ECO_LOG, 1,
			      0x00, 0x00, 0x64, 0x00, 0x00, 0xA0 };

	feed(4, d, sizeof(d));
	MT_ASSERT_EQ(1, fake_output_line_count);
	MT_ASSERT_STR_CONTAINS(LINE(0), "ID: 4 Eco session enter: 100 leave: 160");
}

MT_TEST(c7_eco_log_length_mismatch_is_rejected)
{
	const uint8_t d[] = { DSA_NUS_FLAG_ECO_LOG, 2, 0, 0, 1, 0, 0, 2 };

	feed(4, d, sizeof(d));
	MT_ASSERT_EQ(0, fake_output_line_count);
}

MT_TEST(c7_finished_control_package_invokes_callback)
{
	const uint8_t d[] = { DSA_NUS_FLAG_CONTROL, 'f', 'i', 'n', 'i', 's', 'h', 'e', 'd' };

	feed(2, d, sizeof(d));
	MT_ASSERT_EQ(1, finished_calls);
	MT_ASSERT_STR_CONTAINS(LINE(0), "ID: 2 Transfer complete. Disconnecting");
}

MT_TEST(c7_other_control_package_is_ignored)
{
	const uint8_t d[] = { DSA_NUS_FLAG_CONTROL, 'u', 'n', 'k', 'n', 'o', 'w', 'n', '!' };

	feed(2, d, sizeof(d));
	MT_ASSERT_EQ(0, finished_calls);
	MT_ASSERT_EQ(0, fake_output_line_count);
}

MT_TEST(c7_wrong_fixed_lengths_are_rejected)
{
	const uint8_t time_short[] = { DSA_NUS_FLAG_TIME, 1, 2, 3 };
	const uint8_t volt_long[] = { DSA_NUS_FLAG_VOLTAGE, 1, 2, 3 };
	const uint8_t tcv_short[] = { DSA_NUS_FLAG_TIME_CONTACTS_VOLTAGE, 1, 2 };
	const uint8_t ctrl_short[] = { DSA_NUS_FLAG_CONTROL, 'f', 'i' };

	feed(1, time_short, sizeof(time_short));
	feed(1, volt_long, sizeof(volt_long));
	feed(1, tcv_short, sizeof(tcv_short));
	feed(1, ctrl_short, sizeof(ctrl_short));
	MT_ASSERT_EQ(0, fake_output_line_count);
	MT_ASSERT_EQ(0, finished_calls);
}

MT_TEST(c7_unknown_flag_is_dumped_as_default_package)
{
	const uint8_t d[] = { 0x42, 1, 2, 255 };

	feed(9, d, sizeof(d));
	MT_ASSERT_EQ(1, fake_output_line_count);
	MT_ASSERT_STR_CONTAINS(LINE(0), "ID: 9 DEFAULT uint8=66 1 2 255");
}

MT_TEST(c7_empty_notification_is_ignored)
{
	feed(9, (const uint8_t *)"", 0);
	MT_ASSERT_EQ(0, fake_output_line_count);
}

MT_TEST(c7_beacon_id_printed_as_decimal_for_all_ids)
{
	const uint8_t d[] = { DSA_NUS_FLAG_VOLTAGE, 0x00, 0x01 };
	char expected[32];

	for (unsigned int id = 0; id < 256; id++) {
		fake_output_reset();
		feed((uint8_t)id, d, sizeof(d));
		snprintf(expected, sizeof(expected), "ID: %u VOLTAGE: 1", id);
		MT_ASSERT_STR_CONTAINS(LINE(0), expected);
	}
}

MT_TEST(c7_maximum_data_block_fits_one_notification)
{
	/* 1 flag + 1 count + 48 contacts = 242 bytes <= 244 NUS payload. */
	uint8_t d[2 + 48 * 5];

	d[0] = DSA_NUS_FLAG_DATA;
	d[1] = 48;
	for (int i = 0; i < 48; i++) {
		d[2 + i * 5] = (uint8_t)i;
		d[3 + i * 5] = 0;
		d[4 + i * 5] = 0;
		d[5 + i * 5] = (uint8_t)i;
		d[6 + i * 5] = 60;
	}
	feed(1, d, sizeof(d));
	MT_ASSERT_EQ(48, fake_output_line_count);
	MT_ASSERT_STR_CONTAINS(LINE(47), "Contact-ID: 47 Timer: 47 RSSI: -60");
}

MT_MAIN_WITH_SETUP(setup,
	MT_CASE(c7_time_package),
	MT_CASE(c7_time_contacts_voltage_package),
	MT_CASE(c7_data_block_with_two_contacts),
	MT_CASE(c7_data_block_count_mismatch_is_rejected),
	MT_CASE(c7_empty_data_block),
	MT_CASE(c7_voltage_package),
	MT_CASE(c7_self_report_block),
	MT_CASE(c7_eco_log_block),
	MT_CASE(c7_eco_log_length_mismatch_is_rejected),
	MT_CASE(c7_finished_control_package_invokes_callback),
	MT_CASE(c7_other_control_package_is_ignored),
	MT_CASE(c7_wrong_fixed_lengths_are_rejected),
	MT_CASE(c7_unknown_flag_is_dumped_as_default_package),
	MT_CASE(c7_empty_notification_is_ignored),
	MT_CASE(c7_beacon_id_printed_as_decimal_for_all_ids),
	MT_CASE(c7_maximum_data_block_fits_one_notification))
