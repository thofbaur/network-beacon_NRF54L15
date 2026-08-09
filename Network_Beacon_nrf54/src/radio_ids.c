#include "radio_ids.h"

const struct known_device known_device_table[] = {
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xB1, 0x7D, 0x76, 0x1a, 0x92, 0xd1 } }  // Developmentkit von Tobias (1057714900)
        },
        .id = 254,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC8, 0xDF, 0xFD, 0xA1, 0x8C, 0xC6 } }  // Developmentkit 1057727131
        },
        .id = 253,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xF4, 0x94, 0x73, 0x82, 0x91, 0xE7 } }  // Tag bei Tobias
        },
        .id = 1,
    },
{
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x04, 0xFF, 0x94, 0xFF, 0xD0, 0xD7 } }  // Tag
        },
        .id = 2,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA5, 0x17, 0xC7, 0xBC, 0xE2, 0xDF } }  // Tag
        },
        .id = 3,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xB7, 0x0B, 0x9C, 0xF6, 0x26, 0xEF } }  // Tag
        },
        .id = 4,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x70, 0xE3, 0xF5, 0xFE, 0x03, 0xDE } }  // Tag
        },
        .id = 5,
    },
};

const size_t known_device_table_len =
    sizeof(known_device_table) / sizeof(known_device_table[0]);
