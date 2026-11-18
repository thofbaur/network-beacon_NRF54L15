#ifndef SHIM_ZEPHYR_SYS_CRC_H
#define SHIM_ZEPHYR_SYS_CRC_H

#include "shim_common.h"

/* Same algorithm as Zephyr's crc32_ieee(): reflected CRC-32, polynomial
 * 0xEDB88320, initial value and final XOR 0xFFFFFFFF.
 */
static inline uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data,
					 size_t len)
{
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
		}
	}
	return ~crc;
}

static inline uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
	return crc32_ieee_update(0, data, len);
}

#endif /* SHIM_ZEPHYR_SYS_CRC_H */
