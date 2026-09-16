#include "loopback.h"

#include "packetizer/cobs.h"
#include "packetizer/frame.h"

#include <string.h>

static bool queue_push(loopback_queue_t *q, const uint8_t *buf, size_t len) {
    uint8_t type = 0;
    uint16_t msg_id = 0;
    uint16_t frag_idx = 0;
    bool dup = false;
    bool keep = true;

    if (q->hook != NULL) {
        /* buf still carries its trailing 0x00 delimiter; peel it off before
         * COBS-decoding, same as the real deframer does on the wire. */
        uint8_t plain[PKT_MAX_FRAME];
        size_t plain_len = 0;
        if (len > 0 && cobs_decode(buf, len - 1, plain, sizeof(plain), &plain_len)) {
            struct frame f;
            if (frame_parse(plain, plain_len, &f) == FRAME_OK) {
                type = f.type;
                msg_id = f.msg_id;
                frag_idx = f.frag_idx;
            }
        }
        keep = q->hook(q->hook_user, type, msg_id, frag_idx, buf, len, &dup);
    }

    if (!keep) {
        return true; /* dropped by the hook, not a write() failure */
    }
    if (q->count >= LOOPBACK_MAX_FRAMES) {
        return false;
    }

    memcpy(q->frames[q->count].data, buf, len);
    q->frames[q->count].len = len;
    q->count++;

    if (dup && q->count < LOOPBACK_MAX_FRAMES) {
        memcpy(q->frames[q->count].data, buf, len);
        q->frames[q->count].len = len;
        q->count++;
    }
    return true;
}

static void record_message(loopback_endpoint_t *ep, const uint8_t *data, size_t len) {
    if (ep->message_count >= LOOPBACK_MAX_MESSAGES) {
        return;
    }
    memcpy(ep->messages[ep->message_count].data, data, len);
    ep->messages[ep->message_count].len = len;
    ep->message_count++;
}

static void record_tx_done(loopback_endpoint_t *ep, uint16_t msg_id, pkt_tx_status_t st) {
    if (ep->tx_done_count >= LOOPBACK_MAX_MESSAGES) {
        return;
    }
    ep->tx_done_ids[ep->tx_done_count] = msg_id;
    ep->tx_done_status[ep->tx_done_count] = st;
    ep->tx_done_count++;
}

/* Which physical link direction a callback belongs to is baked into which
 * of these trampolines got registered, not into the user pointer's value
 * (every callback of one endpoint shares the same user = the loopback_t
 * itself), so two independent loopback_t instances never collide. */

static bool write_a(void *user, const uint8_t *buf, size_t len) {
    loopback_t *lb = user;
    return queue_push(&lb->a_to_b, buf, len);
}

static bool write_b(void *user, const uint8_t *buf, size_t len) {
    loopback_t *lb = user;
    return queue_push(&lb->b_to_a, buf, len);
}

static void on_message_a(void *user, const uint8_t *data, size_t len) {
    loopback_t *lb = user;
    record_message(&lb->a, data, len);
}

static void on_message_b(void *user, const uint8_t *data, size_t len) {
    loopback_t *lb = user;
    record_message(&lb->b, data, len);
}

static void on_tx_done_a(void *user, uint16_t msg_id, pkt_tx_status_t st) {
    loopback_t *lb = user;
    record_tx_done(&lb->a, msg_id, st);
}

static void on_tx_done_b(void *user, uint16_t msg_id, pkt_tx_status_t st) {
    loopback_t *lb = user;
    record_tx_done(&lb->b, msg_id, st);
}

void loopback_init(loopback_t *lb, pkt_ctx_t *a, uint8_t epoch_a, pkt_ctx_t *b, uint8_t epoch_b) {
    memset(lb, 0, sizeof(*lb));
    lb->a.ctx = a;
    lb->b.ctx = b;

    pkt_callbacks_t cb_a = { write_a, on_message_a, on_tx_done_a, lb };
    pkt_callbacks_t cb_b = { write_b, on_message_b, on_tx_done_b, lb };
    pkt_init(a, &cb_a, epoch_a);
    pkt_init(b, &cb_b, epoch_b);
}

void loopback_rewire(loopback_t *lb, pkt_ctx_t *a, uint8_t epoch_a, pkt_ctx_t *b) {
    memset(lb, 0, sizeof(*lb));
    lb->a.ctx = a;
    lb->b.ctx = b;

    pkt_callbacks_t cb_a = { write_a, on_message_a, on_tx_done_a, lb };
    pkt_callbacks_t cb_b = { write_b, on_message_b, on_tx_done_b, lb };
    pkt_init(a, &cb_a, epoch_a);
    b->cb = cb_b;
}

void loopback_set_hook(loopback_t *lb, bool a_to_b, loopback_hook_t hook, void *user) {
    loopback_queue_t *q = a_to_b ? &lb->a_to_b : &lb->b_to_a;
    q->hook = hook;
    q->hook_user = user;
}

void loopback_flush_a_to_b(loopback_t *lb) {
    for (size_t i = 0; i < lb->a_to_b.count; i++) {
        pkt_feed(lb->b.ctx, lb->a_to_b.frames[i].data, lb->a_to_b.frames[i].len);
    }
    lb->a_to_b.count = 0;
}

void loopback_flush_b_to_a(loopback_t *lb) {
    for (size_t i = 0; i < lb->b_to_a.count; i++) {
        pkt_feed(lb->a.ctx, lb->b_to_a.frames[i].data, lb->b_to_a.frames[i].len);
    }
    lb->b_to_a.count = 0;
}

void loopback_pump(loopback_t *lb, uint32_t now_ms, int max_rounds) {
    for (int round = 0; round < max_rounds; round++) {
        pkt_poll(lb->a.ctx, now_ms);
        pkt_poll(lb->b.ctx, now_ms);
        size_t sent = lb->a_to_b.count + lb->b_to_a.count;
        loopback_flush_a_to_b(lb);
        loopback_flush_b_to_a(lb);
        if (sent == 0) {
            return;
        }
    }
}
