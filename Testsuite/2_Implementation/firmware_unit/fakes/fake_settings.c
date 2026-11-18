#include <stdlib.h>

#include <zephyr/sys/util.h>

#include "fake_settings.h"

#define MAX_KEYS 32

struct entry {
	char key[64];
	uint8_t data[256];
	size_t len;
	bool used;
};

static struct entry entries[MAX_KEYS];
static int init_err;
static int load_err;
static int save_err;
static int saves;

static struct entry *find(const char *key, bool create)
{
	for (int i = 0; i < MAX_KEYS; i++) {
		if (entries[i].used && strcmp(entries[i].key, key) == 0) {
			return &entries[i];
		}
	}
	if (!create) {
		return NULL;
	}
	for (int i = 0; i < MAX_KEYS; i++) {
		if (!entries[i].used) {
			entries[i].used = true;
			strncpy(entries[i].key, key, sizeof(entries[i].key) - 1);
			entries[i].key[sizeof(entries[i].key) - 1] = '\0';
			return &entries[i];
		}
	}
	abort();
}

void fake_settings_reset(void)
{
	memset(entries, 0, sizeof(entries));
	init_err = 0;
	load_err = 0;
	save_err = 0;
	saves = 0;
}

void fake_settings_put_raw(const char *key, const void *data, size_t len)
{
	struct entry *e = find(key, true);

	memcpy(e->data, data, len);
	e->len = len;
}

ssize_t fake_settings_get_raw(const char *key, void *data, size_t len)
{
	struct entry *e = find(key, false);

	if (!e) {
		return -ENOENT;
	}
	memcpy(data, e->data, MIN(len, e->len));
	return (ssize_t)e->len;
}

bool fake_settings_has(const char *key)
{
	return find(key, false) != NULL;
}

void fake_settings_set_init_error(int err)
{
	init_err = err;
}

void fake_settings_set_load_error(int err)
{
	load_err = err;
}

void fake_settings_set_save_error(int err)
{
	save_err = err;
}

int fake_settings_save_count(void)
{
	return saves;
}

int settings_subsys_init(void)
{
	return init_err;
}

ssize_t settings_load_one(const char *name, void *buf, size_t buf_len)
{
	struct entry *e;

	if (load_err) {
		return load_err;
	}
	e = find(name, false);
	if (!e) {
		return 0; /* CONFIG_SETTINGS_NVS: unknown key reads as 0 bytes */
	}
	memcpy(buf, e->data, MIN(buf_len, e->len));
	return (ssize_t)e->len;
}

int settings_save_one(const char *name, const void *value, size_t val_len)
{
	if (save_err) {
		return save_err;
	}
	saves++;
	fake_settings_put_raw(name, value, val_len);
	return 0;
}

int settings_delete(const char *name)
{
	struct entry *e = find(name, false);

	if (e) {
		e->used = false;
	}
	return 0;
}
