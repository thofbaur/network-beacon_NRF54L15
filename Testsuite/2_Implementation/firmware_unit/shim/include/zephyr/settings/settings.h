/*
 * Settings API shim backed by fakes/fake_settings.c. Mirrors the
 * CONFIG_SETTINGS_NVS behaviour param_storage.c depends on (checked against
 * zephyr/subsys/settings/src/settings_store.c, NCS v3.3.0):
 *   - settings_load_one() copies MIN(buf_len, stored) bytes and returns the
 *     STORED length; a key that was never saved returns 0, not an error.
 */
#ifndef SHIM_ZEPHYR_SETTINGS_SETTINGS_H
#define SHIM_ZEPHYR_SETTINGS_SETTINGS_H

#include "shim_common.h"

int settings_subsys_init(void);
ssize_t settings_load_one(const char *name, void *buf, size_t buf_len);
int settings_save_one(const char *name, const void *value, size_t val_len);
int settings_delete(const char *name);

#endif /* SHIM_ZEPHYR_SETTINGS_SETTINGS_H */
