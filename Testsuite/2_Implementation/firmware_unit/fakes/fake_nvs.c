#include <stdio.h>
#include <stdlib.h>

#include <zephyr/sys/util.h>

#include "fake_nvs.h"

#define MAX_PARTITIONS 8
#define NVS_ATE_OVERHEAD 64 /* 4 ATEs of 16 bytes (nRF54L RRAM write block) */

struct record {
	uint16_t id;
	size_t len;
	uint8_t *data;
};

struct partition {
	bool used;
	off_t offset;
	bool corrupt;
	struct record *records;
	size_t count;
	size_t capacity;
};

static struct partition partitions[MAX_PARTITIONS];
static int cut_after = -1;
static bool power_cut;
static int read_err;
static int read_err_id = -1;
static int write_err;
static int delete_err;
static int writes;
static int deletes;
static int mounts;
static int clears;

static struct partition *find_partition(off_t offset, bool create)
{
	for (int i = 0; i < MAX_PARTITIONS; i++) {
		if (partitions[i].used && partitions[i].offset == offset) {
			return &partitions[i];
		}
	}
	if (!create) {
		return NULL;
	}
	for (int i = 0; i < MAX_PARTITIONS; i++) {
		if (!partitions[i].used) {
			memset(&partitions[i], 0, sizeof(partitions[i]));
			partitions[i].used = true;
			partitions[i].offset = offset;
			return &partitions[i];
		}
	}
	fprintf(stderr, "fake_nvs: too many partitions\n");
	abort();
}

static struct record *find_record(struct partition *p, uint16_t id)
{
	for (size_t i = 0; p && i < p->count; i++) {
		if (p->records[i].id == id) {
			return &p->records[i];
		}
	}
	return NULL;
}

static void remove_record(struct partition *p, uint16_t id)
{
	for (size_t i = 0; p && i < p->count; i++) {
		if (p->records[i].id == id) {
			free(p->records[i].data);
			p->records[i] = p->records[p->count - 1];
			p->count--;
			return;
		}
	}
}

static void wipe_partition(struct partition *p)
{
	for (size_t i = 0; i < p->count; i++) {
		free(p->records[i].data);
	}
	free(p->records);
	p->records = NULL;
	p->count = 0;
	p->capacity = 0;
	p->corrupt = false;
}

static void store_record(struct partition *p, uint16_t id, const void *data,
			 size_t len)
{
	struct record *r = find_record(p, id);

	if (!r) {
		if (p->count == p->capacity) {
			p->capacity = p->capacity ? p->capacity * 2 : 16;
			p->records = realloc(p->records,
					     p->capacity * sizeof(*p->records));
		}
		r = &p->records[p->count++];
		r->id = id;
		r->data = NULL;
	}
	free(r->data);
	r->data = malloc(len ? len : 1);
	memcpy(r->data, data, len);
	r->len = len;
}

/* Returns true when this mutating operation must fail because of a
 * simulated power cut.
 */
static bool consume_power(void)
{
	if (power_cut) {
		return true;
	}
	if (cut_after == 0) {
		power_cut = true;
		cut_after = -1;
		return true;
	}
	if (cut_after > 0) {
		cut_after--;
	}
	return false;
}

void fake_nvs_reset_all(void)
{
	for (int i = 0; i < MAX_PARTITIONS; i++) {
		if (partitions[i].used) {
			wipe_partition(&partitions[i]);
			partitions[i].used = false;
		}
	}
	cut_after = -1;
	power_cut = false;
	read_err = 0;
	read_err_id = -1;
	write_err = 0;
	delete_err = 0;
	writes = 0;
	deletes = 0;
	mounts = 0;
	clears = 0;
}

void fake_nvs_cut_power_after(int writes_before_cut)
{
	cut_after = writes_before_cut;
	power_cut = false;
}

void fake_nvs_power_cycle(void)
{
	cut_after = -1;
	power_cut = false;
}

bool fake_nvs_power_is_cut(void)
{
	return power_cut;
}

void fake_nvs_set_read_error(int err, int id)
{
	read_err = err;
	read_err_id = id;
}

void fake_nvs_set_write_error(int err)
{
	write_err = err;
}

void fake_nvs_set_delete_error(int err)
{
	delete_err = err;
}

void fake_nvs_corrupt_partition(off_t offset)
{
	find_partition(offset, true)->corrupt = true;
}

void fake_nvs_put_raw(off_t offset, uint16_t id, const void *data, size_t len)
{
	store_record(find_partition(offset, true), id, data, len);
}

ssize_t fake_nvs_get_raw(off_t offset, uint16_t id, void *data, size_t len)
{
	struct record *r = find_record(find_partition(offset, false), id);

	if (!r) {
		return -ENOENT;
	}
	memcpy(data, r->data, MIN(len, r->len));
	return (ssize_t)r->len;
}

bool fake_nvs_has(off_t offset, uint16_t id)
{
	return find_record(find_partition(offset, false), id) != NULL;
}

size_t fake_nvs_record_count(off_t offset)
{
	struct partition *p = find_partition(offset, false);

	return p ? p->count : 0;
}

int fake_nvs_write_count(void)
{
	return writes;
}

int fake_nvs_delete_count(void)
{
	return deletes;
}

int fake_nvs_mount_count(void)
{
	return mounts;
}

int fake_nvs_clear_count(void)
{
	return clears;
}

void fake_nvs_erase_range(off_t offset, size_t size)
{
	for (int i = 0; i < MAX_PARTITIONS; i++) {
		if (partitions[i].used && partitions[i].offset >= offset &&
		    partitions[i].offset < offset + (off_t)size) {
			wipe_partition(&partitions[i]);
		}
	}
}

/* ---- NVS API ----------------------------------------------------------- */
int nvs_mount(struct nvs_fs *fs)
{
	struct partition *p;

	mounts++;
	if (fs->sector_count < 2 || fs->sector_size == 0) {
		return -EINVAL;
	}
	p = find_partition(fs->offset, true);
	if (p->corrupt) {
		return -EDEADLK;
	}
	fs->ready = true;
	return 0;
}

int nvs_clear(struct nvs_fs *fs)
{
	clears++;
	if (!fs->ready) {
		return -EACCES;
	}
	if (consume_power()) {
		return -EIO;
	}
	wipe_partition(find_partition(fs->offset, true));
	/* Zephyr: "nvs needs to be reinitialized after clearing". */
	fs->ready = false;
	return 0;
}

ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len)
{
	struct partition *p;
	struct record *r;

	if (!fs->ready) {
		return -EACCES;
	}
	if (len > (size_t)fs->sector_size - NVS_ATE_OVERHEAD ||
	    (len > 0 && data == NULL)) {
		return -EINVAL;
	}
	p = find_partition(fs->offset, true);
	r = find_record(p, id);
	if (r && r->len == len && memcmp(r->data, data, len) == 0) {
		return 0; /* identical data: nothing written */
	}
	if (write_err) {
		return write_err;
	}
	if (consume_power()) {
		return -EIO;
	}
	writes++;
	store_record(p, id, data, len);
	return (ssize_t)len;
}

int nvs_delete(struct nvs_fs *fs, uint16_t id)
{
	struct partition *p;

	if (!fs->ready) {
		return -EACCES;
	}
	if (delete_err) {
		return delete_err;
	}
	p = find_partition(fs->offset, true);
	if (!find_record(p, id)) {
		return 0; /* Zephyr: deleting a missing id is a no-op */
	}
	if (consume_power()) {
		return -EIO;
	}
	deletes++;
	remove_record(p, id);
	return 0;
}

ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len)
{
	struct record *r;

	if (!fs->ready) {
		return -EACCES;
	}
	if (read_err && (read_err_id < 0 || read_err_id == id)) {
		return read_err;
	}
	r = find_record(find_partition(fs->offset, true), id);
	if (!r) {
		return -ENOENT;
	}
	memcpy(data, r->data, MIN(len, r->len));
	return (ssize_t)r->len;
}
