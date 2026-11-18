#ifndef FAKE_SETTINGS_H
#define FAKE_SETTINGS_H

#include <zephyr/settings/settings.h>

void fake_settings_reset(void);
/* Store a raw value under key, bypassing param_storage's record format
 * (e.g. to plant a legacy/old-version record).
 */
void fake_settings_put_raw(const char *key, const void *data, size_t len);
ssize_t fake_settings_get_raw(const char *key, void *data, size_t len);
bool fake_settings_has(const char *key);
void fake_settings_set_init_error(int err);
void fake_settings_set_load_error(int err);
void fake_settings_set_save_error(int err);
int fake_settings_save_count(void);

#endif /* FAKE_SETTINGS_H */
