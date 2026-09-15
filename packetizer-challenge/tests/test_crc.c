#include "packetizer/crc.h"
#include "test.h"

#include <stdlib.h>

/* Bit-by-bit references, kept deliberately naive (no tables) so they serve as
 * an independent check on the nibble-table implementation under test. */

static uint16_t crc16_ref(uint16_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)(((unsigned int)crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)((unsigned int)crc << 1);
            }
        }
    }
    return crc;
}

static uint32_t crc32_ref(uint32_t crc, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

TEST(check_values) {
    static const uint8_t msg[] = "123456789";
    size_t len = sizeof(msg) - 1;

    CHECK_EQ(crc16_ccitt(PKT_CRC16_INIT, msg, len), 0x29B1u);
    CHECK_EQ(crc32_finalize(crc32_update(PKT_CRC32_INIT, msg, len)), 0xCBF43926u);
}

TEST(empty_input) {
    /* No bytes consumed, so the running value is untouched: CRC-16/CCITT-FALSE
     * has no output XOR, and CRC-32's output XOR turns 0xFFFFFFFF into 0. */
    CHECK_EQ(crc16_ccitt(PKT_CRC16_INIT, NULL, 0), PKT_CRC16_INIT);
    CHECK_EQ(crc32_finalize(crc32_update(PKT_CRC32_INIT, NULL, 0)), 0x00000000u);
}

TEST(incremental_matches_single_shot) {
    uint8_t buf[977];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 37u + 11u);
    }

    uint16_t crc16_whole = crc16_ccitt(PKT_CRC16_INIT, buf, sizeof(buf));
    uint32_t crc32_whole = crc32_finalize(crc32_update(PKT_CRC32_INIT, buf, sizeof(buf)));

    static const size_t chunk_sizes[] = { 1, 2, 3, 5, 7, 11, 13, 17, 23, 41, 97 };
    for (size_t c = 0; c < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); c++) {
        size_t chunk = chunk_sizes[c];
        uint16_t crc16_inc = PKT_CRC16_INIT;
        uint32_t crc32_inc = PKT_CRC32_INIT;
        for (size_t off = 0; off < sizeof(buf); off += chunk) {
            size_t n = chunk;
            if (n > sizeof(buf) - off) {
                n = sizeof(buf) - off;
            }
            crc16_inc = crc16_ccitt(crc16_inc, buf + off, n);
            crc32_inc = crc32_update(crc32_inc, buf + off, n);
        }
        CHECK_EQ(crc16_inc, crc16_whole);
        CHECK_EQ(crc32_finalize(crc32_inc), crc32_whole);
    }
}

TEST(matches_bitwise_reference) {
    srand(1234u);

    for (int trial = 0; trial < 1000; trial++) {
        uint8_t buf[256];
        size_t len = (size_t)(rand() % (int)(sizeof(buf) + 1));
        for (size_t i = 0; i < len; i++) {
            buf[i] = (uint8_t)rand();
        }

        uint16_t crc16_got = crc16_ccitt(PKT_CRC16_INIT, buf, len);
        uint16_t crc16_want = crc16_ref(PKT_CRC16_INIT, buf, len);
        CHECK_EQ(crc16_got, crc16_want);

        uint32_t crc32_got = crc32_finalize(crc32_update(PKT_CRC32_INIT, buf, len));
        uint32_t crc32_want = crc32_finalize(crc32_ref(PKT_CRC32_INIT, buf, len));
        CHECK_EQ(crc32_got, crc32_want);
    }
}

int main(void) {
    RUN_TEST(check_values);
    RUN_TEST(empty_input);
    RUN_TEST(incremental_matches_single_shot);
    RUN_TEST(matches_bitwise_reference);
    return TEST_SUMMARY();
}
