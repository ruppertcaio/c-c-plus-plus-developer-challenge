#include "loopback.h"
#include "test.h"

#include "packetizer/pkt.h"
#include "transport/impair.h"

#include <string.h>

/* Test-local RNG for message content, deliberately separate from the one
 * impair.c keeps to itself: this one only ever generates bytes to send, it
 * has nothing to do with how the channel treats them. */
static uint32_t test_rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void fill_random(uint8_t *buf, size_t len, uint32_t *rng) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)test_rng_next(rng);
    }
}

#define SOAK_SEEDS 100
#define SOAK_MSGS_PER_SIDE 30
#define SOAK_STEP_MS 20u
#define SOAK_MAX_STEPS 20000 /* 400s simulated per seed; only a counter, not a real sleep */

typedef struct {
    uint8_t data[PKT_MAX_MESSAGE];
    size_t len;
    uint16_t id;
    bool sent;
} soak_msg_t;

static bool find_match(const uint8_t *data, size_t len, const soak_msg_t *pool, size_t pool_n) {
    for (size_t i = 0; i < pool_n; i++) {
        if (pool[i].len == len && memcmp(pool[i].data, data, len) == 0) {
            return true;
        }
    }
    return false;
}

TEST(soak_full_duplex_hostile_channel) {
    static soak_msg_t msgs_a[SOAK_MSGS_PER_SIDE];
    static soak_msg_t msgs_b[SOAK_MSGS_PER_SIDE];

    for (int seed = 1; seed <= SOAK_SEEDS; seed++) {
        int checks_before = test_failures;

        pkt_ctx_t ctx_a;
        pkt_ctx_t ctx_b;
        loopback_t lb;
        loopback_init(&lb, &ctx_a, 0, &ctx_b, 0);

        /* Distinct seeds per direction: otherwise both directions would draw
         * the exact same loss/corrupt/dup/reorder sequence, which is not
         * what two independent physical link directions would do. */
        struct impair_cfg cfg_ab = { 0.10, 0.05, 0.05, 0.02, true, (uint32_t)seed };
        struct impair_cfg cfg_ba = { 0.10, 0.05, 0.05, 0.02, true, (uint32_t)seed + 1000000u };
        loopback_set_impair(&lb, true, &cfg_ab);
        loopback_set_impair(&lb, false, &cfg_ba);

        uint32_t content_rng = (uint32_t)seed * 2654435761u + 1u; /* Knuth multiplicative hash, just needs to differ per seed */
        memset(msgs_a, 0, sizeof(msgs_a));
        memset(msgs_b, 0, sizeof(msgs_b));
        for (int i = 0; i < SOAK_MSGS_PER_SIDE; i++) {
            msgs_a[i].len = test_rng_next(&content_rng) % (PKT_MAX_MESSAGE + 1u);
            fill_random(msgs_a[i].data, msgs_a[i].len, &content_rng);
            msgs_b[i].len = test_rng_next(&content_rng) % (PKT_MAX_MESSAGE + 1u);
            fill_random(msgs_b[i].data, msgs_b[i].len, &content_rng);
        }

        int next_a = 0;
        int next_b = 0;
        uint32_t now = 0;
        for (int step = 0; step < SOAK_MAX_STEPS; step++) {
            while (next_a < SOAK_MSGS_PER_SIDE) {
                uint16_t id = 0;
                if (pkt_send(&ctx_a, msgs_a[next_a].data, msgs_a[next_a].len, &id) != PKT_OK) {
                    break; /* every tx slot busy, retry next step */
                }
                msgs_a[next_a].id = id;
                msgs_a[next_a].sent = true;
                next_a++;
            }
            while (next_b < SOAK_MSGS_PER_SIDE) {
                uint16_t id = 0;
                if (pkt_send(&ctx_b, msgs_b[next_b].data, msgs_b[next_b].len, &id) != PKT_OK) {
                    break;
                }
                msgs_b[next_b].id = id;
                msgs_b[next_b].sent = true;
                next_b++;
            }

            pkt_poll(&ctx_a, now);
            pkt_poll(&ctx_b, now);
            loopback_flush_a_to_b(&lb);
            loopback_flush_b_to_a(&lb);

            bool all_sent = (next_a == SOAK_MSGS_PER_SIDE) && (next_b == SOAK_MSGS_PER_SIDE);
            bool all_done = (lb.a.tx_done_count == SOAK_MSGS_PER_SIDE) &&
                             (lb.b.tx_done_count == SOAK_MSGS_PER_SIDE);
            if (all_sent && all_done) {
                break;
            }
            now += SOAK_STEP_MS;
        }
        loopback_settle(&lb, now, 16); /* release anything impair still held for reorder */

        CHECK_EQ(next_a, SOAK_MSGS_PER_SIDE);
        CHECK_EQ(next_b, SOAK_MSGS_PER_SIDE);
        CHECK_EQ(lb.a.tx_done_count, SOAK_MSGS_PER_SIDE);
        CHECK_EQ(lb.b.tx_done_count, SOAK_MSGS_PER_SIDE);

        /* Every send gets exactly one on_tx_done: each recorded id traces
         * back to a message this side actually sent, and traces back exactly
         * once (a repeat here would mean the same id reported twice). */
        bool seen_a[SOAK_MSGS_PER_SIDE] = { 0 };
        int delivered_by_a = 0;
        for (size_t i = 0; i < lb.a.tx_done_count; i++) {
            int idx = -1;
            for (int j = 0; j < SOAK_MSGS_PER_SIDE; j++) {
                if (msgs_a[j].sent && msgs_a[j].id == lb.a.tx_done_ids[i]) {
                    idx = j;
                    break;
                }
            }
            CHECK(idx >= 0);
            if (idx >= 0) {
                CHECK(!seen_a[idx]);
                seen_a[idx] = true;
                if (lb.a.tx_done_status[i] == PKT_TX_DELIVERED) {
                    delivered_by_a++;
                }
            }
        }
        bool seen_b[SOAK_MSGS_PER_SIDE] = { 0 };
        int delivered_by_b = 0;
        for (size_t i = 0; i < lb.b.tx_done_count; i++) {
            int idx = -1;
            for (int j = 0; j < SOAK_MSGS_PER_SIDE; j++) {
                if (msgs_b[j].sent && msgs_b[j].id == lb.b.tx_done_ids[i]) {
                    idx = j;
                    break;
                }
            }
            CHECK(idx >= 0);
            if (idx >= 0) {
                CHECK(!seen_b[idx]);
                seen_b[idx] = true;
                if (lb.b.tx_done_status[i] == PKT_TX_DELIVERED) {
                    delivered_by_b++;
                }
            }
        }

        /* DELIVERED implies the peer already ran on_message before sending
         * DONE (accept_fragment() in pkt.c), so #DELIVERED can never exceed
         * the peer's message_count: that direction is a hard bound, and
         * catches silent double-delivery when it's violated in the other
         * direction (peer_count > what any DELIVERED accounting explains).
         * It is not an equality: a corrupted trailing 0x00 merges a frame
         * with whatever follows it into one lost blob (PROTOCOL.md section
         * 3), so an already-delivered message's DONE, and every nudge retry
         * meant to recover it, can all be swallowed that way, timing the
         * sender out on a message the peer received perfectly fine. */
        CHECK(delivered_by_a <= (int)lb.b.message_count);
        CHECK(delivered_by_b <= (int)lb.a.message_count);

        for (size_t i = 0; i < lb.b.message_count; i++) {
            CHECK(find_match(lb.b.messages[i].data, lb.b.messages[i].len, msgs_a, SOAK_MSGS_PER_SIDE));
        }
        for (size_t i = 0; i < lb.a.message_count; i++) {
            CHECK(find_match(lb.a.messages[i].data, lb.a.messages[i].len, msgs_b, SOAK_MSGS_PER_SIDE));
        }

        if (test_failures != checks_before) {
            fprintf(stderr, "soak_full_duplex_hostile_channel: failed at seed=%d\n", seed);
        }
    }
}

TEST(broken_channel_recovers_without_reinit) {
    uint32_t seed = 4242;
    uint32_t rng = seed;

    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    struct impair_cfg dead = { 1.0, 0.0, 0.0, 0.0, false, seed };
    loopback_set_impair(&lb, true, &dead);  /* tx -> rx: everything lost */
    loopback_set_impair(&lb, false, &dead); /* rx -> tx: even if something got through, no ACK comes back */

    uint8_t msg1[64];
    fill_random(msg1, sizeof(msg1), &rng);
    uint16_t id1 = 0;
    CHECK_EQ(pkt_send(&tx, msg1, sizeof(msg1), &id1), PKT_OK);

    uint32_t now = 0;
    for (int step = 0; step < 5000 && lb.a.tx_done_count == 0; step++) {
        now += 50;
        pkt_poll(&tx, now);
        loopback_flush_a_to_b(&lb);
        loopback_flush_b_to_a(&lb);
    }

    CHECK_EQ(lb.a.tx_done_count, 1u);
    if (lb.a.tx_done_count == 1) {
        CHECK_EQ(lb.a.tx_done_ids[0], id1);
        CHECK_EQ(lb.a.tx_done_status[0], PKT_TX_TIMEOUT);
    }

    /* Channel comes back: no pkt_init on tx or rx, no fresh loopback_t, just
     * a new impair config with loss back at zero on both directions. */
    struct impair_cfg clear = { 0.0, 0.0, 0.0, 0.0, false, seed + 1u };
    loopback_set_impair(&lb, true, &clear);
    loopback_set_impair(&lb, false, &clear);

    uint8_t msg2[128];
    fill_random(msg2, sizeof(msg2), &rng);
    uint16_t id2 = 0;
    CHECK_EQ(pkt_send(&tx, msg2, sizeof(msg2), &id2), PKT_OK);
    loopback_pump(&lb, now + (uint32_t)PKT_RTO_MS, 16);

    CHECK_EQ(lb.b.message_count, 1u);
    if (lb.b.message_count == 1) {
        CHECK_EQ(lb.b.messages[0].len, sizeof(msg2));
        CHECK_MEM(lb.b.messages[0].data, msg2, sizeof(msg2));
    }
    CHECK_EQ(lb.a.tx_done_count, 2u);
    if (lb.a.tx_done_count == 2) {
        CHECK_EQ(lb.a.tx_done_ids[1], id2);
        CHECK_EQ(lb.a.tx_done_status[1], PKT_TX_DELIVERED);
    }
}

static void fuzz_on_message(void *user, const uint8_t *data, size_t len) {
    (void)data;
    (void)len;
    int *count = user;
    (*count)++;
}

static bool fuzz_write(void *user, const uint8_t *buf, size_t len) {
    (void)user;
    (void)buf;
    (void)len;
    return true; /* this endpoint never actually needs to send anything back */
}

static void fuzz_on_tx_done(void *user, uint16_t msg_id, pkt_tx_status_t st) {
    (void)user;
    (void)msg_id;
    (void)st;
}

TEST(fuzz_receiver_survives_random_bytes) {
    uint32_t seed = 777;
    uint32_t rng = seed;
    int message_count = 0;

    pkt_callbacks_t cb = { fuzz_write, fuzz_on_message, fuzz_on_tx_done, &message_count };
    pkt_ctx_t rx;
    pkt_init(&rx, &cb, 0);

    static uint8_t chunk[4096];
    const size_t total_bytes = 1024u * 1024u;
    size_t sent = 0;
    while (sent < total_bytes) {
        size_t n = sizeof(chunk);
        if (sent + n > total_bytes) {
            n = total_bytes - sent;
        }
        fill_random(chunk, n, &rng);
        pkt_feed(&rx, chunk, n);
        sent += n;
    }

    if (message_count != 0) {
        fprintf(stderr, "fuzz_receiver_survives_random_bytes: failed with seed=%u\n", (unsigned)seed);
    }
    CHECK_EQ(message_count, 0);
}

int main(void) {
    RUN_TEST(soak_full_duplex_hostile_channel);
    RUN_TEST(broken_channel_recovers_without_reinit);
    RUN_TEST(fuzz_receiver_survives_random_bytes);
    return TEST_SUMMARY();
}
