#ifndef SHIM_ZEPHYR_SYS_BYTEORDER_H
#define SHIM_ZEPHYR_SYS_BYTEORDER_H

#include "shim_common.h"

static inline uint16_t sys_get_be16(const uint8_t src[2])
{
	return (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
}

static inline uint32_t sys_get_be24(const uint8_t src[3])
{
	return ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | src[2];
}

static inline uint32_t sys_get_be32(const uint8_t src[4])
{
	return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
	       ((uint32_t)src[2] << 8) | src[3];
}

static inline void sys_put_be16(uint16_t val, uint8_t dst[2])
{
	dst[0] = (uint8_t)(val >> 8);
	dst[1] = (uint8_t)val;
}

static inline void sys_put_be24(uint32_t val, uint8_t dst[3])
{
	dst[0] = (uint8_t)(val >> 16);
	dst[1] = (uint8_t)(val >> 8);
	dst[2] = (uint8_t)val;
}

static inline void sys_put_be32(uint32_t val, uint8_t dst[4])
{
	dst[0] = (uint8_t)(val >> 24);
	dst[1] = (uint8_t)(val >> 16);
	dst[2] = (uint8_t)(val >> 8);
	dst[3] = (uint8_t)val;
}

#define sys_cpu_to_le16(x) (x)

#endif /* SHIM_ZEPHYR_SYS_BYTEORDER_H */
