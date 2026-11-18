/*
 * Recording fakes for the domain entry points a module under test calls in
 * its neighbours (radio <-> led/network/motion/eco log/nus, device -> radio,
 * motion -> radio/led, self_report -> led). Each fake lives in its own .c
 * file so a test can link the real module instead of its fake.
 */
#ifndef FAKE_PEERS_H
#define FAKE_PEERS_H

#include <stdbool.h>
#include <stdint.h>

#define FAKE_MAX_CALLS 512

struct fake_command {
	uint8_t parameter;
	uint16_t value;
};

struct fake_contact {
	uint8_t id;
	int8_t rssi;
	bool is_location_tag;
};

struct fake_peers {
	/* radio status (fake_radio_status.c) */
	int radio_status_updates;
	int adv_updates;
	int adv_update_err;
	int eco_override_calls;
	bool eco_override;
	/* led (fake_led_api.c) */
	int led_suspend_calls;
	int led_resume_calls;
	int led_self_report_signals;
	int led_begin;
	int led_commit;
	struct fake_command led_cmds[FAKE_MAX_CALLS];
	int led_cmd_count;
	/* network (fake_network_api.c) */
	int network_begin;
	int network_commit;
	struct fake_command network_cmds[FAKE_MAX_CALLS];
	int network_cmd_count;
	struct fake_contact contacts[FAKE_MAX_CALLS];
	int contact_count;
	/* motion (fake_motion_api.c) */
	int motion_begin;
	int motion_commit;
	struct fake_command motion_cmds[FAKE_MAX_CALLS];
	int motion_cmd_count;
	/* eco log (fake_eco_log_api.c) */
	int eco_enter_calls;
	int eco_leave_calls;
	/* nus (fake_nus_api.c) */
	int nus_init_err;
	bool nus_connected;
};

extern struct fake_peers fake_peers;

void fake_peers_reset(void);

#endif /* FAKE_PEERS_H */
