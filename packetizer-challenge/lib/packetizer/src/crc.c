#include "packetizer/crc.h"

/*
 * Nibble tables (16 entries) instead of the usual 256-entry byte table: 1/16th
 * the flash footprint at the cost of two table lookups and two shifts per byte
 * instead of one of each. Bit-by-bit would save the table entirely but costs
 * 8 branchy iterations per byte; on a target this size, the nibble table is
 * the middle ground worth taking.
 */

static const uint16_t crc16_nibble_table[16] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef
};

static const uint32_t crc32_nibble_table[16] = {
    0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
    0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
    0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
    0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu
};

uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        crc = (uint16_t)((crc << 4) ^ crc16_nibble_table[((crc >> 12) ^ (byte >> 4)) & 0x0Fu]);
        crc = (uint16_t)((crc << 4) ^ crc16_nibble_table[((crc >> 12) ^ (byte & 0x0Fu)) & 0x0Fu]);
    }
    return crc;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
        crc = (crc >> 4) ^ crc32_nibble_table[crc & 0x0Fu];
    }
    return crc;
}

uint32_t crc32_finalize(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}
