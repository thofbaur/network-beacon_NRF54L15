#ifndef SHIM_ZEPHYR_RANDOM_RANDOM_H
#define SHIM_ZEPHYR_RANDOM_RANDOM_H
#include "shim_common.h"
/* Deterministic LCG; reseed with shim_rand_seed(). */
uint32_t sys_rand32_get(void);
uint8_t sys_rand8_get(void);
void shim_rand_seed(uint32_t seed);
#endif
