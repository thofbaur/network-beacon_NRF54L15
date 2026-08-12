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
            .a = { .val = { 0xEB, 0xD5, 0x00, 0xBA, 0x1A, 0xE2 } }  // Developmentkit 1057743183
        },
        .id = 252,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x97, 0x57, 0xAF, 0xEF, 0xE9, 0xF7 } }  // Tag
        },
        .id = 1,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x87, 0x4F, 0xB1, 0x46, 0x1D, 0xF1 } }  // Tag
        },
        .id = 2,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x25, 0x96, 0x65, 0x79, 0xD7, 0xC8 } }  // Tag
        },
        .id = 3,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x84, 0x0A, 0xD6, 0x9D, 0xE8, 0xEE } }  // Tag
        },
        .id = 4,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x1A, 0x72, 0xFA, 0xFF, 0x01, 0xFE } }  // Tag
        },
        .id = 5,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xED, 0xDC, 0xED, 0x94, 0x66, 0xFF } }  // Tag
        },
        .id = 6,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xBC, 0xA8, 0x9F, 0xF8, 0x83, 0xDD } }  // Tag
        },
        .id = 7,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC0, 0x4E, 0x22, 0xB5, 0xA2, 0xDE } }  // Tag
        },
        .id = 8,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xE0, 0xD1, 0x66, 0x6B, 0xAE, 0xD8 } }  // Tag
        },
        .id = 9,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x57, 0x8D, 0xB8, 0x73, 0x3C, 0xF9 } }  // Tag
        },
        .id = 10,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x62, 0x02, 0xC5, 0xA5, 0xE7, 0xC4 } }  // Tag
        },
        .id = 11,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC4, 0x41, 0x3C, 0x9E, 0x6F, 0xC3 } }  // Tag
        },
        .id = 12,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x30, 0xC0, 0x52, 0xCD, 0xD5, 0xE2 } }  // Tag
        },
        .id = 13,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC6, 0xA0, 0x24, 0x2D, 0x48, 0xEF } }  // Tag
        },
        .id = 14,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x25, 0x22, 0xA9, 0x82, 0x9A, 0xDC } }  // Tag
        },
        .id = 15,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x41, 0x2C, 0xF5, 0xA7, 0xDA, 0xD1 } }  // Tag
        },
        .id = 16,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x2C, 0x3C, 0xB5, 0xEE, 0x74, 0xD4 } }  // Tag
        },
        .id = 17,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x5B, 0x51, 0xD8, 0xB1, 0xA3, 0xCC } }  // Tag
        },
        .id = 18,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x0B, 0x37, 0x61, 0xB1, 0x49, 0xD4 } }  // Tag
        },
        .id = 19,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x9E, 0x00, 0x41, 0xA1, 0xCC, 0xE2 } }  // Tag
        },
        .id = 20,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xF8, 0x4F, 0xDC, 0xF1, 0xD4, 0xD3 } }  // Tag
        },
        .id = 21,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x52, 0x89, 0xCC, 0xF5, 0x94, 0xDD } }  // Tag
        },
        .id = 22,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x00, 0xB5, 0x1F, 0xF2, 0x84, 0xCB } }  // Tag
        },
        .id = 23,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x63, 0x7E, 0x77, 0xF9, 0xEE, 0xCC } }  // Tag
        },
        .id = 24,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xFA, 0x1F, 0xFE, 0x56, 0x01, 0xDC } }  // Tag
        },
        .id = 25,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x08, 0xF4, 0x02, 0xD4, 0x28, 0xE8 } }  // Tag
        },
        .id = 26,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x9F, 0xF9, 0xF0, 0x45, 0x7E, 0xF3 } }  // Tag
        },
        .id = 27,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x95, 0xB7, 0x8E, 0x1C, 0xDC, 0xFC } }  // Tag
        },
        .id = 28,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x12, 0xC1, 0x8A, 0xE6, 0xB8, 0xE2 } }  // Tag
        },
        .id = 29,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xB3, 0xC3, 0x39, 0x49, 0x2B, 0xC4 } }  // Tag
        },
        .id = 30,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x7A, 0xDC, 0xEC, 0x56, 0x75, 0xEE } }  // Tag
        },
        .id = 31,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x70, 0xFA, 0x96, 0xFA, 0x5E, 0xC2 } }  // Tag
        },
        .id = 32,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x76, 0x23, 0xF3, 0xC6, 0xA0, 0xD4 } }  // Tag
        },
        .id = 33,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x4D, 0x9B, 0x0C, 0x98, 0x32, 0xC2 } }  // Tag
        },
        .id = 34,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x85, 0xA2, 0xFA, 0x5C, 0x37, 0xD8 } }  // Tag
        },
        .id = 35,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x12, 0xDA, 0x71, 0xF5, 0xAF, 0xCA } }  // Tag
        },
        .id = 36,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x7C, 0xB4, 0xAF, 0xDD, 0x9A, 0xD3 } }  // Tag
        },
        .id = 37,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x12, 0x5F, 0xA8, 0xE3, 0x32, 0xE5 } }  // Tag
        },
        .id = 38,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xDB, 0x84, 0x18, 0x07, 0x66, 0xC6 } }  // Tag
        },
        .id = 39,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x2D, 0x1F, 0x96, 0x63, 0x8B, 0xC5 } }  // Tag
        },
        .id = 40,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x16, 0xA7, 0xC2, 0xE8, 0x19, 0xCE } }  // Tag
        },
        .id = 41,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA2, 0x24, 0xB8, 0xE0, 0x6C, 0xE3 } }  // Tag
        },
        .id = 42,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA1, 0x51, 0xCD, 0xFF, 0x9C, 0xDB } }  // Tag
        },
        .id = 43,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA8, 0x2F, 0x4B, 0xFE, 0x2B, 0xD1 } }  // Tag
        },
        .id = 44,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x10, 0x0D, 0xBB, 0x0C, 0x25, 0xEC } }  // Tag
        },
        .id = 45,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x64, 0xEA, 0xD0, 0xF7, 0xE4, 0xE9 } }  // Tag
        },
        .id = 46,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xEE, 0xF4, 0x9F, 0xAB, 0x6E, 0xDD } }  // Tag
        },
        .id = 47,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x90, 0xE1, 0x3D, 0xF1, 0xC0, 0xF3 } }  // Tag
        },
        .id = 48,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x65, 0xC4, 0x2B, 0x38, 0x17, 0xFD } }  // Tag
        },
        .id = 49,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x2B, 0xE8, 0x04, 0xFC, 0x8C, 0xE5 } }  // Tag
        },
        .id = 50,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xFC, 0x26, 0x34, 0xE9, 0xA0, 0xF9 } }  // Tag
        },
        .id = 51,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x4A, 0x58, 0xA8, 0xE3, 0x0F, 0xF8 } }  // Tag
        },
        .id = 52,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD0, 0xEB, 0x24, 0x81, 0x97, 0xE0 } }  // Tag
        },
        .id = 53,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x8E, 0x23, 0x63, 0xDE, 0x29, 0xE2 } }  // Tag
        },
        .id = 54,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x28, 0xA1, 0x83, 0xA0, 0xDB, 0xDF } }  // Tag
        },
        .id = 55,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x3B, 0x64, 0xDB, 0xF6, 0xBF, 0xEA } }  // Tag
        },
        .id = 56,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x1A, 0xB6, 0x98, 0xAA, 0x83, 0xE1 } }  // Tag
        },
        .id = 57,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD4, 0xA0, 0x0B, 0x44, 0x51, 0xFB } }  // Tag
        },
        .id = 58,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x4D, 0xC3, 0x6B, 0xFE, 0x03, 0xDB } }  // Tag
        },
        .id = 59,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA2, 0x17, 0xD4, 0xF4, 0xAF, 0xC7 } }  // Tag
        },
        .id = 60,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x78, 0x06, 0x34, 0x6A, 0x60, 0xC1 } }  // Tag
        },
        .id = 61,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x67, 0x3D, 0x14, 0x17, 0xD2, 0xCC } }  // Tag
        },
        .id = 62,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x00, 0x6A, 0xE7, 0x5E, 0x91, 0xEB } }  // Tag
        },
        .id = 63,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x13, 0xBA, 0xA2, 0x8E, 0xFB, 0xD4 } }  // Tag
        },
        .id = 64,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA4, 0x9E, 0x34, 0xF6, 0xF6, 0xFA } }  // Tag
        },
        .id = 65,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xF6, 0x66, 0x94, 0x82, 0x18, 0xEA } }  // Tag
        },
        .id = 66,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x9F, 0x02, 0x4B, 0x9A, 0x6E, 0xFE } }  // Tag
        },
        .id = 67,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x2E, 0x3B, 0x1D, 0xF3, 0x1D, 0xE8 } }  // Tag
        },
        .id = 68,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xAA, 0xBA, 0xDF, 0xF7, 0x88, 0xE4 } }  // Tag
        },
        .id = 69,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x42, 0xCF, 0x31, 0xD0, 0x38, 0xE6 } }  // Tag
        },
        .id = 70,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA6, 0xA9, 0x6D, 0x34, 0xC6, 0xC6 } }  // Tag
        },
        .id = 71,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x9A, 0xDD, 0xE9, 0x50, 0x44, 0xE9 } }  // Tag
        },
        .id = 72,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x47, 0x37, 0xA9, 0xFE, 0x27, 0xD8 } }  // Tag
        },
        .id = 73,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x08, 0xA2, 0x53, 0xD2, 0x90, 0xFF } }  // Tag
        },
        .id = 74,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x64, 0x20, 0xE4, 0xF1, 0x16, 0xFB } }  // Tag
        },
        .id = 75,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC4, 0xC0, 0x3B, 0xFC, 0x4A, 0xF4 } }  // Tag
        },
        .id = 76,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x51, 0x75, 0x90, 0x60, 0x4B, 0xC8 } }  // Tag
        },
        .id = 77,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x1A, 0xD2, 0x2F, 0x1E, 0x6E, 0xDB } }  // Tag
        },
        .id = 78,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA2, 0xA1, 0x14, 0xEF, 0xC7, 0xFC } }  // Tag
        },
        .id = 79,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x2D, 0x52, 0x72, 0xFF, 0x0C, 0xE5 } }  // Tag
        },
        .id = 80,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xF8, 0x6B, 0xEC, 0x69, 0xED, 0xD7 } }  // Tag
        },
        .id = 81,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x85, 0x2D, 0xF9, 0x89, 0x2E, 0xE2 } }  // Tag
        },
        .id = 82,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x86, 0x00, 0x1A, 0x10, 0x0F, 0xC9 } }  // Tag
        },
        .id = 83,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xEA, 0x20, 0x90, 0xF8, 0xC9, 0xE7 } }  // Tag
        },
        .id = 84,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x32, 0x07, 0xA5, 0x93, 0x27, 0xE8 } }  // Tag
        },
        .id = 85,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x6C, 0xAF, 0x7F, 0xE9, 0x3B, 0xE3 } }  // Tag
        },
        .id = 86,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC0, 0x77, 0x24, 0xF8, 0xB1, 0xCA } }  // Tag
        },
        .id = 87,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xAB, 0x8C, 0xBD, 0x94, 0x45, 0xF6 } }  // Tag
        },
        .id = 88,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x1C, 0x63, 0x40, 0x51, 0xC0, 0xEC } }  // Tag
        },
        .id = 89,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xCB, 0x0C, 0xD1, 0x11, 0xCF, 0xE0 } }  // Tag
        },
        .id = 90,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC7, 0x88, 0xE9, 0xB6, 0x1D, 0xC6 } }  // Tag
        },
        .id = 91,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xB8, 0xDE, 0xEE, 0xFE, 0x10, 0xCE } }  // Tag
        },
        .id = 92,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x54, 0x91, 0x60, 0xCE, 0xFE, 0xCF } }  // Tag
        },
        .id = 93,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xF1, 0x5E, 0x99, 0x3A, 0xDC, 0xFE } }  // Tag
        },
        .id = 94,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA6, 0xD4, 0x99, 0x21, 0xC8, 0xC3 } }  // Tag
        },
        .id = 95,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x7B, 0xCA, 0x73, 0x5D, 0xDE, 0xC0 } }  // Tag
        },
        .id = 96,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x7F, 0x2F, 0x99, 0xC2, 0xDB, 0xF4 } }  // Tag
        },
        .id = 97,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x37, 0xD6, 0x53, 0xA9, 0xEF, 0xE5 } }  // Tag
        },
        .id = 98,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD4, 0xD8, 0x58, 0xE5, 0x75, 0xCB } }  // Tag
        },
        .id = 99,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA7, 0x3C, 0x17, 0x15, 0x1E, 0xC9 } }  // Tag
        },
        .id = 100,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xEF, 0x46, 0x4B, 0x13, 0x28, 0xF2 } }  // Tag
        },
        .id = 101,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x6B, 0x49, 0x46, 0x14, 0xC2, 0xE5 } }  // Tag
        },
        .id = 102,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x46, 0x0A, 0xB5, 0xDD, 0x83, 0xE6 } }  // Tag
        },
        .id = 103,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x1A, 0x36, 0x2A, 0xFA, 0x22, 0xDC } }  // Tag
        },
        .id = 104,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x90, 0xE1, 0x6A, 0x38, 0x40, 0xD4 } }  // Tag
        },
        .id = 105,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x25, 0x46, 0x21, 0xFE, 0x10, 0xE6 } }  // Tag
        },
        .id = 106,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xBF, 0x42, 0xFB, 0xC9, 0x2B, 0xC3 } }  // Tag
        },
        .id = 107,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x42, 0xA5, 0x1A, 0x6E, 0x69, 0xDA } }  // Tag
        },
        .id = 108,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x70, 0xF8, 0x98, 0x8D, 0x67, 0xFB } }  // Tag
        },
        .id = 109,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x53, 0x92, 0xE9, 0xD0, 0xD8, 0xD4 } }  // Tag
        },
        .id = 110,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x97, 0xEB, 0x62, 0xEC, 0xDB, 0xCE } }  // Tag
        },
        .id = 111,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x3C, 0x53, 0x59, 0xC1, 0x32, 0xC6 } }  // Tag
        },
        .id = 112,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xB2, 0xC6, 0xAE, 0xF8, 0xB9, 0xDE } }  // Tag
        },
        .id = 113,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x2D, 0xF9, 0x65, 0xE6, 0x84, 0xDB } }  // Tag
        },
        .id = 114,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD4, 0x7C, 0x2B, 0xFE, 0x15, 0xCD } }  // Tag
        },
        .id = 115,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x7B, 0x53, 0x06, 0x57, 0x41, 0xC8 } }  // Tag
        },
        .id = 116,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x3E, 0x80, 0x1E, 0xB5, 0xBF, 0xCB } }  // Tag
        },
        .id = 117,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x2A, 0xC2, 0x4D, 0x65, 0xC7, 0xED } }  // Tag
        },
        .id = 118,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xAD, 0x3E, 0xAB, 0x14, 0x4F, 0xC5 } }  // Tag
        },
        .id = 119,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xF5, 0x44, 0x84, 0xFF, 0xC3, 0xD8 } }  // Tag
        },
        .id = 120,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD6, 0x4E, 0x22, 0x6F, 0x84, 0xD3 } }  // Tag
        },
        .id = 121,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x8D, 0xC7, 0x01, 0x6A, 0xC0, 0xD5 } }  // Tag
        },
        .id = 122,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xB9, 0x4E, 0xD0, 0xF3, 0xA6, 0xCB } }  // Tag
        },
        .id = 123,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x52, 0x94, 0x95, 0x32, 0xDA, 0xEF } }  // Tag
        },
        .id = 124,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x26, 0xCD, 0x6C, 0x4A, 0x6F, 0xE9 } }  // Tag
        },
        .id = 125,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x10, 0x19, 0xD7, 0xF4, 0x0E, 0xE2 } }  // Tag
        },
        .id = 126,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD1, 0x2D, 0xD2, 0xBC, 0xD6, 0xE6 } }  // Tag
        },
        .id = 127,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC9, 0x01, 0xEC, 0xA3, 0xC4, 0xD2 } }  // Tag
        },
        .id = 128,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xDB, 0x6C, 0x19, 0x31, 0xA7, 0xD8 } }  // Tag
        },
        .id = 129,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xDB, 0x45, 0xD8, 0xE7, 0x3F, 0xD0 } }  // Tag
        },
        .id = 130,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xDD, 0x21, 0x09, 0xFA, 0xF9, 0xDC } }  // Tag
        },
        .id = 131,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x16, 0xB6, 0xC7, 0xF2, 0x57, 0xFF } }  // Tag
        },
        .id = 132,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x17, 0x6E, 0x78, 0xFD, 0x5D, 0xE7 } }  // Tag
        },
        .id = 133,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD6, 0x14, 0xBE, 0x98, 0x7F, 0xC4 } }  // Tag
        },
        .id = 134,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x66, 0xC6, 0xBB, 0xAA, 0x3A, 0xCC } }  // Tag
        },
        .id = 135,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x32, 0x15, 0x24, 0xF2, 0xC4, 0xC9 } }  // Tag
        },
        .id = 136,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x4F, 0x0A, 0xA1, 0x1F, 0xCB, 0xD7 } }  // Tag
        },
        .id = 137,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x1D, 0x1B, 0xF3, 0xFF, 0x86, 0xC3 } }  // Tag
        },
        .id = 138,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xE4, 0xC3, 0x09, 0x90, 0x6C, 0xCE } }  // Tag
        },
        .id = 139,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x20, 0x61, 0x5B, 0xFB, 0xCE, 0xEA } }  // Tag
        },
        .id = 140,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x5A, 0xED, 0x03, 0xFE, 0x98, 0xDA } }  // Tag
        },
        .id = 141,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xE3, 0x0B, 0x61, 0xF3, 0xF7, 0xE4 } }  // Tag
        },
        .id = 142,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x67, 0x24, 0x2E, 0xF3, 0x42, 0xD0 } }  // Tag
        },
        .id = 143,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xFC, 0x35, 0xBA, 0x06, 0xAA, 0xFD } }  // Tag
        },
        .id = 144,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x06, 0x14, 0xFF, 0xEE, 0xA3, 0xC6 } }  // Tag
        },
        .id = 145,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x4B, 0xFA, 0x68, 0xEF, 0xBB, 0xCC } }  // Tag
        },
        .id = 146,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC0, 0x56, 0x66, 0x66, 0x97, 0xD1 } }  // Tag
        },
        .id = 147,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x4B, 0x44, 0x27, 0x62, 0xE7, 0xE8 } }  // Tag
        },
        .id = 148,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xEF, 0x77, 0x7D, 0x04, 0xF9, 0xCE } }  // Tag
        },
        .id = 149,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x23, 0xF6, 0xCD, 0x73, 0x4D, 0xC6 } }  // Tag
        },
        .id = 150,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xCC, 0x94, 0x66, 0x60, 0x1A, 0xDC } }  // Tag
        },
        .id = 151,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x10, 0x0C, 0x05, 0xE9, 0xF0, 0xEB } }  // Tag
        },
        .id = 152,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x9B, 0x49, 0x5D, 0x04, 0xA8, 0xC0 } }  // Tag
        },
        .id = 153,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x6C, 0x6A, 0xD6, 0xE8, 0xE3, 0xC7 } }  // Tag
        },
        .id = 154,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xD1, 0xD8, 0xF9, 0x76, 0x9C, 0xC0 } }  // Tag
        },
        .id = 155,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x66, 0x11, 0x93, 0x97, 0xCA, 0xD6 } }  // Tag
        },
        .id = 156,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x86, 0x2C, 0x99, 0x88, 0x9A, 0xF3 } }  // Tag
        },
        .id = 157,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA7, 0x09, 0x92, 0xF5, 0xB5, 0xD9 } }  // Tag
        },
        .id = 158,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x1F, 0x95, 0x9B, 0xFD, 0x93, 0xF5 } }  // Tag
        },
        .id = 159,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x76, 0xD2, 0x30, 0xAC, 0xEF, 0xD5 } }  // Tag
        },
        .id = 160,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x6C, 0x88, 0xAE, 0xF8, 0xD7, 0xF8 } }  // Tag
        },
        .id = 161,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x28, 0x95, 0xEC, 0xE5, 0x2B, 0xDE } }  // Tag
        },
        .id = 162,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xC7, 0x19, 0xD7, 0x05, 0x8C, 0xC8 } }  // Tag
        },
        .id = 163,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA4, 0x5D, 0xA1, 0xAC, 0xDF, 0xFD } }  // Tag
        },
        .id = 164,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x07, 0x2A, 0x7E, 0xDF, 0xF6, 0xE6 } }  // Tag
        },
        .id = 165,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xF4, 0x94, 0x73, 0x82, 0x91, 0xE7 } }  // Tag
        },
        .id = 166,
    },
{
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x04, 0xFF, 0x94, 0xFF, 0xD0, 0xD7 } }  // Tag
        },
        .id = 167,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xA5, 0x17, 0xC7, 0xBC, 0xE2, 0xDF } }  // Tag
        },
        .id = 168,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0xB7, 0x0B, 0x9C, 0xF6, 0x26, 0xEF } }  // Tag
        },
        .id = 169,
    },
    {
        .addr = {
            .type = BT_ADDR_LE_RANDOM,
            .a = { .val = { 0x70, 0xE3, 0xF5, 0xFE, 0x03, 0xDE } }  // Tag
        },
        .id = 170,
    },
};

const size_t known_device_table_len =
    sizeof(known_device_table) / sizeof(known_device_table[0]);
