/* Fake of the radio.c entry points used by device.c, network.c, motion.c. */
#include <stdbool.h>

#include "fake_peers.h"

void radio_schedule_status_update(void)
{
	fake_peers.radio_status_updates++;
}

int adv_update(void)
{
	fake_peers.adv_updates++;
	return fake_peers.adv_update_err;
}

void radio_set_eco_override(bool active)
{
	fake_peers.eco_override_calls++;
	fake_peers.eco_override = active;
}
