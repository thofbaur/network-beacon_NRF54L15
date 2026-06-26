#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "defines.h"
#include "device.h"
#include "network.h"
#include "network_storage.h"
#include "param_storage.h"
#include "radio.h"

#define NETWORK_PARAMS_STORAGE_KEY "dsa/network"

struct network_params {
	uint8_t 	rssi_threshold;	
};

static struct network_params params_network;

typedef struct {
	uint8_t     id;
	uint8_t     time[3];
	uint8_t     rssi;  // negative value, e.g., -80 dBm is stored as 80
} contact_entry;
#define CONTACT_ENTRY_SIZE 5





#define LENGTH_DATA_BUFFER 26000

#define NETWORK_LIMIT_RSSI		80 // approx. 1-2m distance
#define NETWORK_STATUS_UPDATE_DELAY_MS	300
#define NETWORK_RAM_CONTACT_LIMIT	25000 /* TODO: tune RAM-to-NVM flush threshold. */

#define DATA_LEVEL_1	0
#define DATA_LEVEL_2	1
#define DATA_LEVEL_3	4   // TODO 10
#define DATA_LEVEL_4	16   // TODO 100
#define DATA_LEVEL_5	32 // TODO 500
#define DATA_LEVEL_6	64 // TODO
#define DATA_LEVEL_7	256 // TODO xxx

#define P_SHIFT_STATUS_DATA 5
static contact_entry	data_array[LENGTH_DATA_BUFFER]; // ID 1 Byte; time 3 Byte; RSSI 1 Byte
static uint16_t idx_read = 0;
static uint16_t idx_write = 0;
static uint16_t contact_count = 0;
static K_MUTEX_DEFINE(contact_lock);

static void network_status_update_handler(struct k_work *work);
static void network_flush_handler(struct k_work *work);

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
	params_network.rssi_threshold = NETWORK_LIMIT_RSSI;

}


void network_init(void)
{
	int err;

	reset_parameters();
	err = network_storage_init();
	if (err) {
		printk("Failed to initialize network contact NVM storage (err %d)\n", err);
	}

	err = network_params_load();
	if (err == -ENOENT) {
		printk("No stored network parameters, using defaults\n");
	} else if (err) {
		printk("Failed to load network parameters (err %d), using defaults\n", err);
	}
}

void network_apply_command(uint8_t parameter, uint16_t value)
{
	struct network_params old_params_network = params_network;

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
		printk("Network tracking command value %u not implemented\n", value);
		break;
	default:
		printk("Unknown network parameter 0x%02x value %u\n", parameter, value);
		break;
	}

	if (memcmp(&old_params_network, &params_network, sizeof(params_network)) != 0) {
		int err = network_params_save();

		if (err) {
			printk("Failed to save network parameters (err %d)\n", err);
		}
	}
}

int network_params_load(void)
{
	return param_storage_load(NETWORK_PARAMS_STORAGE_KEY,
				  &params_network, sizeof(params_network));
}

int network_params_save(void)
{
	return param_storage_save(NETWORK_PARAMS_STORAGE_KEY,
				  &params_network, sizeof(params_network));
}

static bool scan_extract_id(struct bt_data *data, void *user_data)
{
    uint8_t *id = user_data;
    
    if (data->type == BT_DATA_MANUFACTURER_DATA)
    {
        data->data_len >= 1 ? (*id = data->data[0]) : (*id = 0);
        
        return false;
    }

    return true;
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
	flush_needed = contact_count >= NETWORK_RAM_CONTACT_LIMIT;
	k_mutex_unlock(&contact_lock);

	if (flush_needed) {
		k_work_submit(&network_flush_work);
	}
}

static void network_flush_handler(struct k_work *work)
{
	uint8_t block[NETWORK_STORAGE_BLOCK_DATA_LEN];
	uint16_t entries_to_flush;
	uint16_t bytes_to_flush;
	uint16_t read_index;
	int err;

	ARG_UNUSED(work);

	while (true) {
		k_mutex_lock(&contact_lock, K_FOREVER);

		if (contact_count < NETWORK_RAM_CONTACT_LIMIT) {
			k_mutex_unlock(&contact_lock);
			return;
		}

		entries_to_flush = MIN((uint16_t)NETWORK_STORAGE_BLOCK_CONTACTS,
				       contact_count);
		bytes_to_flush = entries_to_flush * CONTACT_ENTRY_SIZE;
		read_index = idx_read;

		for (uint16_t i = 0; i < entries_to_flush; i++) {
			contact_entry_write(&block[i * CONTACT_ENTRY_SIZE],
					    &data_array[read_index]);
			read_index = (read_index + 1) % LENGTH_DATA_BUFFER;
		}

		/* Keep the lock until the write succeeds so RAM data is not dropped
		 * or duplicated while the block is being persisted.
		 */
		err = network_storage_append_block(block, bytes_to_flush);
		if (err) {
			printk("Failed to flush contacts to NVM (err %d)\n", err);
			k_mutex_unlock(&contact_lock);
			return;
		}

		idx_read = (idx_read + entries_to_flush) % LENGTH_DATA_BUFFER;
		contact_count -= entries_to_flush;

		k_mutex_unlock(&contact_lock);
		network_update_tag();
	}
}

static void network_status_update_handler(struct k_work *work)
{
	int err;
	uint32_t number_dataset;

	ARG_UNUSED(work);

	k_mutex_lock(&contact_lock, K_FOREVER);
	number_dataset = contact_count;
	k_mutex_unlock(&contact_lock);
	number_dataset += network_storage_pending_bytes() / CONTACT_ENTRY_SIZE;
	if (number_dataset > UINT16_MAX) {
		number_dataset = UINT16_MAX;
	}

	device_set_network_status(contact_status_from_count((uint16_t)number_dataset));
	err = adv_update();
	if (err) {
		printk("Failed to update network status advertising data (err %d)\n", err);
	}
}

void network_evaluate_contact(const bt_addr_le_t *addr, 
    int8_t rssi, uint8_t adv_type, struct net_buf_simple *buf)
{
    struct net_buf_simple ad_temp;
    uint8_t id = 0;
    uint8_t rssi_magnitude = (uint8_t)(-(int16_t)rssi);

    if (rssi_magnitude <= params_network.rssi_threshold)
    {
        
        net_buf_simple_clone(buf, &ad_temp);
	    bt_data_parse(&ad_temp, scan_extract_id, &id);
		
		k_mutex_lock(&contact_lock, K_FOREVER);
		data_array[idx_write].id = id;
		contact_time_put(data_array[idx_write].time, (uint32_t)k_uptime_seconds());
		data_array[idx_write].rssi = rssi_magnitude;
		idx_write = (idx_write + 1) % LENGTH_DATA_BUFFER;

		if (contact_count == LENGTH_DATA_BUFFER) {
			idx_read = (idx_read + 1) % LENGTH_DATA_BUFFER;
		} else {
			contact_count++;
		}
		k_mutex_unlock(&contact_lock);
		network_schedule_flush_if_needed();

		network_schedule_tag_update_once(K_MSEC(NETWORK_STATUS_UPDATE_DELAY_MS));
    }
}

/* TODO Remove: development-only helper for synthetic contact buffer data. */
void network_dev_append_contact(uint8_t id, uint32_t uptime_s, uint8_t rssi)
{
	k_mutex_lock(&contact_lock, K_FOREVER);
	data_array[idx_write].id = id;
	contact_time_put(data_array[idx_write].time, uptime_s);
	data_array[idx_write].rssi = rssi;
	idx_write = (idx_write + 1) % LENGTH_DATA_BUFFER;

	if (contact_count == LENGTH_DATA_BUFFER) {
		idx_read = (idx_read + 1) % LENGTH_DATA_BUFFER;
	} else {
		contact_count++;
	}
	k_mutex_unlock(&contact_lock);
	network_schedule_flush_if_needed();

	network_schedule_tag_update_once(K_MSEC(NETWORK_STATUS_UPDATE_DELAY_MS));
}

uint16_t network_read_contact(uint8_t *buffer, uint16_t buffer_len)
{
	uint16_t bytes_written;

	bytes_written = network_peek_contact(buffer, buffer_len);
	network_drop_contact_bytes(bytes_written);

	return bytes_written;
}

uint16_t network_peek_contact(uint8_t *buffer, uint16_t buffer_len)
{
	uint16_t bytes_written = 0;
	uint16_t read_index;
	uint16_t entries_available;

	bytes_written = network_storage_peek(buffer, buffer_len);
	if ((buffer_len - bytes_written) < CONTACT_ENTRY_SIZE) {
		return bytes_written;
	}
	
	k_mutex_lock(&contact_lock, K_FOREVER);

	read_index = idx_read;
	entries_available = contact_count;

	while (entries_available > 0)
	{
		if ((buffer_len - bytes_written) < CONTACT_ENTRY_SIZE) {
			break;
		}

		contact_entry_write(&buffer[bytes_written], &data_array[read_index]);
		bytes_written += CONTACT_ENTRY_SIZE;

		read_index = (read_index + 1) % LENGTH_DATA_BUFFER;
		entries_available--;
	}

	k_mutex_unlock(&contact_lock);

	return bytes_written;
}

void network_drop_contact_bytes(uint16_t bytes_to_drop)
{
	uint16_t nvm_pending;
	uint16_t nvm_to_drop;
	uint16_t nvm_dropped;
	uint16_t entries_to_drop;
	bool entries_removed = false;

	k_mutex_lock(&contact_lock, K_FOREVER);

	nvm_pending = network_storage_pending_bytes();
	nvm_to_drop = MIN(bytes_to_drop, nvm_pending);
	nvm_dropped = network_storage_drop(nvm_to_drop);

	if (nvm_dropped < nvm_to_drop) {
		k_mutex_unlock(&contact_lock);
		if (nvm_dropped > 0) {
			network_update_tag();
		}
		return;
	}

	bytes_to_drop -= nvm_dropped;
	entries_to_drop = bytes_to_drop / CONTACT_ENTRY_SIZE;

	while (entries_to_drop > 0 && contact_count > 0) {
		idx_read = (idx_read + 1) % LENGTH_DATA_BUFFER;
		contact_count--;
		entries_to_drop--;
		entries_removed = true;
	}

	k_mutex_unlock(&contact_lock);

	if (entries_removed) {
		network_update_tag();
	} else if (nvm_dropped > 0) {
		network_update_tag();
	}
}
