/* Fake of the nus.c entry points used by radio.c. */
#include <stdbool.h>

#include "fake_peers.h"

int nus_service_init(void)
{
	return fake_peers.nus_init_err;
}

bool nus_is_connected(void)
{
	return fake_peers.nus_connected;
}
