#include "loopback.h"
#include "test.h"

#include "packetizer/frame.h"

#include <string.h>

static void fill_payload(uint8_t *buf, size_t len, unsigned seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i * 31u);
    }
}

/* Sum of PROTOCOL.md section 7's backoff schedule: PKT_MAX_RETRIES waits,
 * PKT_RTO_MS doubling each time up to PKT_RTO_MAX_MS. Mirrors rto_for() in
 * pkt.c exactly, so a config override (-DPKT_MAX_RETRIES=...) still gets
 * the right expected instant instead of a hardcoded 11000. */
static uint32_t worst_case_timeout_ms(void) {
    uint32_t total = 0;
    uint32_t rto = (uint32_t)PKT_RTO_MS;
    for (int i = 0; i < PKT_MAX_RETRIES; i++) {
        total += rto;
        rto *= 2;
        if (rto > (uint32_t)PKT_RTO_MAX_MS) {
            rto = (uint32_t)PKT_RTO_MAX_MS;
        }
    }
    return total;
}

typedef struct {
    int hits;
} drop_once_t;

static bool drop_frag2_once(void *user, uint8_t type, uint16_t msg_id, uint16_t frag_idx,
                             const uint8_t *frame, size_t len, bool *dup) {
    (void)msg_id;
    (void)frame;
    (void)len;
    (void)dup;
    drop_once_t *c = user;
    if (type == FRAME_DATA && frag_idx == 2 && c->hits == 0) {
        c->hits++;
        return false;
    }
    return true;
}

TEST(lost_fragment_retransmits_once) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);
    drop_once_t dropped = { 0 };
    loopback_set_hook(&lb, true, drop_frag2_once, &dropped);

    uint8_t msg[500]; /* frag_cnt = 4, fits one window burst */
    fill_payload(msg, sizeof(msg), 12);
    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);

    pkt_poll(&tx, 0);
    loopback_flush_a_to_b(&lb);
    loopback_flush_b_to_a(&lb);
    CHECK_EQ(lb.b.message_count, 0u); /* frag 2 missing, bitmap incomplete */

    pkt_poll(&tx, (uint32_t)PKT_RTO_MS); /* frag 2's RTO elapses, retransmitted */
    loopback_flush_a_to_b(&lb);
    loopback_flush_b_to_a(&lb);
    CHECK_EQ(lb.b.message_count, 1u);
    if (lb.b.message_count == 1) {
        CHECK_EQ(lb.b.messages[0].len, sizeof(msg));
        CHECK_MEM(lb.b.messages[0].data, msg, sizeof(msg));
    }

    loopback_pump(&lb, (uint32_t)PKT_RTO_MS, 8); /* settle the ACK/DONE round trip */
    CHECK_EQ(lb.a.tx_done_count, 1u);
    if (lb.a.tx_done_count == 1) {
        CHECK_EQ(lb.a.tx_done_ids[0], id);
        CHECK_EQ(lb.a.tx_done_status[0], PKT_TX_DELIVERED);
    }

    pkt_stats_t st;
    pkt_get_stats(&tx, &st);
    CHECK_EQ(st.retransmissions, 1u);
}

typedef struct {
    bool active;
} drop_while_active_t;

static bool drop_acks_while_active(void *user, uint8_t type, uint16_t msg_id, uint16_t frag_idx,
                                    const uint8_t *frame, size_t len, bool *dup) {
    (void)msg_id;
    (void)frag_idx;
    (void)frame;
    (void)len;
    (void)dup;
    drop_while_active_t *c = user;
    return !(c->active && type == FRAME_ACK);
}

TEST(lost_ack_round_still_delivers) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);
    drop_while_active_t drop = { true };
    loopback_set_hook(&lb, false, drop_acks_while_active, &drop);

    uint8_t msg[1500]; /* frag_cnt = 12, over PKT_TX_WINDOW: first round only covers 8 of them */
    fill_payload(msg, sizeof(msg), 21);
    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);

    pkt_poll(&tx, 0);
    loopback_flush_a_to_b(&lb); /* rx stores all 8, ACKs every one */
    loopback_flush_b_to_a(&lb); /* every ACK in this round is dropped */
    CHECK_EQ(lb.a.tx_done_count, 0u);

    drop.active = false; /* that round's ACKs are gone for good; only a retransmit round succeeds now */

    pkt_poll(&tx, (uint32_t)PKT_RTO_MS); /* every one of the 8 fragments is still unacked, all retransmit */
    loopback_flush_a_to_b(&lb);
    loopback_pump(&lb, (uint32_t)PKT_RTO_MS, 8);

    CHECK_EQ(lb.b.message_count, 1u); /* duplicates of already-stored fragments never redeliver */
    if (lb.b.message_count == 1) {
        CHECK_EQ(lb.b.messages[0].len, sizeof(msg));
        CHECK_MEM(lb.b.messages[0].data, msg, sizeof(msg));
    }
    CHECK_EQ(lb.a.tx_done_count, 1u);
    if (lb.a.tx_done_count == 1) {
        CHECK_EQ(lb.a.tx_done_status[0], PKT_TX_DELIVERED);
    }
}

static bool drop_done_once(void *user, uint8_t type, uint16_t msg_id, uint16_t frag_idx,
                            const uint8_t *frame, size_t len, bool *dup) {
    (void)msg_id;
    (void)frag_idx;
    (void)frame;
    (void)len;
    (void)dup;
    drop_once_t *c = user;
    if (type == FRAME_DONE && c->hits == 0) {
        c->hits++;
        return false;
    }
    return true;
}

TEST(lost_done_delivers_exactly_once) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);
    drop_once_t dropped = { 0 };
    loopback_set_hook(&lb, false, drop_done_once, &dropped);

    uint8_t msg[300]; /* frag_cnt = 3, one window burst, bitmap completes in this same round */
    fill_payload(msg, sizeof(msg), 31);
    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);

    pkt_poll(&tx, 0);
    loopback_flush_a_to_b(&lb); /* rx delivers to the app and sends DONE */
    loopback_flush_b_to_a(&lb); /* the 3 ACKs get through; DONE is eaten once */

    CHECK_EQ(lb.b.message_count, 1u); /* already delivered on the receiving side */
    CHECK_EQ(lb.a.tx_done_count, 0u); /* sender doesn't know yet: DONE never arrived */

    pkt_poll(&tx, (uint32_t)PKT_RTO_MS); /* arms the DONE-wait timer */
    /* One full RTO after arming: the nudge (last fragment, resent) hits the
     * recent-ids cache at rx and gets a fresh DONE back, not redelivery. */
    loopback_pump(&lb, (uint32_t)(2 * PKT_RTO_MS), 8);

    CHECK_EQ(lb.b.message_count, 1u); /* still exactly once */
    CHECK_EQ(lb.a.tx_done_count, 1u);
    if (lb.a.tx_done_count == 1) {
        CHECK_EQ(lb.a.tx_done_ids[0], id);
        CHECK_EQ(lb.a.tx_done_status[0], PKT_TX_DELIVERED);
    }
}

TEST(dead_channel_times_out_once_at_expected_instant) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);
    /* Channel is dead: nothing is ever flushed to rx, so nothing ever ACKs. */

    uint8_t msg[50];
    fill_payload(msg, sizeof(msg), 44);
    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);

    uint32_t give_up_at = worst_case_timeout_ms();

    pkt_poll(&tx, 0); /* initial send, retry_cnt = 0, first deadline = PKT_RTO_MS */
    lb.a_to_b.count = 0;

    uint32_t rto = (uint32_t)PKT_RTO_MS;
    uint32_t t = 0;
    for (int i = 0; i < PKT_MAX_RETRIES - 1; i++) {
        t += rto;
        pkt_poll(&tx, t); /* every one of these is a scheduled retry, none give up yet */
        lb.a_to_b.count = 0;
        CHECK_EQ(lb.a.tx_done_count, 0u);
        rto *= 2;
        if (rto > (uint32_t)PKT_RTO_MAX_MS) {
            rto = (uint32_t)PKT_RTO_MAX_MS;
        }
    }

    pkt_poll(&tx, give_up_at - 1);
    CHECK_EQ(lb.a.tx_done_count, 0u); /* not due yet */

    pkt_poll(&tx, give_up_at);
    CHECK_EQ(lb.a.tx_done_count, 1u);
    if (lb.a.tx_done_count == 1) {
        CHECK_EQ(lb.a.tx_done_ids[0], id);
        CHECK_EQ(lb.a.tx_done_status[0], PKT_TX_TIMEOUT);
    }

    pkt_poll(&tx, give_up_at + 1000); /* slot already freed: on_tx_done never fires a second time */
    CHECK_EQ(lb.a.tx_done_count, 1u);
}

TEST(sender_restart_new_epoch_delivers) {
    pkt_ctx_t tx1;
    pkt_ctx_t rx;
    loopback_t lb1;
    loopback_init(&lb1, &tx1, 0, &rx, 0);

    uint8_t msg1[50];
    fill_payload(msg1, sizeof(msg1), 1);
    uint16_t id1 = 0;
    CHECK_EQ(pkt_send(&tx1, msg1, sizeof(msg1), &id1), PKT_OK);
    loopback_pump(&lb1, 0, 8);
    CHECK_EQ(lb1.b.message_count, 1u);
    CHECK_EQ(lb1.a.tx_done_count, 1u);
    CHECK_EQ(id1, 0u); /* first message off a fresh ctx */

    /* Sender process restarts: fresh pkt_ctx_t, epoch bumped to 1, its own
     * msg_id counter starts back at 0. rx is NOT reinitialized: it still
     * holds (epoch=0, msg_id=0) in its recent-ids cache. */
    pkt_ctx_t tx2;
    loopback_t lb2;
    loopback_rewire(&lb2, &tx2, 1, &rx);

    uint8_t msg2[60];
    fill_payload(msg2, sizeof(msg2), 2);
    uint16_t id2 = 0;
    CHECK_EQ(pkt_send(&tx2, msg2, sizeof(msg2), &id2), PKT_OK);
    CHECK_EQ(id2, 0u); /* collides with msg1's id; epoch is what disambiguates */

    loopback_pump(&lb2, 0, 8);

    CHECK_EQ(lb2.b.message_count, 1u);
    if (lb2.b.message_count == 1) {
        CHECK_EQ(lb2.b.messages[0].len, sizeof(msg2));
        CHECK_MEM(lb2.b.messages[0].data, msg2, sizeof(msg2));
    }
    CHECK_EQ(lb2.a.tx_done_count, 1u);
    if (lb2.a.tx_done_count == 1) {
        CHECK_EQ(lb2.a.tx_done_ids[0], id2);
        CHECK_EQ(lb2.a.tx_done_status[0], PKT_TX_DELIVERED);
    }
}

static void write_u32_le_forged(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

TEST(forged_oversized_data_rejects_sender) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    uint8_t msg[50];
    fill_payload(msg, sizeof(msg), 9);
    uint16_t id = 0;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);
    /* tx's real fragments are never sent; the forged frame below claims the
     * same (epoch, msg_id) as if it were this message's fragment 0. */

    uint8_t meta_payload[PKT_META_SIZE];
    write_u32_le_forged(meta_payload, (uint32_t)PKT_MAX_MESSAGE + 1u);
    write_u32_le_forged(meta_payload + 4, 0xDEADBEEFu); /* msg_crc32: irrelevant, never reached */

    struct frame f;
    f.type = FRAME_DATA;
    f.epoch = 0;
    f.msg_id = id;
    f.frag_idx = 0;
    f.frag_cnt = 1;
    f.payload = meta_payload;
    f.payload_len = sizeof(meta_payload);

    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = frame_encode(&f, wire, sizeof(wire));
    CHECK(wire_len > 0);

    pkt_feed(&rx, wire, wire_len);
    loopback_flush_b_to_a(&lb);

    CHECK_EQ(lb.a.tx_done_count, 1u);
    if (lb.a.tx_done_count == 1) {
        CHECK_EQ(lb.a.tx_done_ids[0], id);
        CHECK_EQ(lb.a.tx_done_status[0], PKT_TX_REJECTED);
    }
}

TEST(tx_slots_full_returns_no_slot) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    uint8_t msg[20];
    fill_payload(msg, sizeof(msg), 3);
    for (size_t i = 0; i < PKT_MAX_TX_MSGS; i++) {
        uint16_t id = 0;
        CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_OK);
    }

    uint16_t id = 999;
    CHECK_EQ(pkt_send(&tx, msg, sizeof(msg), &id), PKT_ERR_NO_SLOT);
    CHECK_EQ(id, 999u); /* untouched on failure */
}

TEST(short_messages_overtake_long_one) {
    pkt_ctx_t tx;
    pkt_ctx_t rx;
    loopback_t lb;
    loopback_init(&lb, &tx, 0, &rx, 0);

    static uint8_t long_msg[PKT_MAX_MESSAGE]; /* frag_cnt = 33, well over PKT_TX_WINDOW */
    fill_payload(long_msg, sizeof(long_msg), 5);
    uint16_t long_id = 0;
    CHECK_EQ(pkt_send(&tx, long_msg, sizeof(long_msg), &long_id), PKT_OK);

    uint8_t short_msgs[3][10];
    uint16_t short_ids[3];
    for (size_t i = 0; i < 3; i++) {
        fill_payload(short_msgs[i], sizeof(short_msgs[i]), (unsigned)(100 + i));
        CHECK_EQ(pkt_send(&tx, short_msgs[i], sizeof(short_msgs[i]), &short_ids[i]), PKT_OK);
    }

    loopback_pump(&lb, 0, 20);

    CHECK_EQ(lb.b.message_count, 4u);
    CHECK_EQ(lb.a.tx_done_count, 4u);

    size_t long_pos = lb.a.tx_done_count;
    size_t short_pos[3] = { lb.a.tx_done_count, lb.a.tx_done_count, lb.a.tx_done_count };
    for (size_t i = 0; i < lb.a.tx_done_count; i++) {
        CHECK_EQ(lb.a.tx_done_status[i], PKT_TX_DELIVERED);
        if (lb.a.tx_done_ids[i] == long_id) {
            long_pos = i;
        }
        for (size_t j = 0; j < 3; j++) {
            if (lb.a.tx_done_ids[i] == short_ids[j]) {
                short_pos[j] = i;
            }
        }
    }

    CHECK(long_pos < lb.a.tx_done_count);
    for (size_t j = 0; j < 3; j++) {
        CHECK(short_pos[j] < lb.a.tx_done_count);
        CHECK(short_pos[j] < long_pos);
    }
}

int main(void) {
    RUN_TEST(lost_fragment_retransmits_once);
    RUN_TEST(lost_ack_round_still_delivers);
    RUN_TEST(lost_done_delivers_exactly_once);
    RUN_TEST(dead_channel_times_out_once_at_expected_instant);
    RUN_TEST(sender_restart_new_epoch_delivers);
    RUN_TEST(forged_oversized_data_rejects_sender);
    RUN_TEST(tx_slots_full_returns_no_slot);
    RUN_TEST(short_messages_overtake_long_one);
    return TEST_SUMMARY();
}
