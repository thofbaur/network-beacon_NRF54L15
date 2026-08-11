#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "param_storage.h"

static bool settings_initialized;

#define PARAM_STORAGE_MAGIC 0x44534150U
#define PARAM_STORAGE_VERSION 1U
#define PARAM_STORAGE_HEADER_SIZE 12U
#define PARAM_STORAGE_MAX_PAYLOAD 64U

static int param_storage_init(void)
{
	int err;

	if (settings_initialized) {
		return 0;
	}

	err = settings_subsys_init();
	if (err) {
		printk("Settings init failed (err %d)\n", err);
		return err;
	}

	settings_initialized = true;
	return 0;
}

int param_storage_load(const char *key, void *data, size_t len)
{
	uint8_t record[PARAM_STORAGE_HEADER_SIZE + PARAM_STORAGE_MAX_PAYLOAD];
	uint32_t stored_crc;
	uint32_t calculated_crc;
	uint16_t payload_len;
	ssize_t loaded;
	int err;

	if (!key || !data || len == 0 || len > PARAM_STORAGE_MAX_PAYLOAD) {
		return -EINVAL;
	}

	err = param_storage_init();
	if (err) {
		return err;
	}

	loaded = settings_load_one(key, record, sizeof(record));
	if (loaded < 0) {
		return loaded;
	}
	/* CONFIG_SETTINGS_NVS has no csi_load_one, so settings_load_one()
	 * falls back to the generic csi_load() path in settings_store.c: it
	 * walks every stored entry looking for a name match and, finding
	 * none, returns 0 (success) rather than a negative errno. A real
	 * saved record is never 0 bytes (param_storage_save() rejects
	 * len == 0), so this can only mean the key was never saved.
	 */
	if (loaded == 0) {
		return -ENOENT;
	}

	if ((size_t)loaded != PARAM_STORAGE_HEADER_SIZE + len ||
	    sys_get_be32(&record[0]) != PARAM_STORAGE_MAGIC ||
	    sys_get_be16(&record[4]) != PARAM_STORAGE_VERSION) {
		return -EBADMSG;
	}

	payload_len = sys_get_be16(&record[6]);
	if (payload_len != len) {
		return -EBADMSG;
	}

	stored_crc = sys_get_be32(&record[8]);
	calculated_crc = crc32_ieee(&record[PARAM_STORAGE_HEADER_SIZE],
				    payload_len);
	if (stored_crc != calculated_crc) {
		return -EBADMSG;
	}

	memcpy(data, &record[PARAM_STORAGE_HEADER_SIZE], len);
	return 0;
}

int param_storage_load_legacy(const char *key, void *data, size_t len)
{
	uint8_t legacy[PARAM_STORAGE_MAX_PAYLOAD + 1U];
	ssize_t loaded;
	int err;

	if (!key || !data || len == 0 || len > PARAM_STORAGE_MAX_PAYLOAD) {
		return -EINVAL;
	}

	err = param_storage_init();
	if (err) {
		return err;
	}

	loaded = settings_load_one(key, legacy, len + 1U);
	if (loaded < 0) {
		return (int)loaded;
	}
	/* See the matching comment in param_storage_load(): under
	 * CONFIG_SETTINGS_NVS, "key not found" is a 0 return, not negative.
	 */
	if (loaded == 0) {
		return -ENOENT;
	}
	if ((size_t)loaded != len) {
		return -EINVAL;
	}

	memcpy(data, legacy, len);
	return 0;
}

int param_storage_save(const char *key, const void *data, size_t len)
{
	uint8_t record[PARAM_STORAGE_HEADER_SIZE + PARAM_STORAGE_MAX_PAYLOAD];
	int err;

	if (!key || !data || len == 0 || len > PARAM_STORAGE_MAX_PAYLOAD) {
		return -EINVAL;
	}

	err = param_storage_init();
	if (err) {
		return err;
	}

	sys_put_be32(PARAM_STORAGE_MAGIC, &record[0]);
	sys_put_be16(PARAM_STORAGE_VERSION, &record[4]);
	sys_put_be16((uint16_t)len, &record[6]);
	memcpy(&record[PARAM_STORAGE_HEADER_SIZE], data, len);
	sys_put_be32(crc32_ieee(&record[PARAM_STORAGE_HEADER_SIZE], len),
		     &record[8]);

	err = settings_save_one(key, record, PARAM_STORAGE_HEADER_SIZE + len);
	return err;
}

int param_storage_delete(const char *key)
{
	int err;

	err = param_storage_init();
	if (err) {
		return err;
	}

	err = settings_delete(key);
	return err;
}
