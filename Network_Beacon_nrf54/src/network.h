#ifndef NETWORK_H
#define NETWORK_H

#include <stdbool.h>
#include <stdint.h>

#define NETWORK_CONTACT_ENTRY_SIZE 5U

void network_evaluate_contact(uint8_t id, int8_t rssi, bool is_location_tag);
int network_init(void);
void network_command_begin(void);
void network_apply_command(uint8_t parameter, uint16_t value);
void network_command_commit(void);
int network_params_load(void);
int network_params_save(void);
int network_contact_export_begin(uint8_t *buffer, uint16_t buffer_len,
				 uint16_t *bytes_written);
int network_contact_export_commit(void);
void network_contact_export_abort(void);
int network_sync_contact_storage(void);
#if defined(CONFIG_DSA_DEV_SYNTHETIC_CONTACTS)
void network_development_validation_append_contact(uint8_t id,
						   uint32_t uptime_s,
						   uint8_t rssi);
#endif
int network_get_contact_count(uint32_t *count);

#endif /* NETWORK_H */
