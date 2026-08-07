#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>

int radio_init(void);
int radio_start(void);
int radio_params_load(void);
int radio_params_save(void);
void radio_command_begin(void);
void radio_command_commit(void);
int adv_update(void);
void radio_schedule_status_update(void);
void radio_set_eco_override(bool active);

#endif /* RADIO_H */
