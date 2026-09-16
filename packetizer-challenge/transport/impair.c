#include "transport/impair.h"

#include <string.h>

/* Marsaglia's xorshift32. Not cryptographic, doesn't need to be: this is a
 * repeatable noise source for tests, not a security mechanism. State 0 is a
 * fixed point (stays 0 forever), so impair_init() refuses to store it. */
static uint32_t xorshift32_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* p <= 0 and p >= 1 are handled without touching the RNG, so a disabled
 * impairment (the common case in most tests) never perturbs the sequence
 * the enabled ones see. */
static bool decide(struct impair_state *st, double p) {
    if (p <= 0.0) {
        return false;
    }
    if (p >= 1.0) {
        return true;
    }
    uint32_t r = xorshift32_next(&st->rng);
    return (double)r < p * 4294967296.0; /* 2^32, r's full range */
}

static void corrupt_bits(struct impair_state *st, uint8_t *buf, size_t len) {
    if (len == 0) {
        return;
    }
    unsigned nbits = 1u + xorshift32_next(&st->rng) % 3u; /* 1..3 bits, PROTOCOL.md CRC-16 HD=4 */
    for (unsigned i = 0; i < nbits; i++) {
        size_t byte_idx = xorshift32_next(&st->rng) % len;
        unsigned bit_idx = xorshift32_next(&st->rng) % 8u;
        buf[byte_idx] ^= (uint8_t)(1u << bit_idx);
    }
}

/* Emits buf as 1 piece, or (cfg.split) as 1-4 pieces of random size, each
 * piece a separate emit_cb call. That's what lets the receiving deframer see
 * a frame arriving split across multiple reads, same as a real byte stream
 * with no message boundaries of its own (PROTOCOL.md section 1). */
static void emit_split(struct impair_state *st, const uint8_t *buf, size_t len,
                        impair_emit_t emit_cb, void *user) {
    unsigned pieces = st->cfg.split ? (1u + xorshift32_next(&st->rng) % 4u) : 1u;
    if (pieces <= 1 || len == 0) {
        emit_cb(user, buf, len);
        return;
    }

    size_t offset = 0;
    for (unsigned i = 0; i < pieces; i++) {
        size_t remaining = len - offset;
        size_t left_after = pieces - i - 1; /* pieces still owed after this one */
        size_t chunk;
        if (left_after == 0 || left_after >= remaining) {
            chunk = remaining; /* last piece, or not enough bytes left to keep splitting */
        } else {
            size_t max_chunk = remaining - left_after; /* leave >=1 byte per remaining piece */
            chunk = 1 + xorshift32_next(&st->rng) % max_chunk;
        }
        emit_cb(user, buf + offset, chunk);
        offset += chunk;
        if (offset >= len) {
            break;
        }
    }
}

void impair_init(struct impair_state *st, const struct impair_cfg *cfg) {
    memset(st, 0, sizeof(*st));
    st->cfg = *cfg;
    st->rng = cfg->seed != 0 ? cfg->seed : 0x9E3779B9u;
}

void impair_process(struct impair_state *st, const uint8_t *buf, size_t len,
                     impair_emit_t emit_cb, void *user) {
    /* Snapshot whatever a previous call decided to hold, then tentatively
     * clear has_held: it's set again below only if this block also draws a
     * reorder decision. Either way the old block is released after this
     * block's own output, further down. */
    bool release_prior = st->has_held;
    uint8_t prior[IMPAIR_MAX_BLOCK];
    size_t prior_len = st->held_len;
    if (release_prior) {
        memcpy(prior, st->held, prior_len);
    }
    st->has_held = false;

    /* Every decision draws from the RNG in the same fixed order regardless
     * of outcome, so a seed's reproduced run doesn't depend on which branches
     * happened to be taken along the way. */
    bool lost = decide(st, st->cfg.loss);
    bool corrupt = decide(st, st->cfg.corrupt);
    bool dup = decide(st, st->cfg.dup);
    bool reorder = decide(st, st->cfg.reorder);

    if (!lost) {
        uint8_t work[IMPAIR_MAX_BLOCK];
        memcpy(work, buf, len);
        if (corrupt) {
            corrupt_bits(st, work, len);
        }

        if (reorder) {
            memcpy(st->held, work, len);
            st->held_len = len;
            st->has_held = true;
        } else {
            emit_split(st, work, len, emit_cb, user);
            if (dup) {
                emit_split(st, work, len, emit_cb, user);
            }
        }
    }

    if (release_prior) {
        emit_split(st, prior, prior_len, emit_cb, user);
    }
}

void impair_flush(struct impair_state *st, impair_emit_t emit_cb, void *user) {
    if (!st->has_held) {
        return;
    }
    uint8_t held[IMPAIR_MAX_BLOCK];
    size_t held_len = st->held_len;
    memcpy(held, st->held, held_len);
    st->has_held = false;
    emit_split(st, held, held_len, emit_cb, user);
}
