#include "packetizer/pkt.h"

#include "packetizer/cobs.h"
#include "packetizer/crc.h"

#include <string.h>

enum {
    TX_FREE = 0,
    TX_SENDING,
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

/* Dedup-then-write for one already-validated fragment, and the completion
 * check that follows it. Shared by the frag_idx == 0 and frag_idx > 0 paths
 * in handle_data_frame(). */
static void accept_fragment(pkt_ctx_t *ctx, pkt_rx_session_t *s, uint16_t frag_idx,
                             const uint8_t *frag_data, size_t frag_len) {
    uint64_t bit = (uint64_t)1 << frag_idx;
    if (s->received_mask & bit) {
        ctx->stats.duplicate_frags++;
        return;
    }

    memcpy(s->data + frag_offset(frag_idx), frag_data, frag_len);
    s->received_mask |= bit;

    if (s->received_mask != full_mask(s->frag_cnt)) {
        return;
    }

    uint32_t crc = crc32_finalize(crc32_update(PKT_CRC32_INIT, s->data, s->total_len));
    if (crc == s->msg_crc32) {
        ctx->stats.payload_delivered += s->total_len;
        if (ctx->cb.on_message != NULL) {
            ctx->cb.on_message(ctx->cb.user, s->data, s->total_len);
        }
    } else {
        ctx->stats.message_crc_errors++;
    }
    s->state = RX_FREE;
}

/* PROTOCOL.md section 5's validation order, minus the NACK reasons: nothing
 * sends NACK yet (no sender-side reaction to react to it either), so every
 * rejection here is just a silent drop instead. */
static void handle_data_frame(pkt_ctx_t *ctx, const struct frame *f) {
    if (f->frag_idx == 0) {
        if (f->frag_cnt == 0 || f->payload_len < (size_t)PKT_META_SIZE) {
            return;
        }

        uint32_t total_len = read_u32_le(f->payload);
        uint32_t msg_crc32 = read_u32_le(f->payload + 4);

        if (total_len > (uint32_t)PKT_MAX_MESSAGE) {
            return;
        }
        if (f->frag_cnt != frag_count_for(total_len)) {
            return;
        }
        if (f->payload_len - (size_t)PKT_META_SIZE != frag_payload_len(total_len, 0, f->frag_cnt)) {
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
        return;
    }

    pkt_rx_session_t *s = find_rx_session(ctx, f->epoch, f->msg_id);
    if (s == NULL || f->frag_idx >= s->frag_cnt) {
        return;
    }
    if (f->payload_len != frag_payload_len(s->total_len, f->frag_idx, s->frag_cnt)) {
        return;
    }
    s->touched = true;
    accept_fragment(ctx, s, f->frag_idx, f->payload, f->payload_len);
}

/* Builds and writes one fragment of slot. Returns false on backpressure
 * (write() declined it); slot->next_frag is only advanced on success, so the
 * same fragment is retried by the next pkt_poll() call. */
static bool send_fragment(pkt_ctx_t *ctx, pkt_tx_slot_t *slot) {
    struct frame f;
    f.type = FRAME_DATA;
    f.epoch = slot->epoch;
    f.msg_id = slot->msg_id;
    f.frag_idx = slot->next_frag;
    f.frag_cnt = slot->frag_cnt;

    size_t len = frag_payload_len(slot->total_len, slot->next_frag, slot->frag_cnt);

    /* Fragment 0's payload is total_len+msg_crc32 followed by message bytes,
     * which do not sit next to each other in memory, so it needs a local
     * staging buffer. Every other fragment points straight into the
     * caller's buffer: frame_encode() copies from it once, synchronously. */
    uint8_t meta_payload[PKT_MAX_PAYLOAD];
    if (slot->next_frag == 0) {
        write_u32_le(meta_payload, slot->total_len);
        write_u32_le(meta_payload + 4, slot->msg_crc32);
        if (len > 0) {
            memcpy(meta_payload + PKT_META_SIZE, slot->msg, len);
        }
        f.payload = meta_payload;
        f.payload_len = (size_t)PKT_META_SIZE + len;
    } else {
        f.payload = slot->msg + frag_offset(slot->next_frag);
        f.payload_len = len;
    }

    uint8_t wire[PKT_MAX_WIRE_FRAME];
    size_t wire_len = frame_encode(&f, wire, sizeof(wire));

    if (!ctx->cb.write(ctx->cb.user, wire, wire_len)) {
        return false;
    }

    ctx->stats.frames_sent++;
    ctx->stats.bytes_sent += (uint32_t)wire_len;
    slot->next_frag++;
    return true;
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

    slot->msg = data;
    slot->total_len = (uint32_t)len;
    slot->msg_crc32 = crc32_finalize(crc32_update(PKT_CRC32_INIT, data, len));
    slot->frag_cnt = frag_count_for((uint32_t)len);
    slot->msg_id = ctx->next_msg_id++;
    slot->epoch = ctx->epoch;
    slot->next_frag = 0;
    slot->state = TX_SENDING;

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
            if (f.type == FRAME_DATA) {
                handle_data_frame(ctx, &f);
            }
            /* ACK/DONE/NACK land with the reliability commit; nothing sends
             * them yet, and there is nothing to react to them with here. */
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

    for (size_t i = 0; i < PKT_MAX_TX_MSGS; i++) {
        pkt_tx_slot_t *slot = &ctx->tx_slots[i];
        if (slot->state != TX_SENDING) {
            continue;
        }
        while (slot->next_frag < slot->frag_cnt) {
            if (!send_fragment(ctx, slot)) {
                break;
            }
        }
        if (slot->next_frag == slot->frag_cnt) {
            ctx->stats.payload_sent += slot->total_len;
            slot->state = TX_FREE;
            if (ctx->cb.on_tx_done != NULL) {
                ctx->cb.on_tx_done(ctx->cb.user, slot->msg_id, PKT_TX_SENT);
            }
        }
    }
}

void pkt_get_stats(const pkt_ctx_t *ctx, pkt_stats_t *out) {
    *out = ctx->stats;
    out->frame_crc_errors = ctx->rx_errors.bad_crc;
    out->cobs_errors = ctx->rx_errors.cobs_error;
}
