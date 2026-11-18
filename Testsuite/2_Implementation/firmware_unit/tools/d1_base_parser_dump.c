/*
 * D1 helper: feeds NUS notifications to the Base's message_parser.c and
 * prints every output line. Input (stdin), one notification per line:
 *
 *     <beacon_id_decimal> <hex bytes of flag+payload>
 *
 * Output (stdout): for each input line, "#<n>" followed by the parsed lines,
 * and "FINISHED" when the finished callback fired. Used by
 * 2_Implementation/tests/contract/test_d1_frame_contract.py to compare the Base's decoding
 * with dsa_logger.py's decoding of the same frames.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_output.h"
#include "message_parser.h"

static int finished;

static void on_finished(void)
{
	finished = 1;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

int main(void)
{
	static char line[4096];
	int n = 0;

	message_parser_init(on_finished);
	while (fgets(line, sizeof(line), stdin)) {
		uint8_t data[1024];
		uint16_t len = 0;
		unsigned int id;
		char *p;

		if (sscanf(line, "%u", &id) != 1) {
			continue;
		}
		p = strchr(line, ' ');
		while (p && *p) {
			int hi, lo;

			while (*p == ' ') {
				p++;
			}
			hi = hex_value(p[0]);
			lo = hi < 0 ? -1 : hex_value(p[1]);
			if (hi < 0 || lo < 0) {
				break;
			}
			data[len++] = (uint8_t)(hi * 16 + lo);
			p += 2;
		}
		fake_output_reset();
		finished = 0;
		message_parser_feed((uint8_t)id, data, len);
		printf("#%d\n", n++);
		for (int i = 0; i < fake_output_line_count; i++) {
			printf("%s\n", fake_output_lines[i]);
		}
		if (finished) {
			printf("FINISHED\n");
		}
	}
	return 0;
}
