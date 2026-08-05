#ifndef MESSAGE_PARSER_H_
#define MESSAGE_PARSER_H_

#include <stdint.h>

typedef void (*message_parser_finished_cb_t)(void);

void message_parser_init(message_parser_finished_cb_t finished_cb);
void message_parser_reset(void);
void message_parser_feed(uint8_t beacon_id, const uint8_t *data, uint16_t len);

#endif /* MESSAGE_PARSER_H_ */
