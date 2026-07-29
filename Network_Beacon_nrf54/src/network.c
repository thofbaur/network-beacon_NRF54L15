#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "common_include.h"
#include "device.h"
#include "network.h"
#include "network_storage.h"
#include "param_storage.h"
#include "radio.h"
#include "storage_work_queue.h"

#define NETWORK_PARAMS_STORAGE_KEY "dsa/network"
#define NETWORK_PARAMS_STORED_SIZE 2U

struct network_params {
	uint8_t rssi_threshold;
	uint8_t tracking_active;
};

static struct network_params params_network;
static struct network_params command_old_params_network;
static bool command_batch_active;
static atomic_t tracking_active;

typedef struct {
	uint8_t     id;
	uint8_t     time[3];
	uint8_t     rssi;  // negative value, e.g., -80 dBm is stored as 80
} contact_entry;
#define CONTACT_ENTRY_SIZE 5





BUILD_ASSERT(CONFIG_DSA_NETWORK_RAM_FLUSH_THRESHOLD <=
	     CONFIG_DSA_NETWORK_CONTACT_RING_COUNT,
	     "Contact RAM flush threshold exceeds contact ring");

static contact_entry data_array[CONFIG_DSA_NETWORK_CONTACT_RING_COUNT];
static uint16_t idx_read = 0;
static uint16_t idx_write = 0;
static uint16_t contact_count = 0;
static bool contact_nvm_full;
enum contact_export_source {
	CONTACT_EXPORT_NONE,
	CONTACT_EXPORT_NVM,
	CONTACT_EXPORT_RAM,
};
static enum contact_export_source contact_export_source;
static uint16_t contact_export_bytes;
static bool contact_flush_active;
static uint16_t contact_flush_entries;
static uint16_t contact_flush_read_index;
static uint8_t contact_flush_block[NETWORK_STORAGE_BLOCK_DATA_LEN];
static K_MUTEX_DEFINE(contact_lock);

static void network_status_update_handler(struct k_work *work);
static void network_flush_handler(struct k_work *work);
static uint8_t contact_status_from_count(uint16_t number_dataset);

static K_WORK_DELAYABLE_DEFINE(network_status_update_work,
			       network_status_update_handler);
static K_WORK_DEFINE(network_flush_work, network_flush_handler);

static void contact_time_put(uint8_t time[3], uint32_t uptime_s)
{
	time[0] = (uptime_s >> 16) & 0xff;
	time[1] = (uptime_s >> 8) & 0xff;
	time[2] = uptime_s & 0xff;
}

static void contact_entry_write(uint8_t *buffer, const contact_entry *entry)
{
	buffer[0] = entry->id;
	buffer[1] = entry->time[0];
	buffer[2] = entry->time[1];
	buffer[3] = entry->time[2];
	buffer[4] = entry->rssi;
}

static void reset_parameters(void)
{
	params_network.rssi_threshold =
		CONFIG_DSA_NETWORK_DEFAULT_RSSI_THRESHOLD;
	params_network.tracking_active = 1U;
	atomic_set(&tracking_active, 1);
}


int network_init(void)
{
	int err;
	uint32_t contact_total;

	reset_parameters();
	storage_work_queue_init();

	err = network_storage_init();
	if (err) {
		printk("Failed to initialize network contact NVM storage (err %d)\n", err);
		return err;
	}

	err = network_params_load();
	if (err == -ENOENT) {
		printk("No stored network parameters, using defaults\n");
	} else if (err) {
		printk("Failed to load network parameters (err %d), using defaults\n", err);
	}

	err = network_get_contact_count(&contact_total);
	if (err) {
		printk("Failed to initialize network status (err %d)\n", err);
		return err;
	}
	device_set_network_status(contact_status_from_count(
		(uint16_t)MIN(contact_total, UINT16_MAX)));

	return 0;
}

void network_command_begin(void)
{
	command_old_params_network = params_network;
	command_batch_active = true;
}

void network_apply_command(uint8_t parameter, uint16_t value)
{
	switch (parameter) {
	case P_RSSI_NETWORK:
		if (value > UINT8_MAX) {
			printk("Rejecting invalid RSSI threshold value %u\n", value);
			return;
		}
		params_network.rssi_threshold = (uint8_t)value;
		printk("Network RSSI threshold set to -%u dBm\n", params_network.rssi_threshold);
		break;
	case P_NETWORK_RESET_PARAMS:
		reset_parameters();
		printk("Network parameters reset\n");
		break;
	case P_TRACKING_ACTIVE:
		if (value > 1U) {
			printk("Rejecting invalid tracking-active value %u\n", value);
			return;
		}
		params_network.tracking_active = (uint8_t)value;
		atomic_set(&tracking_active, value != 0U);
		printk("Network contact tracking %s\n",
		       value ? "enabled" : "disabled");
		break;
	default:
		printk("Unknown network parameter 0x%02x value %u\n", parameter, value);
		break;
	}
}

void network_command_commit(void)
{
	if (!command_batch_active) {
		return;
	}
	command_batch_active = false;

	if (memcmp(&command_old_params_network, &params_network,
		   sizeof(params_network)) != 0) {
		int err = network_params_save();

		if (err) {
			printk("Failed to save network parameters (err %d)\n", err);
		}
	}
}

int network_params_load(void)
{
	uint8_t stored[NETWORK_PARAMS_STORED_SIZE];
	uint8_t legacy_rssi;
	int err;

	err = param_storage_load(NETWORK_PARAMS_STORAGE_KEY, &stored,
				 sizeof(stored));
	if (!err) {
		if (stored[1] > 1U) {
			device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, true);
			return -EBADMSG;
		}
		params_network.rssi_threshold = stored[0];
		params_network.tracking_active = stored[1];
		atomic_set(&tracking_active, stored[1] != 0U);
		device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, false);
		return 0;
	}
	if (err == -ENOENT) {
		device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, false);
		return err;
	}

	/* Migrate the previous versioned record, which only contained RSSI. */
	err = param_storage_load(NETWORK_PARAMS_STORAGE_KEY, &legacy_rssi,
				 sizeof(legacy_rssi));
	if (!err) {
		params_network.rssi_threshold = legacy_rssi;
		params_network.tracking_active = 1U;
		atomic_set(&tracking_active, 1);
		err = network_params_save();
		if (!err) {
			printk("Migrated network parameters with tracking enabled\n");
		}
		device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, err != 0);
		return err;
	}

	err = param_storage_load_legacy(NETWORK_PARAMS_STORAGE_KEY,
					&legacy_rssi, sizeof(legacy_rssi));
	if (err) {
		device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, true);
		return err;
	}

	params_network.rssi_threshold = legacy_rssi;
	params_network.tracking_active = 1U;
	atomic_set(&tracking_active, 1);
	err = network_params_save();
	if (!err) {
		printk("Migrated network parameters to versioned storage\n");
	}
	device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, err != 0);
	return err;
}

int network_params_save(void)
{
	uint8_t stored[NETWORK_PARAMS_STORED_SIZE] = {
		params_network.rssi_threshold,
		params_network.tracking_active,
	};

	int err = param_storage_save(NETWORK_PARAMS_STORAGE_KEY,
				    &stored, sizeof(stored));

	device_set_storage_fault(STORAGE_FAULT_NETWORK_PARAMS, err != 0);
	return err;
}

static uint8_t contact_status_from_count(uint16_t number_dataset)
{
	if (number_dataset > DATA_LEVEL_7) {
		return 7 << P_SHIFT_STATUS_DATA;
	}
	if (number_dataset > DATA_LEVEL_6) {
		return 6 << P_SHIFT_STATUS_DATA;
	}
	if (number_dataset > DATA_LEVEL_5) {
		return 5 << P_SHIFT_STATUS_DATA;
	}
	if (number_dataset > DATA_LEVEL_4) {
		return 4 << P_SHIFT_STATUS_DATA;
	}
	if (number_dataset > DATA_LEVEL_3) {
		return 3 << P_SHIFT_STATUS_DATA;
	}
	if (number_dataset > DATA_LEVEL_2) {
		return 2 << P_SHIFT_STATUS_DATA;
	}
	if (number_dataset > DATA_LEVEL_1) {
		return 1 << P_SHIFT_STATUS_DATA;
	}

	return 0;
}

static void network_schedule_tag_update_once(k_timeout_t delay)
{
	if (k_work_delayable_is_pending(&network_status_update_work)) {
		return;
	}

	k_work_reschedule(&network_status_update_work, delay);
}

static void network_update_tag(void)
{
	k_work_reschedule(&network_status_update_work, K_NO_WAIT);
}

static void network_schedule_flush_if_needed(void)
{
	bool flush_needed;

	k_mutex_lock(&contact_lock, K_FOREVER);
	flush_needed = !contact_nvm_full &&
		       !contact_flush_active &&
		       contact_export_source == CONTACT_EXPORT_NONE &&
		       contact_count >=
			       CONFIG_DSA_NETWORK_RAM_FLUSH_THRESHOLD;
	k_mutex_unlock(&contact_lock);

	if (flush_needed) {
		storage_work_submit(&network_flush_work);
	}
}

static void network_flush_handler(struct k_work *work)
{
	uint16_t entries_to_flush;
	uint16_t bytes_to_flush;
	uint16_t read_index;
	int err;

	ARG_UNUSED(work);

	k_mutex_lock(&contact_lock, K_FOREVER);

	if (contact_flush_active ||
	    contact_export_source != CONTACT_EXPORT_NONE ||
	    contact_count < CONFIG_DSA_NETWORK_RAM_FLUSH_THRESHOLD) {
		k_mutex_unlock(&contact_lock);
		return;
	}

	entries_to_flush = MIN((uint16_t)NETWORK_STORAGE_BLOCK_CONTACTS,
			       contact_count);
	bytes_to_flush = entries_to_flush * CONTACT_ENTRY_SIZE;
	read_index = idx_read;

	for (uint16_t i = 0; i < entries_to_flush; i++) {
		contact_entry_write(&contact_flush_block[i * CONTACT_ENTRY_SIZE],
				    &data_array[read_index]);
		read_index = (read_index + 1) %
			     CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;
	}

	contact_flush_active = true;
	contact_flush_entries = entries_to_flush;
	contact_flush_read_index = idx_read;
	k_mutex_unlock(&contact_lock);

	err = network_storage_append_block(contact_flush_block, bytes_to_flush);

	k_mutex_lock(&contact_lock, K_FOREVER);
	if (err) {
		if (err == -ENOSPC) {
			contact_nvm_full = true;
		}
		contact_flush_active = false;
		contact_flush_entries = 0;
		k_mutex_unlock(&contact_lock);
		if (err == -ENOSPC) {
			printk("Contact NVM storage full; keeping contacts in RAM\n");
		}
		printk("Failed to flush contacts to NVM (err %d)\n", err);
		return;
	}

	if (idx_read != contact_flush_read_index ||
	    contact_count < contact_flush_entries) {
		contact_flush_active = false;
		contact_flush_entries = 0;
		k_mutex_unlock(&contact_lock);
		printk("Contact RAM changed during reserved flash flush\n");
		return;
	}

	idx_read = (idx_read + contact_flush_entries) %
		   CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;
	contact_count -= contact_flush_entries;
	contact_flush_active = false;
	contact_flush_entries = 0;

	k_mutex_unlock(&contact_lock);
	network_update_tag();
	network_schedule_flush_if_needed();
}

static void network_status_update_handler(struct k_work *work)
{
	int err;
	uint32_t number_dataset;

	ARG_UNUSED(work);

	err = network_get_contact_count(&number_dataset);
	if (err) {
		printk("Failed to count stored contacts (err %d)\n", err);
		return;
	}
	if (number_dataset > UINT16_MAX) {
		number_dataset = UINT16_MAX;
	}

	device_set_network_status(contact_status_from_count((uint16_t)number_dataset));
	err = adv_update();
	if (err) {
		printk("Failed to update network status advertising data (err %d)\n", err);
	}
}

void network_evaluate_contact(uint8_t id, int8_t rssi)
{
    uint8_t rssi_magnitude = (uint8_t)(-(int16_t)rssi);

    if (!atomic_get(&tracking_active)) {
	    return;
    }

    if (rssi_magnitude <= params_network.rssi_threshold)
    {
		k_mutex_lock(&contact_lock, K_FOREVER);
		if (contact_count == CONFIG_DSA_NETWORK_CONTACT_RING_COUNT &&
		    (contact_export_source != CONTACT_EXPORT_NONE ||
		     contact_flush_active)) {
			k_mutex_unlock(&contact_lock);
			printk("Contact RAM full during export; dropping newest contact\n");
			return;
		}

		data_array[idx_write].id = id;
		contact_time_put(data_array[idx_write].time, (uint32_t)k_uptime_seconds());
		data_array[idx_write].rssi = rssi_magnitude;
		idx_write = (idx_write + 1) %
			    CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;

		if (contact_count == CONFIG_DSA_NETWORK_CONTACT_RING_COUNT) {
			idx_read = (idx_read + 1) %
				   CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;
		} else {
			contact_count++;
		}
		k_mutex_unlock(&contact_lock);
		network_schedule_flush_if_needed();

		network_schedule_tag_update_once(
			K_MSEC(CONFIG_DSA_NETWORK_STATUS_UPDATE_DELAY_MS));
    }
}

#if defined(CONFIG_DSA_DEV_SYNTHETIC_CONTACTS)
void network_dev_append_contact(uint8_t id, uint32_t uptime_s, uint8_t rssi)
{
	if (!atomic_get(&tracking_active)) {
		return;
	}

	k_mutex_lock(&contact_lock, K_FOREVER);
	if (contact_count == CONFIG_DSA_NETWORK_CONTACT_RING_COUNT &&
	    (contact_export_source != CONTACT_EXPORT_NONE ||
	     contact_flush_active)) {
		k_mutex_unlock(&contact_lock);
		return;
	}

	data_array[idx_write].id = id;
	contact_time_put(data_array[idx_write].time, uptime_s);
	data_array[idx_write].rssi = rssi;
	idx_write = (idx_write + 1) %
		    CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;

	if (contact_count == CONFIG_DSA_NETWORK_CONTACT_RING_COUNT) {
		idx_read = (idx_read + 1) %
			   CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;
	} else {
		contact_count++;
	}
	k_mutex_unlock(&contact_lock);
	network_schedule_flush_if_needed();

	network_schedule_tag_update_once(
		K_MSEC(CONFIG_DSA_NETWORK_STATUS_UPDATE_DELAY_MS));
}
#endif

int network_contact_export_begin(uint8_t *buffer, uint16_t buffer_len,
				 uint16_t *bytes_written)
{
	uint16_t written = 0;
	uint16_t read_index;
	uint16_t entries_available;
	uint32_t nvm_contacts;
	int err;

	if (!buffer || !bytes_written) {
		return -EINVAL;
	}

	buffer_len -= buffer_len % CONTACT_ENTRY_SIZE;
	*bytes_written = 0;
	if (buffer_len == 0) {
		return 0;
	}

	k_mutex_lock(&contact_lock, K_FOREVER);

	if (contact_export_source != CONTACT_EXPORT_NONE ||
	    contact_flush_active) {
		k_mutex_unlock(&contact_lock);
		return -EBUSY;
	}

	err = network_storage_get_contact_count(&nvm_contacts);
	if (err) {
		k_mutex_unlock(&contact_lock);
		return err;
	}
	if (nvm_contacts > 0) {
		err = network_storage_peek(buffer, buffer_len, &written);
		if (err) {
			k_mutex_unlock(&contact_lock);
			return err;
		}
		if (written == 0) {
			k_mutex_unlock(&contact_lock);
			return -EIO;
		}
		contact_export_source = CONTACT_EXPORT_NVM;
	} else {
		read_index = idx_read;
		entries_available = contact_count;

		while (entries_available > 0 &&
		       (buffer_len - written) >= CONTACT_ENTRY_SIZE) {
			contact_entry_write(&buffer[written], &data_array[read_index]);
			written += CONTACT_ENTRY_SIZE;

			read_index = (read_index + 1) %
				     CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;
			entries_available--;
		}

		if (written > 0) {
			contact_export_source = CONTACT_EXPORT_RAM;
		}
	}

	contact_export_bytes = written;
	*bytes_written = written;
	k_mutex_unlock(&contact_lock);

	return 0;
}

int network_contact_export_commit(void)
{
	enum contact_export_source source;
	uint16_t exported_bytes;
	uint16_t entries_to_drop;
	int err = 0;

	k_mutex_lock(&contact_lock, K_FOREVER);
	source = contact_export_source;
	exported_bytes = contact_export_bytes;

	switch (source) {
	case CONTACT_EXPORT_NVM:
		/* Keep the export reservation active, but release the RAM contact
		 * lock while NVS performs flash writes and erases.
		 */
		k_mutex_unlock(&contact_lock);
		err = network_storage_drop(exported_bytes);
		k_mutex_lock(&contact_lock, K_FOREVER);
		if (contact_export_source != source ||
		    contact_export_bytes != exported_bytes) {
			err = -EIO;
			break;
		}
		if (!err) {
			contact_nvm_full = false;
		}
		break;
	case CONTACT_EXPORT_RAM:
		entries_to_drop = exported_bytes / CONTACT_ENTRY_SIZE;
		if (entries_to_drop > contact_count) {
			err = -EIO;
			break;
		}
		idx_read = (idx_read + entries_to_drop) %
			   CONFIG_DSA_NETWORK_CONTACT_RING_COUNT;
		contact_count -= entries_to_drop;
		break;
	case CONTACT_EXPORT_NONE:
	default:
		err = -EINVAL;
		break;
	}

	contact_export_source = CONTACT_EXPORT_NONE;
	contact_export_bytes = 0;
	k_mutex_unlock(&contact_lock);

	if (!err) {
		network_update_tag();
		network_schedule_flush_if_needed();
	}

	return err;
}

void network_contact_export_abort(void)
{
	k_mutex_lock(&contact_lock, K_FOREVER);
	contact_export_source = CONTACT_EXPORT_NONE;
	contact_export_bytes = 0;
	k_mutex_unlock(&contact_lock);

	network_schedule_flush_if_needed();
}

int network_sync_contact_storage(void)
{
	return network_storage_sync();
}

int network_get_contact_count(uint32_t *count)
{
	uint32_t stored_count;
	int err;

	if (!count) {
		return -EINVAL;
	}

	err = network_storage_get_contact_count(&stored_count);
	if (err) {
		return err;
	}

	k_mutex_lock(&contact_lock, K_FOREVER);
	*count = stored_count + contact_count;
	k_mutex_unlock(&contact_lock);

	return 0;
}
