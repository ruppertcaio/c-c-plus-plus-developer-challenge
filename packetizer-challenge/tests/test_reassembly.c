#include "loopback.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static void fill_payload(uint8_t *buf, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i * 31u);
    }
}

TEST(size_roundtrip_all_boundaries) {
    static const size_t sizes[] = {
        0, 1, PKT_MAX_PAYLOAD - 8, PKT_MAX_PAYLOAD - 7, PKT_MAX_PAYLOAD, 1000, PKT_MAX_MESSAGE
    };

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        pkt_ctx_t tx;
        pkt_ctx_t rx;
        loopback_t lb;
        loopback_init(&lb, &tx, 0, &rx, 0);

        uint8_t msg[PKT_MAX_MESSAGE];
        fill_payload(msg, sizes[s], (unsigned)s);

        uint16_t id = 0;
        CHECK_EQ(pkt_send(&tx, msg, sizes[s], &id), PKT_OK);

        /* PKT_MAX_MESSAGE needs several send/ack rounds against an 8-fragment
         * window (33 fragments total), so drive both directions until the
         * ACK/DONE round trip settles instead of a single poll+flush. */
        loopback_pump(&lb, 0, 8);

        CHECK_EQ(lb.b.message_count, 1u);
        if (lb.b.message_count == 1) {
            CHECK_EQ(lb.b.messages[0].len, sizes[s]);
            CHECK_MEM(lb.b.messages[0].data, msg, sizes[s]);
        }
        CHECK_EQ(lb.a.tx_done_count, 1u);
        if (lb.a.tx_done_count == 1) {
            CHECK_EQ(lb.a.tx_done_ids[0], id);
            CHECK_EQ(lb.a.tx_done_status[0], PKT_TX_DELIVERED);
        }
    }
}

TEST(oversize_message_rejected) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    static uint8_t msg[PKT_MAX_MESSAGE + 1];
    uint16_t id = 999;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_ERR_TOO_BIG);
    CHECK_EQ(id, 999u); /* untouched on failure */

    pkt_poll(&tx, 0);
    CHECK_EQ(lb.a_to_b.count, 0u);
}

TEST(fragments_reverse_order_recovers) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    uint8_t msg[1000];
    fill_payload(msg, sizeof(msg), 11);

    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);
    pkt_poll(&tx, 0);

    size_t n = lb.a_to_b.count;
    CHECK(n > 1);
    /* Fragment 0 has to stay first: without NACK/retry (deferred to the
     * reliability commit), a fragment arriving before its session exists is
     * dropped for good, not buffered. Reversing everything else still
     * exercises out-of-order reassembly once the session is open. */
    for (size_t i = 1; i < 1 + (n - 1) / 2; i++) {
        loopback_frame_t tmp = lb.a_to_b.frames[i];
        lb.a_to_b.frames[i] = lb.a_to_b.frames[n - i];
        lb.a_to_b.frames[n - i] = tmp;
    }

    loopback_flush_a_to_b(&lb);
    CHECK_EQ(lb.b.message_count, 1u);
    if (lb.b.message_count == 1) {
        CHECK_EQ(lb.b.messages[0].len, sizeof(msg));
        CHECK_MEM(lb.b.messages[0].data, msg, sizeof(msg));
    }
}

TEST(fragments_shuffled_order_recovers) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    uint8_t msg[1000];
    fill_payload(msg, sizeof(msg), 22);

    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);
    pkt_poll(&tx, 0);

    size_t n = lb.a_to_b.count;
    CHECK(n > 1);
    /* Fragment 0 stays first, same reasoning as the reverse-order test:
     * without retry, a fragment arriving before its session exists is lost
     * for good. Fisher-Yates over indices [1, n) only. */
    srand(777u);
    for (size_t i = n; i > 2; i--) {
        size_t j = 1 + (size_t)rand() % (i - 1);
        loopback_frame_t tmp = lb.a_to_b.frames[i - 1];
        lb.a_to_b.frames[i - 1] = lb.a_to_b.frames[j];
        lb.a_to_b.frames[j] = tmp;
    }

    loopback_flush_a_to_b(&lb);
    CHECK_EQ(lb.b.message_count, 1u);
    if (lb.b.message_count == 1) {
        CHECK_EQ(lb.b.messages[0].len, sizeof(msg));
        CHECK_MEM(lb.b.messages[0].data, msg, sizeof(msg));
    }
}

static bool dup_every_frame(void *user, uint8_t type, uint16_t msg_id, uint16_t frag_idx,
                             const uint8_t *frame, size_t len, bool *dup) {
    (void)user;
    (void)type;
    (void)msg_id;
    (void)frag_idx;
    (void)frame;
    (void)len;
    *dup = true;
    return true;
}

TEST(all_fragments_duplicated_delivers_once) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);
    loopback_set_hook(&lb, true, dup_every_frame, NULL);

    uint8_t msg[500];
    fill_payload(msg, sizeof(msg), 33);

    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);
    pkt_poll(&tx, 0);
    loopback_flush_a_to_b(&lb);

    CHECK_EQ(lb.b.message_count, 1u);
    if (lb.b.message_count == 1) {
        CHECK_EQ(lb.b.messages[0].len, sizeof(msg));
        CHECK_MEM(lb.b.messages[0].data, msg, sizeof(msg));
    }

    /* The last fragment's duplicate copy arrives right after the one that
     * completes the bitmap, by which point the session is already freed and
     * (epoch, msg_id) is in the recent-ids cache: that straggler gets a
     * resent DONE, not counted as a duplicate fragment. Every other
     * fragment's duplicate lands while its session is still open, so the
     * achievable count here is frag_cnt - 1, not frag_cnt. */
    size_t frag_cnt = (sizeof(msg) + PKT_META_SIZE + PKT_MAX_PAYLOAD - 1) / PKT_MAX_PAYLOAD;
    pkt_stats_t st;
    pkt_get_stats(&rx, &st);
    CHECK_EQ(st.duplicate_frags, frag_cnt - 1);
}

TEST(two_messages_interleaved_both_deliver) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    uint8_t msg1[300];
    uint8_t msg2[400];
    fill_payload(msg1, sizeof(msg1), 44);
    fill_payload(msg2, sizeof(msg2), 55);

    uint16_t id1 = 0;
    uint16_t id2 = 0;
    CHECK_EQ(pkt_send(&tx, msg1, sizeof(msg1), &id1), PKT_OK);
    pkt_poll(&tx, 0);
    size_t n1 = lb.a_to_b.count;

    CHECK_EQ(pkt_send(&tx, msg2, sizeof(msg2), &id2), PKT_OK);
    pkt_poll(&tx, 0);
    size_t n_total = lb.a_to_b.count;

    /* The two messages sit as contiguous runs (msg1's fragments, then
     * msg2's). Interleave them here so the receiver has to track both
     * sessions concurrently instead of finishing one before the other
     * starts. */
    loopback_frame_t merged[LOOPBACK_MAX_FRAMES];
    size_t i1 = 0;
    size_t i2 = n1;
    size_t m = 0;
    while (i1 < n1 || i2 < n_total) {
        if (i1 < n1) {
            merged[m++] = lb.a_to_b.frames[i1++];
        }
        if (i2 < n_total) {
            merged[m++] = lb.a_to_b.frames[i2++];
        }
    }
    memcpy(lb.a_to_b.frames, merged, m * sizeof(merged[0]));

    loopback_flush_a_to_b(&lb);

    CHECK_EQ(lb.b.message_count, 2u);
    bool got1 = false;
    bool got2 = false;
    for (size_t i = 0; i < lb.b.message_count; i++) {
        if (lb.b.messages[i].len == sizeof(msg1) && memcmp(lb.b.messages[i].data, msg1, sizeof(msg1)) == 0) {
            got1 = true;
        }
        if (lb.b.messages[i].len == sizeof(msg2) && memcmp(lb.b.messages[i].data, msg2, sizeof(msg2)) == 0) {
            got2 = true;
        }
    }
    CHECK(got1);
    CHECK(got2);
}

static bool drop_frag1(void *user, uint8_t type, uint16_t msg_id, uint16_t frag_idx,
                        const uint8_t *frame, size_t len, bool *dup) {
    (void)user;
    (void)msg_id;
    (void)frame;
    (void)len;
    (void)dup;
    return !(type == FRAME_DATA && frag_idx == 1);
}

TEST(abandoned_session_expires_and_frees_slot) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);
    loopback_set_hook(&lb, true, drop_frag1, NULL);

    uint8_t stuck[300]; /* forces frag_cnt > 1, so dropping frag 1 leaves a gap */
    fill_payload(stuck, sizeof(stuck), 66);

    for (size_t i = 0; i < PKT_MAX_RX_SESSIONS; i++) {
        uint16_t id = 0;
        CHECK_EQ(pkt_send(&tx, stuck, sizeof(stuck), &id), PKT_OK);
        pkt_poll(&tx, 0);
        loopback_flush_a_to_b(&lb);
    }
    pkt_poll(&rx, 0); /* stamps last_rx_ms for both stuck sessions */

    CHECK_EQ(lb.b.message_count, 0u);

    pkt_poll(&rx, (uint32_t)PKT_RX_TIMEOUT_MS + 1u);

    pkt_stats_t st;
    pkt_get_stats(&rx, &st);
    CHECK_EQ(st.sessions_expired, (uint32_t)PKT_MAX_RX_SESSIONS);

    loopback_set_hook(&lb, true, NULL, NULL);

    uint8_t recovery[100];
    fill_payload(recovery, sizeof(recovery), 77);
    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, recovery, sizeof(recovery), &id), PKT_OK);
    pkt_poll(&tx, 0);
    loopback_flush_a_to_b(&lb);

    CHECK_EQ(lb.b.message_count, 1u);
    if (lb.b.message_count == 1) {
        CHECK_EQ(lb.b.messages[0].len, sizeof(recovery));
        CHECK_MEM(lb.b.messages[0].data, recovery, sizeof(recovery));
    }
}

int main(void) {
    RUN_TEST(size_roundtrip_all_boundaries);
    RUN_TEST(oversize_message_rejected);
    RUN_TEST(fragments_reverse_order_recovers);
    RUN_TEST(fragments_shuffled_order_recovers);
    RUN_TEST(all_fragments_duplicated_delivers_once);
    RUN_TEST(two_messages_interleaved_both_deliver);
    RUN_TEST(abandoned_session_expires_and_frees_slot);
    return TEST_SUMMARY();
}
