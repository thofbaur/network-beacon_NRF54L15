/* Fake of the eco_log.c entry points used by radio.c. */
#include "fake_peers.h"

void eco_log_enter(void)
{
	fake_peers.eco_enter_calls++;
}

void eco_log_leave(void)
{
	fake_peers.eco_leave_calls++;
}
