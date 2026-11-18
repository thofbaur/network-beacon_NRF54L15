/* Fake of the network.c entry points used by radio.c. */
#include <stdbool.h>
#include <stdint.h>

#include "fake_peers.h"

void network_command_begin(void)
{
	fake_peers.network_begin++;
}

void network_apply_command(uint8_t parameter, uint16_t value)
{
	if (fake_peers.network_cmd_count < FAKE_MAX_CALLS) {
		fake_peers.network_cmds[fake_peers.network_cmd_count].parameter = parameter;
		fake_peers.network_cmds[fake_peers.network_cmd_count].value = value;
		fake_peers.network_cmd_count++;
	}
}

void network_command_commit(void)
{
	fake_peers.network_commit++;
}

void network_evaluate_contact(uint8_t id, int8_t rssi, bool is_location_tag)
{
	if (fake_peers.contact_count < FAKE_MAX_CALLS) {
		fake_peers.contacts[fake_peers.contact_count].id = id;
		fake_peers.contacts[fake_peers.contact_count].rssi = rssi;
		fake_peers.contacts[fake_peers.contact_count].is_location_tag =
			is_location_tag;
		fake_peers.contact_count++;
	}
}
