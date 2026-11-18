#ifndef SHIM_ZEPHYR_DRIVERS_FLASH_H
#define SHIM_ZEPHYR_DRIVERS_FLASH_H

#include "shim_common.h"
#include <zephyr/device.h>

struct flash_pages_info {
	off_t start_offset;
	size_t size;
	uint32_t index;
};

int flash_get_page_info_by_offs(const struct device *dev, off_t offset,
				struct flash_pages_info *info);
int flash_erase(const struct device *dev, off_t offset, size_t size);

/* ---- test control ------------------------------------------------------ */
void shim_flash_set_page_size(size_t size);
void shim_flash_set_page_info_error(int err);
void shim_flash_set_erase_error(int err);
int shim_flash_erase_count(void);

#endif /* SHIM_ZEPHYR_DRIVERS_FLASH_H */
