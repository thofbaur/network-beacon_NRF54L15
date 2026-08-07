#ifndef MOTION_H
#define MOTION_H

#include <stdint.h>

int motion_init(void);
void motion_command_begin(void);
void motion_apply_command(uint8_t parameter, uint16_t value);
void motion_command_commit(void);
int motion_params_load(void);
int motion_params_save(void);

#endif /* MOTION_H */
