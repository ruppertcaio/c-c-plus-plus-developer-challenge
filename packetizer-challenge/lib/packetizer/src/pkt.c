#include "packetizer/pkt.h"

#include "packetizer/cobs.h"
#include "packetizer/crc.h"

#include <string.h>

enum {
    TX_FREE = 0,
    TX_ACTIVE,
};

enum {
    RX_FREE = 0,
    RX_ACTIVE,
};

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* PROTOCOL.md section 4-5 fragmentation, parameterized by an actual total_len
 * instead of the compile-time PKT_MAX_MESSAGE the PKT_MAX_FRAGS macro uses. */

static uint16_t frag_count_for(uint32_t total_len) {
    size_t n = ((size_t)total_len + (size_t)PKT_META_SIZE + (size_t)PKT_MAX_PAYLOAD - 1) /
               (size_t)PKT_MAX_PAYLOAD;
    return (uint16_t)n;
}

static size_t frag_offset(uint16_t idx) {
    if (idx == 0) {
        return 0;
    }
    return (size_t)(PKT_MAX_PAYLOAD - PKT_META_SIZE) + (size_t)(idx - 1) * (size_t)PKT_MAX_PAYLOAD;
}

static size_t frag_payload_len(uint32_t total_len, uint16_t idx, uint16_t cnt) {
    if (idx == 0) {
        if (cnt == 1) {
            return (size_t)total_len;
        }
        return (size_t)PKT_MAX_PAYLOAD - (size_t)PKT_META_SIZE;
    }
    if (idx < (uint16_t)(cnt - 1)) {
        return (size_t)PKT_MAX_PAYLOAD;
    }
    return (size_t)total_len - frag_offset(idx);
}

/* frag_cnt is asserted <= 64 in pkt_config.h; a plain 1 << frag_cnt would be
 * undefined behavior for frag_cnt == 64 (shift by the full width of the type). */
static uint64_t full_mask(uint16_t frag_cnt) {
    if (frag_cnt >= 64) {
        return ~(uint64_t)0;
    }
    return ((uint64_t)1 << frag_cnt) - 1;
}

/* PROTOCOL.md section 7: PKT_RTO_MS doubling per timeout, capped at
 * PKT_RTO_MAX_MS. retry_cnt = 0 is the wait after the first transmission. */
static uint32_t rto_for(uint8_t retry_cnt) {
    if (retry_cnt >= 31) {
        return (uint32_t)PKT_RTO_MAX_MS;
    }
    uint32_t rto = (uint32_t)PKT_RTO_MS << retry_cnt;
    if (rto > (uint32_t)PKT_RTO_MAX_MS || rto < (uint32_t)PKT_RTO_MS) {
        return (uint32_t)PKT_RTO_MAX_MS;
    }
    return rto;
}

static pkt_rx_session_t *find_rx_session(pkt_ctx_t *ctx, uint8_t epoch, uint16_t msg_id) {
    for (size_t i = 0; i < PKT_MAX_RX_SESSIONS; i++) {
        pkt_rx_session_t *s = &ctx->rx_sessions[i];
        if (s->state == RX_ACTIVE && s->epoch == epoch && s->msg_id == msg_id) {
            return s;
        }
    }
    return NULL;
}

static pkt_rx_session_t *alloc_rx_session(pkt_ctx_t *ctx) {
    for (size_t i = 0; i < PKT_MAX_RX_SESSIONS; i++) {
        if (ctx->rx_sessions[i].state == RX_FREE) {
            return &ctx->rx_sessions[i];
        }
    }
    return NULL;
}

/* Only ever matches a slot still actively tracking that exact message: a
 * freed slot (never reused yet, or reused for something else entirely) is
 * never a match, regardless of what stale epoch/msg_id bytes it still holds.
 * This is what makes a late-arriving duplicate ACK/DONE/NACK safe to just
 * ignore instead of accidentally poking a slot that has moved on. */
static pkt_tx_slot_t *find_tx_slot(pkt_ctx_t *ctx, uint8_t epoch, uint16_t msg_id) {
    for (size_t i = 0; i < PKT_MAX_TX_MSGS; i++) {
        pkt_tx_slot_t *s = &ctx->tx_slots[i];
        if (s->state == TX_ACTIVE && s->epoch == epoch && s->msg_id == msg_id) {
            return s;
        }
    }
    return NULL;
}

static bool recent_cache_lookup(const pkt_recent_cache_t *c, uint8_t epoch, uint16_t msg_id) {
    for (size_t i = 0; i < c->count; i++) {
        if (c->entries[i].epoch == epoch && c->entries[i].msg_id == msg_id) {
            return true;
        }
    }
    return false;
}

static void recent_cache_insert(pkt_recent_cache_t *c, uint8_t epoch, uint16_t msg_id) {
    c->entries[c->cursor].epoch = epoch;
    c->entries[c->cursor].msg_id = msg_id;
    c->cursor = (uint8_t)((c->cursor + 1u) % (uint8_t)PKT_RECENT_IDS);
    if (c->count < (uint8_t)PKT_RECENT_IDS) {
        c->count++;
    }
}

/* Shared by send_ack/send_done/send_nack: builds one control frame (empty or
 * 1-byte payload) and hands it to write(). Fire-and-forget, like every other
 * frame type here — if it's lost, the sender's own RTO or the receiver's own
 * dedup logic covers for it, so there is nothing to retry from this side. */
static void send_control(pkt_ctx_t *ctx, uint8_t type, uint8_t epoch, uint16_t msg_id,
                          uint16_t frag_idx, const uint8_t *payload, size_t payload_len) {
    struct frame f;
    f.type = type;
    f.epoch = epoch;
    f.msg_id = msg_id;
    f.frag_idx = frag_idx;
    f.frag_cnt = 0;
    f.payload = payload;
    f.payload_len = payload_len;

    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = frame_encode(&f, wire, sizeof(wire));
    if (wire_len == 0 || !ctx->cb.write(ctx->cb.user, wire, wire_len)) {
        return;
    }

    ctx->stats.frames_sent++;
    ctx->stats.bytes_sent += (uint32_t)wire_len;
    switch (type) {
    case FRAME_ACK:
        ctx->stats.acks_sent++;
        break;
    case FRAME_DONE:
        ctx->stats.dones_sent++;
        break;
    case FRAME_NACK:
        ctx->stats.nacks_sent++;
        break;
    default:
        break;
    }
}

static void send_ack(pkt_ctx_t *ctx, uint8_t epoch, uint16_t msg_id, uint16_t frag_idx) {
    send_control(ctx, FRAME_ACK, epoch, msg_id, frag_idx, NULL, 0);
}

static void send_done(pkt_ctx_t *ctx, uint8_t epoch, uint16_t msg_id) {
    send_control(ctx, FRAME_DONE, epoch, msg_id, 0, NULL, 0);
}

static void send_nack(pkt_ctx_t *ctx, uint8_t epoch, uint16_t msg_id, uint16_t frag_idx,
                       uint8_t reason) {
    uint8_t payload = reason;
    send_control(ctx, FRAME_NACK, epoch, msg_id, frag_idx, &payload, 1);
}

/* Dedup-then-write for one already-validated fragment, and the completion
 * check that follows it. Shared by the frag_idx == 0 and frag_idx > 0 paths
 * in handle_data_frame(). Every valid stored fragment gets an ACK, including
 * a duplicate: the sender only learns a fragment landed by seeing its ACK. */
static void accept_fragment(pkt_ctx_t *ctx, pkt_rx_session_t *s, uint16_t frag_idx,
                             const uint8_t *frag_data, size_t frag_len) {
    uint64_t bit = (uint64_t)1 << frag_idx;
    if (s->received_mask & bit) {
        ctx->stats.duplicate_frags++;
        send_ack(ctx, s->epoch, s->msg_id, frag_idx);
        return;
    }

    memcpy(s->data + frag_offset(frag_idx), frag_data, frag_len);
    s->received_mask |= bit;
    send_ack(ctx, s->epoch, s->msg_id, frag_idx);

    if (s->received_mask != full_mask(s->frag_cnt)) {
        return;
    }

    uint32_t crc = crc32_finalize(crc32_update(PKT_CRC32_INIT, s->data, s->total_len));
    if (crc == s->msg_crc32) {
        ctx->stats.payload_delivered += s->total_len;
        if (ctx->cb.on_message != NULL) {
            ctx->cb.on_message(ctx->cb.user, s->data, s->total_len);
        }
        recent_cache_insert(&ctx->recent, s->epoch, s->msg_id);
        send_done(ctx, s->epoch, s->msg_id);
    } else {
        ctx->stats.message_crc_errors++;
        send_nack(ctx, s->epoch, s->msg_id, frag_idx, FRAME_NACK_BAD_CRC);
    }
    s->state = RX_FREE;
}

/* PROTOCOL.md section 5's validation order for fragment 0. */
static void handle_data_frame_first(pkt_ctx_t *ctx, const struct frame *f) {
    if (f->frag_cnt == 0 || f->payload_len < (size_t)PKT_META_SIZE) {
        return;
    }

    uint32_t total_len = read_u32_le(f->payload);
    uint32_t msg_crc32 = read_u32_le(f->payload + 4);

    if (total_len > (uint32_t)PKT_MAX_MESSAGE) {
        send_nack(ctx, f->epoch, f->msg_id, f->frag_idx, FRAME_NACK_TOO_BIG);
        return;
    }
    if (f->frag_cnt != frag_count_for(total_len) ||
        f->payload_len - (size_t)PKT_META_SIZE != frag_payload_len(total_len, 0, f->frag_cnt)) {
        send_nack(ctx, f->epoch, f->msg_id, f->frag_idx, FRAME_NACK_MALFORMED);
        return;
    }

    /* Checked only after the frame passes its own internal consistency
     * checks above: a forged frame does not get to ride a real cached
     * message's identity past TOO_BIG/MALFORMED just by matching its ids. */
    if (recent_cache_lookup(&ctx->recent, f->epoch, f->msg_id)) {
        send_done(ctx, f->epoch, f->msg_id);
        return;
    }

    pkt_rx_session_t *s = find_rx_session(ctx, f->epoch, f->msg_id);
    if (s != NULL) {
        /* Resend of fragment 0 against an in-progress session: only
         * proceed if it still describes the same message. */
        if (s->total_len != total_len || s->frag_cnt != f->frag_cnt) {
            return;
        }
    } else {
        s = alloc_rx_session(ctx);
        if (s == NULL) {
            send_nack(ctx, f->epoch, f->msg_id, f->frag_idx, FRAME_NACK_NO_SESSION);
            return;
        }
        s->epoch = f->epoch;
        s->msg_id = f->msg_id;
        s->total_len = total_len;
        s->frag_cnt = f->frag_cnt;
        s->msg_crc32 = msg_crc32;
        s->received_mask = 0;
        s->state = RX_ACTIVE;
    }
    s->touched = true;
    accept_fragment(ctx, s, 0, f->payload + PKT_META_SIZE, f->payload_len - (size_t)PKT_META_SIZE);
}

/* PROTOCOL.md section 5's validation for frag_idx > 0, plus the recent-ids
 * cache check up front: section 7's "DONE lost" recovery nudges with the
 * *last* fragment of the message, not necessarily fragment 0, so a
 * duplicate-of-completed check keyed only on "session exists" would miss it. */
static void handle_data_frame_rest(pkt_ctx_t *ctx, const struct frame *f) {
    if (recent_cache_lookup(&ctx->recent, f->epoch, f->msg_id)) {
        send_done(ctx, f->epoch, f->msg_id);
        return;
    }

    pkt_rx_session_t *s = find_rx_session(ctx, f->epoch, f->msg_id);
    if (s == NULL) {
        send_nack(ctx, f->epoch, f->msg_id, f->frag_idx, FRAME_NACK_NO_SESSION);
        return;
    }
    if (f->frag_idx >= s->frag_cnt || f->payload_len != frag_payload_len(s->total_len, f->frag_idx, s->frag_cnt)) {
        send_nack(ctx, f->epoch, f->msg_id, f->frag_idx, FRAME_NACK_MALFORMED);
        return;
    }
    s->touched = true;
    accept_fragment(ctx, s, f->frag_idx, f->payload, f->payload_len);
}

static void handle_data_frame(pkt_ctx_t *ctx, const struct frame *f) {
    if (f->frag_idx == 0) {
        handle_data_frame_first(ctx, f);
    } else {
        handle_data_frame_rest(ctx, f);
    }
}

/* Builds and writes fragment frag_idx of slot, reading straight from
 * slot->msg: TX is zero-copy, there is never a buffered copy to rebuild a
 * retransmit from, first send or tenth. */
static bool send_fragment_at(pkt_ctx_t *ctx, pkt_tx_slot_t *slot, uint16_t frag_idx) {
    struct frame f;
    f.type = FRAME_DATA;
    f.epoch = slot->epoch;
    f.msg_id = slot->msg_id;
    f.frag_idx = frag_idx;
    f.frag_cnt = slot->frag_cnt;

    size_t len = frag_payload_len(slot->total_len, frag_idx, slot->frag_cnt);

    /* Fragment 0's payload is total_len+msg_crc32 followed by message bytes,
     * which do not sit next to each other in memory, so it needs a local
     * staging buffer. Every other fragment points straight into the
     * caller's buffer: frame_encode() copies from it once, synchronously. */
    uint8_t meta_payload[PKT_MAX_PAYLOAD];
    if (frag_idx == 0) {
        write_u32_le(meta_payload, slot->total_len);
        write_u32_le(meta_payload + 4, slot->msg_crc32);
        if (len > 0) {
            memcpy(meta_payload + PKT_META_SIZE, slot->msg, len);
        }
        f.payload = meta_payload;
        f.payload_len = (size_t)PKT_META_SIZE + len;
    } else {
        f.payload = slot->msg + frag_offset(frag_idx);
        f.payload_len = len;
    }

    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = frame_encode(&f, wire, sizeof(wire));

    if (!ctx->cb.write(ctx->cb.user, wire, wire_len)) {
        return false;
    }

    ctx->stats.frames_sent++;
    ctx->stats.bytes_sent += (uint32_t)wire_len;
    return true;
}

static void tx_slot_free(pkt_tx_slot_t *slot) {
    slot->state = TX_FREE;
}

static void handle_ack_frame(pkt_ctx_t *ctx, const struct frame *f) {
    ctx->stats.acks_received++;
    pkt_tx_slot_t *slot = find_tx_slot(ctx, f->epoch, f->msg_id);
    if (slot == NULL || f->frag_idx >= slot->frag_cnt) {
        return;
    }

    for (size_t i = 0; i < PKT_TX_WINDOW; i++) {
        if (slot->flight[i].active && slot->flight[i].frag_idx == f->frag_idx) {
            slot->flight[i].active = false;
            break;
        }
    }
    slot->acked_mask |= (uint64_t)1 << f->frag_idx;

    if (!slot->done_wait && slot->acked_mask == full_mask(slot->frag_cnt)) {
        slot->done_wait = true;
        slot->done_wait_armed = false;
        slot->done_retry_cnt = 0;
    }
}

static void handle_done_frame(pkt_ctx_t *ctx, const struct frame *f) {
    ctx->stats.dones_received++;
    pkt_tx_slot_t *slot = find_tx_slot(ctx, f->epoch, f->msg_id);
    if (slot == NULL) {
        return;
    }
    ctx->stats.payload_sent += slot->total_len;
    if (ctx->cb.on_tx_done != NULL) {
        ctx->cb.on_tx_done(ctx->cb.user, slot->msg_id, PKT_TX_DELIVERED);
    }
    tx_slot_free(slot);
}

/* The receiver has no memory of this message at all (fragment 0 never
 * arrived, or its session was evicted), so every fragment the sender thinks
 * is acked is stale from the receiver's point of view: reset the window and
 * restart from fragment 0. Fragment 0's own retry_cnt survives repeated
 * NO_SESSIONs (kept in flight[0]) so the message can still time out instead
 * of retrying forever against a peer that keeps rejecting it. */
static void handle_nack_no_session(pkt_ctx_t *ctx, pkt_tx_slot_t *slot) {
    uint8_t next_retry = 0;
    for (size_t i = 0; i < PKT_TX_WINDOW; i++) {
        if (slot->flight[i].active && slot->flight[i].frag_idx == 0) {
            if (slot->flight[i].retry_cnt >= (uint8_t)(PKT_MAX_RETRIES - 1)) {
                if (ctx->cb.on_tx_done != NULL) {
                    ctx->cb.on_tx_done(ctx->cb.user, slot->msg_id, PKT_TX_TIMEOUT);
                }
                tx_slot_free(slot);
                return;
            }
            next_retry = (uint8_t)(slot->flight[i].retry_cnt + 1u);
            break;
        }
    }

    for (size_t i = 0; i < PKT_TX_WINDOW; i++) {
        slot->flight[i].active = false;
    }
    slot->flight[0].active = true;
    slot->flight[0].frag_idx = 0;
    slot->flight[0].retry_cnt = next_retry;
    slot->flight[0].armed = false; /* already (best-effort) resent below; pkt_poll only stamps the deadline */

    slot->acked_mask = 0;
    slot->next_frag = 1;
    slot->done_wait = false;

    if (send_fragment_at(ctx, slot, 0)) {
        ctx->stats.retransmissions++;
    }
}

static void handle_nack_frame(pkt_ctx_t *ctx, const struct frame *f) {
    ctx->stats.nacks_received++;
    if (f->payload_len < 1) {
        return;
    }
    uint8_t reason = f->payload[0];

    pkt_tx_slot_t *slot = find_tx_slot(ctx, f->epoch, f->msg_id);
    if (slot == NULL) {
        return;
    }

    switch (reason) {
    case FRAME_NACK_TOO_BIG:
    case FRAME_NACK_MALFORMED:
        if (ctx->cb.on_tx_done != NULL) {
            ctx->cb.on_tx_done(ctx->cb.user, slot->msg_id, PKT_TX_REJECTED);
        }
        tx_slot_free(slot);
        break;

    case FRAME_NACK_NO_SESSION:
        handle_nack_no_session(ctx, slot);
        break;

    case FRAME_NACK_BAD_CRC:
        slot->msg_retry_cnt++;
        if (slot->msg_retry_cnt > (uint8_t)PKT_MSG_RETRIES) {
            if (ctx->cb.on_tx_done != NULL) {
                ctx->cb.on_tx_done(ctx->cb.user, slot->msg_id, PKT_TX_REJECTED);
            }
            tx_slot_free(slot);
            break;
        }
        /* Own epoch only: other concurrently in-flight messages are untouched. */
        slot->epoch++;
        slot->acked_mask = 0;
        slot->next_frag = 0;
        slot->done_wait = false;
        for (size_t i = 0; i < PKT_TX_WINDOW; i++) {
            slot->flight[i].active = false;
        }
        break;

    default:
        break;
    }
}

static bool find_free_flight(pkt_tx_slot_t *slot, size_t *out_idx) {
    for (size_t i = 0; i < PKT_TX_WINDOW; i++) {
        if (!slot->flight[i].active) {
            *out_idx = i;
            return true;
        }
    }
    return false;
}

/* Every fragment acked, no DONE yet (PROTOCOL.md section 7): nudge with the
 * last fragment once a full RTO passes with nothing back, same backoff as
 * any other retransmit. */
static bool service_done_wait(pkt_ctx_t *ctx, pkt_tx_slot_t *slot, uint32_t now_ms) {
    if (!slot->done_wait_armed) {
        slot->done_deadline_ms = now_ms + (uint32_t)PKT_RTO_MS;
        slot->done_wait_armed = true;
        return false;
    }
    if (now_ms < slot->done_deadline_ms) {
        return false;
    }
    if (slot->done_retry_cnt >= (uint8_t)(PKT_MAX_RETRIES - 1)) {
        if (ctx->cb.on_tx_done != NULL) {
            ctx->cb.on_tx_done(ctx->cb.user, slot->msg_id, PKT_TX_TIMEOUT);
        }
        tx_slot_free(slot);
        return true;
    }
    if (!send_fragment_at(ctx, slot, (uint16_t)(slot->frag_cnt - 1u))) {
        return false;
    }
    ctx->stats.retransmissions++;
    slot->done_retry_cnt++;
    slot->done_deadline_ms = now_ms + rto_for(slot->done_retry_cnt);
    return true;
}

/* One scheduling opportunity for slot: at most one fragment goes out (a
 * retransmit takes priority over a fresh send), so pkt_poll()'s round-robin
 * loop gives every active message a turn instead of one message draining
 * its whole window before its neighbors get a look in. Returns whether
 * anything happened (sent, retransmitted, or the slot reached a terminal
 * status) so the caller knows whether another pass could still make progress. */
static bool service_slot(pkt_ctx_t *ctx, pkt_tx_slot_t *slot, uint32_t now_ms) {
    if (slot->done_wait) {
        return service_done_wait(ctx, slot, now_ms);
    }

    for (size_t i = 0; i < PKT_TX_WINDOW; i++) {
        pkt_flight_t *e = &slot->flight[i];
        if (!e->active) {
            continue;
        }
        if (!e->armed) {
            /* Already (best-effort) resent synchronously by NACK handling;
             * just start this attempt's timer for real. */
            e->deadline_ms = now_ms + rto_for(e->retry_cnt);
            e->armed = true;
            continue;
        }
        if (now_ms < e->deadline_ms) {
            continue;
        }
        if (e->retry_cnt >= (uint8_t)(PKT_MAX_RETRIES - 1)) {
            if (ctx->cb.on_tx_done != NULL) {
                ctx->cb.on_tx_done(ctx->cb.user, slot->msg_id, PKT_TX_TIMEOUT);
            }
            tx_slot_free(slot);
            return true;
        }
        if (!send_fragment_at(ctx, slot, e->frag_idx)) {
            return false;
        }
        ctx->stats.retransmissions++;
        e->retry_cnt++;
        e->deadline_ms = now_ms + rto_for(e->retry_cnt);
        return true;
    }

    if (slot->next_frag < slot->frag_cnt) {
        size_t idx;
        if (!find_free_flight(slot, &idx)) {
            return false; /* window full */
        }
        if (!send_fragment_at(ctx, slot, slot->next_frag)) {
            return false;
        }
        slot->flight[idx].frag_idx = slot->next_frag;
        slot->flight[idx].retry_cnt = 0;
        slot->flight[idx].deadline_ms = now_ms + rto_for(0);
        slot->flight[idx].armed = true;
        slot->flight[idx].active = true;
        slot->next_frag++;
        return true;
    }

    return false;
}

void pkt_init(pkt_ctx_t *ctx, const pkt_callbacks_t *cb, uint8_t epoch) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb = *cb;
    ctx->epoch = epoch;
}

pkt_result_t pkt_send(pkt_ctx_t *ctx, const uint8_t *data, size_t len, uint16_t *id_out) {
    if (len > (size_t)PKT_MAX_MESSAGE) {
        return PKT_ERR_TOO_BIG;
    }

    pkt_tx_slot_t *slot = NULL;
    for (size_t i = 0; i < PKT_MAX_TX_MSGS; i++) {
        if (ctx->tx_slots[i].state == TX_FREE) {
            slot = &ctx->tx_slots[i];
            break;
        }
    }
    if (slot == NULL) {
        return PKT_ERR_NO_SLOT;
    }

    memset(slot, 0, sizeof(*slot));
    slot->msg = data;
    slot->total_len = (uint32_t)len;
    slot->msg_crc32 = crc32_finalize(crc32_update(PKT_CRC32_INIT, data, len));
    slot->frag_cnt = frag_count_for((uint32_t)len);
    slot->msg_id = ctx->next_msg_id++;
    slot->epoch = ctx->epoch;
    slot->state = TX_ACTIVE;

    if (id_out != NULL) {
        *id_out = slot->msg_id;
    }
    return PKT_OK;
}

void pkt_feed(pkt_ctx_t *ctx, const uint8_t *bytes, size_t len) {
    ctx->stats.bytes_received += (uint32_t)len;
    for (size_t i = 0; i < len; i++) {
        struct frame f;
        if (deframer_push(&ctx->deframer, bytes[i], &f, &ctx->rx_errors)) {
            ctx->stats.frames_received++;
            switch (f.type) {
            case FRAME_DATA:
                handle_data_frame(ctx, &f);
                break;
            case FRAME_ACK:
                handle_ack_frame(ctx, &f);
                break;
            case FRAME_DONE:
                handle_done_frame(ctx, &f);
                break;
            case FRAME_NACK:
                handle_nack_frame(ctx, &f);
                break;
            default:
                break;
            }
        }
    }
}

void pkt_poll(pkt_ctx_t *ctx, uint32_t now_ms) {
    for (size_t i = 0; i < PKT_MAX_RX_SESSIONS; i++) {
        pkt_rx_session_t *s = &ctx->rx_sessions[i];
        if (s->state != RX_ACTIVE) {
            continue;
        }
        if (s->touched) {
            s->last_rx_ms = now_ms;
            s->touched = false;
            continue;
        }
        /* Unsigned subtraction wraps correctly even if now_ms has wrapped
         * past last_rx_ms, as long as the real elapsed time fits in 32
         * bits (about 49 days). */
        if (now_ms - s->last_rx_ms >= (uint32_t)PKT_RX_TIMEOUT_MS) {
            s->state = RX_FREE;
            ctx->stats.sessions_expired++;
        }
    }

    /* Round-robin across active messages at fragment granularity: each pass
     * gives every active slot one scheduling opportunity, so a long
     * message's many fragments interleave with short messages' instead of
     * one slot draining its whole window before the next slot gets a turn. */
    bool progressed;
    do {
        progressed = false;
        for (size_t k = 0; k < PKT_MAX_TX_MSGS; k++) {
            size_t i = (ctx->tx_rr_cursor + k) % PKT_MAX_TX_MSGS;
            pkt_tx_slot_t *slot = &ctx->tx_slots[i];
            if (slot->state != TX_ACTIVE) {
                continue;
            }
            if (service_slot(ctx, slot, now_ms)) {
                progressed = true;
            }
        }
    } while (progressed);
    ctx->tx_rr_cursor = (uint8_t)((ctx->tx_rr_cursor + 1u) % (uint8_t)PKT_MAX_TX_MSGS);
}

void pkt_get_stats(const pkt_ctx_t *ctx, pkt_stats_t *out) {
    *out = ctx->stats;
    out->frame_crc_errors = ctx->rx_errors.bad_crc;
    out->cobs_errors = ctx->rx_errors.cobs_error;
}
