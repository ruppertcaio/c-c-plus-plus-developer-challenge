#include "frame.h"
#include "packetizer/crc.h"
#include "test.h"

#include <string.h>

#define MAX_FRAMES_OUT 8

/* deframer_push()'s f->payload aliases d->buf, which the very next push can
 * overwrite, so results are snapshotted here to stay valid for later
 * assertions in the same test. */
typedef struct {
    struct frame frames[MAX_FRAMES_OUT];
    uint8_t payload_copy[MAX_FRAMES_OUT][PKT_MAX_PAYLOAD];
    size_t count;
} push_result_t;

static void push_all(struct deframer *d, const uint8_t *bytes, size_t n,
                      struct pkt_rx_errors *errs, push_result_t *res) {
    for (size_t i = 0; i < n; i++) {
        struct frame f;
        if (deframer_push(d, bytes[i], &f, errs)) {
            if (res->count < MAX_FRAMES_OUT) {
                if (f.payload_len > 0) {
                    memcpy(res->payload_copy[res->count], f.payload, f.payload_len);
                }
                f.payload = res->payload_copy[res->count];
                res->frames[res->count] = f;
                res->count++;
            }
        }
    }
}

static void fill_payload(uint8_t *buf, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i * 31u);
    }
}

static size_t make_encoded(uint8_t type, uint8_t epoch, uint16_t msg_id, uint16_t frag_idx,
                            uint16_t frag_cnt, const uint8_t *payload, size_t payload_len,
                            uint8_t *out, size_t cap) {
    struct frame f;
    f.type = type;
    f.epoch = epoch;
    f.msg_id = msg_id;
    f.frag_idx = frag_idx;
    f.frag_cnt = frag_cnt;
    f.payload = payload;
    f.payload_len = payload_len;
    return frame_encode(&f, out, cap);
}

static void set_crc(uint8_t *buf, size_t len_without_crc) {
    uint16_t crc = crc16_ccitt(PKT_CRC16_INIT, buf, len_without_crc);
    buf[len_without_crc] = (uint8_t)(crc & 0xFFu);
    buf[len_without_crc + 1] = (uint8_t)(crc >> 8);
}

TEST(roundtrip_each_type_payload_bounds) {
    static const uint8_t types[] = { FRAME_DATA, FRAME_ACK, FRAME_DONE, FRAME_NACK };
    static const size_t payload_lens[] = { 0, PKT_MAX_PAYLOAD };

    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        for (size_t p = 0; p < sizeof(payload_lens) / sizeof(payload_lens[0]); p++) {
            uint8_t payload[PKT_MAX_PAYLOAD];
            fill_payload(payload, payload_lens[p], (unsigned)(t * 17u + p));

            uint8_t wire[PKT_MAX_WIRE_FRAME];
            size_t wire_len = make_encoded(types[t], (uint8_t)(t + 1), (uint16_t)(1000 + t),
                                            (uint16_t)p, (uint16_t)(p + 1), payload,
                                            payload_lens[p], wire, sizeof(wire));
            CHECK(wire_len != 0);
            CHECK_EQ(wire[wire_len - 1], 0x00);
            for (size_t i = 0; i < wire_len - 1; i++) {
                CHECK(wire[i] != 0x00);
            }

            struct deframer d = { 0 };
            struct pkt_rx_errors errs = { 0 };
            push_result_t res = { 0 };
            push_all(&d, wire, wire_len, &errs, &res);

            CHECK_EQ(res.count, 1u);
            if (res.count == 1) {
                CHECK_EQ(res.frames[0].type, types[t]);
                CHECK_EQ(res.frames[0].epoch, (uint8_t)(t + 1));
                CHECK_EQ(res.frames[0].msg_id, 1000 + t);
                CHECK_EQ(res.frames[0].frag_idx, p);
                CHECK_EQ(res.frames[0].frag_cnt, p + 1);
                CHECK_EQ(res.frames[0].payload_len, payload_lens[p]);
                CHECK_MEM(res.frames[0].payload, payload, payload_lens[p]);
            }
            CHECK_EQ(errs.cobs_error + errs.short_frame + errs.bad_crc + errs.bad_version +
                     errs.bad_type + errs.too_long + errs.overflow, 0u);
        }
    }
}

/* Flips every bit of every byte of one encoded frame EXCEPT the delimiter,
 * feeds the corrupted variant straight into the next, untouched valid frame
 * (no resync byte), and checks the corrupted variant is never accepted while
 * the valid one always is.
 *
 * This holds even for the flips that happen to turn a content byte into a
 * spurious mid-frame 0x00 (any byte whose original value had exactly one bit
 * set): the frame's real trailing delimiter is untouched, so it still closes
 * the frame out cleanly, either directly or via a second rejected attempt
 * over the leftover suffix once the spurious zero has already forced one
 * rejection. Either way the deframer's buffer is empty again before the next
 * frame's bytes start arriving, so nothing merges. See
 * bitflip_delimiter_loses_next_frame for the one case that is not
 * self-healing this way. */
TEST(bitflip_content_recovers_directly) {
    uint8_t payload[PKT_MAX_PAYLOAD];
    fill_payload(payload, sizeof(payload), 7);
    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = make_encoded(FRAME_DATA, 3, 55, 2, 9, payload, sizeof(payload), wire, sizeof(wire));
    CHECK(wire_len != 0);

    uint8_t valid_payload[PKT_MAX_PAYLOAD];
    fill_payload(valid_payload, sizeof(valid_payload), 99);
    uint8_t valid_wire[PKT_MAX_WIRE_FRAME];
    size_t valid_len = make_encoded(FRAME_ACK, 4, 200, 0, 1, valid_payload, sizeof(valid_payload),
                                     valid_wire, sizeof(valid_wire));
    CHECK(valid_len != 0);

    for (size_t byte_i = 0; byte_i + 1 < wire_len; byte_i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t corrupted[PKT_MAX_WIRE_FRAME];
            memcpy(corrupted, wire, wire_len);
            corrupted[byte_i] = (uint8_t)(corrupted[byte_i] ^ (uint8_t)(1u << bit));

            struct deframer d = { 0 };
            struct pkt_rx_errors errs = { 0 };
            push_result_t res = { 0 };

            push_all(&d, corrupted, wire_len, &errs, &res);
            CHECK_EQ(res.count, 0u);

            push_all(&d, valid_wire, valid_len, &errs, &res);
            CHECK_EQ(res.count, 1u);
            if (res.count == 1) {
                CHECK_EQ(res.frames[0].type, FRAME_ACK);
                CHECK_EQ(res.frames[0].msg_id, 200u);
                CHECK_MEM(res.frames[0].payload, valid_payload, sizeof(valid_payload));
            }
        }
    }
}

/* Flipping a bit of the delimiter itself (0x00 -> nonzero) is different in
 * kind from every other single-bit corruption: it destroys the one thing
 * marking where the frame ends. Nothing else in the stream can be a
 * delimiter by COBS's own no-embedded-zero invariant, so the deframer just
 * keeps accumulating: the corrupted frame's bytes run straight into the
 * next frame (victim) and both are judged, and rejected, as one blob when
 * victim's own delimiter is finally reached. Victim is not decoded on its
 * own; it is lost along with the corruption. Only a frame arriving after
 * that point (sacrifice), behind victim's own intact delimiter, gets a
 * clean boundary to resync on. PROTOCOL.md section 3 documents this. */
TEST(bitflip_delimiter_loses_next_frame) {
    uint8_t payload[PKT_MAX_PAYLOAD];
    fill_payload(payload, sizeof(payload), 7);
    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = make_encoded(FRAME_DATA, 3, 55, 2, 9, payload, sizeof(payload), wire, sizeof(wire));
    CHECK(wire_len != 0);

    uint8_t victim_payload[PKT_MAX_PAYLOAD];
    fill_payload(victim_payload, sizeof(victim_payload), 99);
    uint8_t victim_wire[PKT_MAX_WIRE_FRAME];
    size_t victim_len = make_encoded(FRAME_ACK, 4, 200, 0, 1, victim_payload, sizeof(victim_payload),
                                      victim_wire, sizeof(victim_wire));
    CHECK(victim_len != 0);

    uint8_t sacrifice_payload[PKT_MAX_PAYLOAD];
    fill_payload(sacrifice_payload, sizeof(sacrifice_payload), 150);
    uint8_t sacrifice_wire[PKT_MAX_WIRE_FRAME];
    size_t sacrifice_len = make_encoded(FRAME_DONE, 5, 300, 0, 1, sacrifice_payload, sizeof(sacrifice_payload),
                                         sacrifice_wire, sizeof(sacrifice_wire));
    CHECK(sacrifice_len != 0);

    for (int bit = 0; bit < 8; bit++) {
        uint8_t corrupted[PKT_MAX_WIRE_FRAME];
        memcpy(corrupted, wire, wire_len);
        corrupted[wire_len - 1] = (uint8_t)(corrupted[wire_len - 1] ^ (uint8_t)(1u << bit));
        CHECK(corrupted[wire_len - 1] != 0x00);

        struct deframer d = { 0 };
        struct pkt_rx_errors errs = { 0 };
        push_result_t res = { 0 };

        push_all(&d, corrupted, wire_len, &errs, &res);
        push_all(&d, victim_wire, victim_len, &errs, &res);
        push_all(&d, sacrifice_wire, sacrifice_len, &errs, &res);

        CHECK_EQ(res.count, 1u);
        if (res.count == 1) {
            CHECK_EQ(res.frames[0].type, FRAME_DONE);
            CHECK_EQ(res.frames[0].msg_id, 300u);
            CHECK_MEM(res.frames[0].payload, sacrifice_payload, sizeof(sacrifice_payload));
        }
    }
}

TEST(garbage_before_frame_recovers) {
    uint8_t payload[16];
    fill_payload(payload, sizeof(payload), 3);
    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = make_encoded(FRAME_DATA, 1, 10, 0, 1, payload, sizeof(payload), wire, sizeof(wire));
    CHECK(wire_len != 0);

    static const uint8_t junk[] = { 0x11, 0x22, 0x33, 0x44, 0x00 };

    struct deframer d = { 0 };
    struct pkt_rx_errors errs = { 0 };
    push_result_t res = { 0 };

    push_all(&d, junk, sizeof(junk), &errs, &res);
    CHECK_EQ(res.count, 0u);
    CHECK(errs.cobs_error >= 1u);

    push_all(&d, wire, wire_len, &errs, &res);
    CHECK_EQ(res.count, 1u);
    if (res.count == 1) {
        CHECK_EQ(res.frames[0].msg_id, 10u);
        CHECK_MEM(res.frames[0].payload, payload, sizeof(payload));
    }
}

TEST(two_frames_back_to_back) {
    uint8_t pa[8];
    uint8_t pb[20];
    fill_payload(pa, sizeof(pa), 1);
    fill_payload(pb, sizeof(pb), 2);

    uint8_t wire_a[PKT_MAX_WIRE_FRAME];
    uint8_t wire_b[PKT_MAX_WIRE_FRAME];
    size_t len_a = make_encoded(FRAME_DATA, 0, 1, 0, 2, pa, sizeof(pa), wire_a, sizeof(wire_a));
    size_t len_b = make_encoded(FRAME_DATA, 0, 1, 1, 2, pb, sizeof(pb), wire_b, sizeof(wire_b));
    CHECK(len_a != 0);
    CHECK(len_b != 0);

    uint8_t glued[2 * PKT_MAX_WIRE_FRAME];
    memcpy(glued, wire_a, len_a);
    memcpy(glued + len_a, wire_b, len_b);

    struct deframer d = { 0 };
    struct pkt_rx_errors errs = { 0 };
    push_result_t res = { 0 };
    push_all(&d, glued, len_a + len_b, &errs, &res);

    CHECK_EQ(res.count, 2u);
    if (res.count == 2) {
        CHECK_EQ(res.frames[0].frag_idx, 0u);
        CHECK_MEM(res.frames[0].payload, pa, sizeof(pa));
        CHECK_EQ(res.frames[1].frag_idx, 1u);
        CHECK_MEM(res.frames[1].payload, pb, sizeof(pb));
    }
}

TEST(chunked_delivery_recovers) {
    uint8_t payload[PKT_MAX_PAYLOAD];
    fill_payload(payload, sizeof(payload), 5);
    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = make_encoded(FRAME_DATA, 2, 77, 1, 3, payload, sizeof(payload), wire, sizeof(wire));
    CHECK(wire_len != 0);

    for (size_t chunk = 1; chunk <= 7; chunk++) {
        struct deframer d = { 0 };
        struct pkt_rx_errors errs = { 0 };
        push_result_t res = { 0 };

        for (size_t off = 0; off < wire_len; off += chunk) {
            size_t n = chunk;
            if (n > wire_len - off) {
                n = wire_len - off;
            }
            push_all(&d, wire + off, n, &errs, &res);
        }

        CHECK_EQ(res.count, 1u);
        if (res.count == 1) {
            CHECK_EQ(res.frames[0].msg_id, 77u);
            CHECK_MEM(res.frames[0].payload, payload, sizeof(payload));
        }
    }
}

TEST(giant_unterminated_frame_then_valid_recovers) {
    struct deframer d = { 0 };
    struct pkt_rx_errors errs = { 0 };
    struct frame scratch;

    /* Twice the buffer capacity, no zero byte in the run: forces overflow
     * discard well before any delimiter could close the frame. */
    for (size_t i = 0; i < 2 * sizeof(d.buf); i++) {
        uint8_t b = (uint8_t)(1u + (i % 255u));
        CHECK(!deframer_push(&d, b, &scratch, &errs));
    }
    CHECK_EQ(errs.overflow, 1u);

    CHECK(!deframer_push(&d, 0x00, &scratch, &errs));

    uint8_t payload[10];
    fill_payload(payload, sizeof(payload), 6);
    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = make_encoded(FRAME_NACK, 0, 5, 0, 1, payload, sizeof(payload), wire, sizeof(wire));
    CHECK(wire_len != 0);

    push_result_t res = { 0 };
    push_all(&d, wire, wire_len, &errs, &res);
    CHECK_EQ(res.count, 1u);
    if (res.count == 1) {
        CHECK_EQ(res.frames[0].msg_id, 5u);
    }
}

TEST(parse_rejects_short_and_too_long) {
    uint8_t too_short[PKT_HEADER_SIZE + PKT_CRC_SIZE - 1];
    memset(too_short, 0, sizeof(too_short));
    struct frame f;
    CHECK_EQ(frame_parse(too_short, sizeof(too_short), &f), FRAME_ERR_SHORT);

    uint8_t too_long[PKT_MAX_FRAME + 1];
    memset(too_long, 0, sizeof(too_long));
    CHECK_EQ(frame_parse(too_long, sizeof(too_long), &f), FRAME_ERR_TOO_LONG);
}

TEST(parse_rejects_bad_version_and_type_with_good_crc) {
    uint8_t buf[PKT_HEADER_SIZE + PKT_CRC_SIZE];
    struct frame f;

    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)((2u << 4) | (uint8_t)FRAME_DATA); /* ver = 2, unsupported */
    set_crc(buf, PKT_HEADER_SIZE);
    CHECK_EQ(frame_parse(buf, sizeof(buf), &f), FRAME_ERR_BAD_VERSION);

    memset(buf, 0, sizeof(buf));
    buf[0] = (uint8_t)((PKT_VERSION << 4) | 0x0Fu); /* type 15, not DATA/ACK/DONE/NACK */
    set_crc(buf, PKT_HEADER_SIZE);
    CHECK_EQ(frame_parse(buf, sizeof(buf), &f), FRAME_ERR_BAD_TYPE);
}

int main(void) {
    RUN_TEST(roundtrip_each_type_payload_bounds);
    RUN_TEST(bitflip_content_recovers_directly);
    RUN_TEST(bitflip_delimiter_loses_next_frame);
    RUN_TEST(garbage_before_frame_recovers);
    RUN_TEST(two_frames_back_to_back);
    RUN_TEST(chunked_delivery_recovers);
    RUN_TEST(giant_unterminated_frame_then_valid_recovers);
    RUN_TEST(parse_rejects_short_and_too_long);
    RUN_TEST(parse_rejects_bad_version_and_type_with_good_crc);
    return TEST_SUMMARY();
}
