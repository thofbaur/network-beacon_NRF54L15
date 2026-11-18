/* Fake of the led.c entry points used by radio.c, motion.c, self_report.c. */
#include <stdint.h>

#include "fake_peers.h"

void led_suspend_blinking(void)
{
	fake_peers.led_suspend_calls++;
}

void led_resume_blinking(void)
{
	fake_peers.led_resume_calls++;
}

void led_signal_self_report(void)
{
	fake_peers.led_self_report_signals++;
}

void led_command_begin(void)
{
	fake_peers.led_begin++;
}

void led_apply_command(uint8_t parameter, uint16_t value)
{
	if (fake_peers.led_cmd_count < FAKE_MAX_CALLS) {
		fake_peers.led_cmds[fake_peers.led_cmd_count].parameter = parameter;
		fake_peers.led_cmds[fake_peers.led_cmd_count].value = value;
		fake_peers.led_cmd_count++;
	}
}

void led_command_commit(void)
{
	fake_peers.led_commit++;
}
