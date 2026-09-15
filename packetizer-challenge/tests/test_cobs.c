#include "packetizer/cobs.h"
#include "test.h"

#include <stdlib.h>

typedef struct {
    const uint8_t *raw;
    size_t raw_len;
    const uint8_t *encoded;
    size_t encoded_len;
} cobs_vector_t;

static const uint8_t v0_raw[] = { 0x00 };
static const uint8_t v0_enc[] = { 0x01, 0x01 };

static const uint8_t v1_raw[] = { 0x00, 0x00 };
static const uint8_t v1_enc[] = { 0x01, 0x01, 0x01 };

static const uint8_t v2_raw[] = { 0x11, 0x22, 0x00, 0x33 };
static const uint8_t v2_enc[] = { 0x03, 0x11, 0x22, 0x02, 0x33 };

static const uint8_t v3_raw[] = { 0x11, 0x00, 0x00, 0x00 };
static const uint8_t v3_enc[] = { 0x02, 0x11, 0x01, 0x01, 0x01 };

static const cobs_vector_t vectors[] = {
    { v0_raw, sizeof(v0_raw), v0_enc, sizeof(v0_enc) },
    { v1_raw, sizeof(v1_raw), v1_enc, sizeof(v1_enc) },
    { v2_raw, sizeof(v2_raw), v2_enc, sizeof(v2_enc) },
    { v3_raw, sizeof(v3_raw), v3_enc, sizeof(v3_enc) },
};
static const size_t vector_count = sizeof(vectors) / sizeof(vectors[0]);

TEST(known_vectors_encode) {
    for (size_t v = 0; v < vector_count; v++) {
        uint8_t out[32];
        size_t n = cobs_encode(vectors[v].raw, vectors[v].raw_len, out, sizeof(out));
        CHECK_EQ(n, vectors[v].encoded_len);
        CHECK_MEM(out, vectors[v].encoded, vectors[v].encoded_len);
    }
}

TEST(known_vectors_decode) {
    for (size_t v = 0; v < vector_count; v++) {
        uint8_t out[32];
        size_t n = 0;
        bool ok = cobs_decode(vectors[v].encoded, vectors[v].encoded_len, out, sizeof(out), &n);
        CHECK(ok);
        CHECK_EQ(n, vectors[v].raw_len);
        CHECK_MEM(out, vectors[v].raw, vectors[v].raw_len);
    }
}

TEST(empty_input) {
    uint8_t enc[COBS_MAX_ENCODED(0)];
    size_t n = cobs_encode(NULL, 0, enc, sizeof(enc));
    CHECK_EQ(n, 1u);
    CHECK_EQ(enc[0], 0x01);

    uint8_t out[1];
    size_t out_len = 99;
    bool ok = cobs_decode(enc, n, out, sizeof(out), &out_len);
    CHECK(ok);
    CHECK_EQ(out_len, 0u);
}

/* 254 non-zero bytes is the largest run a single code byte can cover: code
 * climbs from 1 to exactly 0xFF without ever seeing a zero. 255 pushes it
 * one byte past that boundary and forces a second block. */
TEST(boundary_254_and_255_nonzero) {
    uint8_t raw[255];
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = (uint8_t)(1 + (i % 255));
    }

    uint8_t enc[COBS_MAX_ENCODED(255)];

    size_t n254 = cobs_encode(raw, 254, enc, sizeof(enc));
    CHECK_EQ(n254, COBS_MAX_ENCODED(254));
    CHECK_EQ(enc[0], 0xFF);
    CHECK_EQ(enc[n254 - 1], 0x01);

    uint8_t out[255];
    size_t out_len = 0;
    CHECK(cobs_decode(enc, n254, out, sizeof(out), &out_len));
    CHECK_EQ(out_len, 254u);
    CHECK_MEM(out, raw, 254u);

    size_t n255 = cobs_encode(raw, 255, enc, sizeof(enc));
    CHECK_EQ(n255, COBS_MAX_ENCODED(255));
    CHECK_EQ(enc[0], 0xFF);

    out_len = 0;
    CHECK(cobs_decode(enc, n255, out, sizeof(out), &out_len));
    CHECK_EQ(out_len, 255u);
    CHECK_MEM(out, raw, 255u);
}

TEST(roundtrip_random) {
    srand(4242u);

    for (size_t len = 0; len <= 600; len++) {
        uint8_t raw[601];
        for (size_t i = 0; i < len; i++) {
            raw[i] = (uint8_t)rand();
        }

        uint8_t enc[COBS_MAX_ENCODED(601)];
        size_t enc_len = cobs_encode(raw, len, enc, sizeof(enc));
        CHECK(enc_len != 0);

        uint8_t out[601];
        size_t out_len = 0;
        bool ok = cobs_decode(enc, enc_len, out, sizeof(out), &out_len);
        CHECK(ok);
        CHECK_EQ(out_len, len);
        CHECK_MEM(out, raw, len);
    }
}

TEST(encoded_never_contains_zero) {
    srand(99u);

    for (size_t len = 0; len <= 600; len++) {
        uint8_t raw[601];
        for (size_t i = 0; i < len; i++) {
            raw[i] = (uint8_t)rand();
        }

        uint8_t enc[COBS_MAX_ENCODED(601)];
        size_t enc_len = cobs_encode(raw, len, enc, sizeof(enc));
        CHECK(enc_len != 0);

        for (size_t i = 0; i < enc_len; i++) {
            CHECK(enc[i] != 0x00);
        }
    }
}

TEST(decode_rejects_zero_byte) {
    static const uint8_t bad_code[] = { 0x00 };
    static const uint8_t bad_data[] = { 0x03, 0x11, 0x00, 0x22 };

    uint8_t out[16];
    size_t out_len = 0;
    CHECK(!cobs_decode(bad_code, sizeof(bad_code), out, sizeof(out), &out_len));
    CHECK(!cobs_decode(bad_data, sizeof(bad_data), out, sizeof(out), &out_len));
}

TEST(decode_rejects_code_past_end) {
    /* Code 5 claims 4 data bytes but only 2 remain before the input ends. */
    static const uint8_t bad[] = { 0x05, 0x11, 0x22 };

    uint8_t out[16];
    size_t out_len = 0;
    CHECK(!cobs_decode(bad, sizeof(bad), out, sizeof(out), &out_len));
}

TEST(decode_rejects_output_too_large) {
    uint8_t out[3];
    size_t out_len = 0;
    /* Decodes to 4 bytes (see known_vectors), cap only holds 3. */
    CHECK(!cobs_decode(v2_enc, sizeof(v2_enc), out, sizeof(out), &out_len));
}

TEST(encode_rejects_insufficient_cap) {
    uint8_t out[4];
    CHECK_EQ(cobs_encode(v2_raw, sizeof(v2_raw), out, 0), 0);
    CHECK_EQ(cobs_encode(v2_raw, sizeof(v2_raw), out, sizeof(out)), 0);
}

TEST(decode_rejects_insufficient_cap) {
    uint8_t out[3];
    size_t out_len = 0;
    CHECK(!cobs_decode(v2_enc, sizeof(v2_enc), out, 0, &out_len));
    CHECK(!cobs_decode(v2_enc, sizeof(v2_enc), out, sizeof(out), &out_len));
}

int main(void) {
    RUN_TEST(known_vectors_encode);
    RUN_TEST(known_vectors_decode);
    RUN_TEST(empty_input);
    RUN_TEST(boundary_254_and_255_nonzero);
    RUN_TEST(roundtrip_random);
    RUN_TEST(encoded_never_contains_zero);
    RUN_TEST(decode_rejects_zero_byte);
    RUN_TEST(decode_rejects_code_past_end);
    RUN_TEST(decode_rejects_output_too_large);
    RUN_TEST(encode_rejects_insufficient_cap);
    RUN_TEST(decode_rejects_insufficient_cap);
    return TEST_SUMMARY();
}
