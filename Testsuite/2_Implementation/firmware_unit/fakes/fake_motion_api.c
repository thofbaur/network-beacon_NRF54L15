/* Fake of the motion.c entry points used by radio.c. */
#include <stdint.h>

#include "fake_peers.h"

void motion_command_begin(void)
{
	fake_peers.motion_begin++;
}

void motion_apply_command(uint8_t parameter, uint16_t value)
{
	if (fake_peers.motion_cmd_count < FAKE_MAX_CALLS) {
		fake_peers.motion_cmds[fake_peers.motion_cmd_count].parameter = parameter;
		fake_peers.motion_cmds[fake_peers.motion_cmd_count].value = value;
		fake_peers.motion_cmd_count++;
	}
}

void motion_command_commit(void)
{
	fake_peers.motion_commit++;
}
