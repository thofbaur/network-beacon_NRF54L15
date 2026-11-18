/*
 * In-RAM NVS fake with fault injection. Partitions are identified by their
 * flash offset and survive simulated reboots (re-initialising the firmware's
 * store structs), exactly like real flash does.
 */
#ifndef FAKE_NVS_H
#define FAKE_NVS_H

#include <zephyr/fs/nvs.h>

/* Erase every partition and clear all fault injection. */
void fake_nvs_reset_all(void);

/* Power-cut simulation: the next `writes_before_cut` writes/deletes succeed,
 * the one after that is lost and every later write/delete fails with -EIO
 * until fake_nvs_power_cycle() ("the device rebooted"). Negative = disabled.
 */
void fake_nvs_cut_power_after(int writes_before_cut);
void fake_nvs_power_cycle(void);
bool fake_nvs_power_is_cut(void);

/* Persistent I/O errors: every read (write, delete) returns err until reset
 * with 0. read_error_id limits the read error to one record id (-1 = all).
 */
void fake_nvs_set_read_error(int err, int read_error_id);
void fake_nvs_set_write_error(int err);
void fake_nvs_set_delete_error(int err);

/* Make the partition at `offset` unmountable (stale foreign data) until an
 * erase covers it.
 */
void fake_nvs_corrupt_partition(off_t offset);

/* Raw access for crafting or inspecting on-flash records. */
void fake_nvs_put_raw(off_t offset, uint16_t id, const void *data, size_t len);
ssize_t fake_nvs_get_raw(off_t offset, uint16_t id, void *data, size_t len);
bool fake_nvs_has(off_t offset, uint16_t id);
size_t fake_nvs_record_count(off_t offset);

int fake_nvs_write_count(void);
int fake_nvs_delete_count(void);
int fake_nvs_mount_count(void);
int fake_nvs_clear_count(void);

/* Called by the flash_erase() shim. */
void fake_nvs_erase_range(off_t offset, size_t size);

#endif /* FAKE_NVS_H */
