/*
 * NVS API shim backed by an in-RAM fake (fakes/fake_nvs.c). The fake
 * reproduces the Zephyr NVS behaviour the firmware relies on - checked
 * against zephyr/subsys/fs/nvs/nvs.c of NCS v3.3.0:
 *   - nvs_read() copies MIN(len, stored) bytes and returns the STORED length,
 *     -ENOENT when the id was never written or was deleted.
 *   - nvs_write() returns len, or 0 when identical data is already stored;
 *     -EINVAL when len exceeds sector_size - 4 ATEs (16-byte ATEs on the
 *     nRF54L RRAM, i.e. sector_size - 64).
 *   - nvs_clear() erases the partition AND leaves the file system unmounted
 *     (fs->ready = false); every following read/write/delete returns -EACCES
 *     until nvs_mount() is called again.
 *   - each write is atomic: a power cut loses the write in progress but never
 *     leaves a partial entry behind.
 */
#ifndef SHIM_ZEPHYR_FS_NVS_H
#define SHIM_ZEPHYR_FS_NVS_H

#include "shim_common.h"
#include <zephyr/device.h>

struct nvs_fs {
	off_t offset;
	uint32_t ate_wra;
	uint32_t data_wra;
	uint16_t sector_size;
	uint16_t sector_count;
	bool ready;
	const struct device *flash_device;
};

int nvs_mount(struct nvs_fs *fs);
int nvs_clear(struct nvs_fs *fs);
ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len);
int nvs_delete(struct nvs_fs *fs, uint16_t id);
ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len);

#endif /* SHIM_ZEPHYR_FS_NVS_H */
