#include <string.h>

#include "fake_peers.h"

struct fake_peers fake_peers;

void fake_peers_reset(void)
{
	memset(&fake_peers, 0, sizeof(fake_peers));
}
