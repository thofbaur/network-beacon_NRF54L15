#ifndef RADIO_H
#define RADIO_H

#define ADV_POS_ID 0
#define ADV_POS_RADIO_STATUS 1
#define ADV_POS_NETWORK_STATUS 2

int radio_init(void);
int radio_start(void);
int radio_params_load(void);
int radio_params_save(void);
void radio_command_begin(void);
void radio_command_commit(void);
int adv_update(void);
void radio_schedule_status_update(void);

#endif /* RADIO_H */
