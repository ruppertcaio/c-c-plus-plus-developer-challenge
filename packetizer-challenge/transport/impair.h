#ifndef TRANSPORT_IMPAIR_H
#define TRANSPORT_IMPAIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Deterministic channel impairment, no sockets, no I/O. Sits between a real
 * transport and the packetizer core (typically in tests) and degrades a
 * stream of already-framed blocks the way a hostile link does: dropped,
 * bit-corrupted, duplicated, reordered or split across reads.
 *
 * Every decision comes out of an xorshift32 PRNG seeded by the caller, not
 * the C library's rand(), so a run reproduces bit-for-bit from the seed
 * alone on any machine, no libc RNG implementation differences involved.
 */

/* Largest block impair_process() will hold onto for a reorder swap. Sized
 * for one wire frame (PKT_MAX_WIRE_FRAME=140 with the packetizer's default
 * config); this module has no dependency on packetizer headers, so the
 * bound is its own constant rather than borrowed from pkt_config.h. */
#ifndef IMPAIR_MAX_BLOCK
#define IMPAIR_MAX_BLOCK 256
#endif

struct impair_cfg {
    double loss;     /* P(block dropped outright) */
    double corrupt;  /* P(block takes 1-3 bit flips) */
    double dup;      /* P(block is emitted a second time) */
    double reorder;  /* P(block is held back one step, swapping with the next) */
    bool split;      /* true: emit each surviving copy as 1-4 random-sized pieces */
    uint32_t seed;   /* xorshift32 seed; 0 is remapped, the algorithm can't run on it */
};

/* Hands one impaired chunk of bytes downstream, e.g. into the peer's
 * pkt_feed() or a real socket write(). May be called zero or more times for
 * a single impair_process() call (loss: zero, dup/split: more than one). */
typedef bool (*impair_emit_t)(void *user, const uint8_t *buf, size_t len);

struct impair_state {
    struct impair_cfg cfg;
    uint32_t rng;
    /* At most one block held for a reorder swap at a time: a second reorder
     * decision while one is already held just flushes the older one first
     * (see impair.c), so this never needs to be a queue. */
    uint8_t held[IMPAIR_MAX_BLOCK];
    size_t held_len;
    bool has_held;
};

/* Zeroes st and records cfg. A zero seed is remapped to a fixed nonzero
 * constant, xorshift32 with an all-zero state never produces anything but
 * zero. */
void impair_init(struct impair_state *st, const struct impair_cfg *cfg);

/* Runs one block through the impairment pipeline: loss, corruption, dup and
 * split are decided for this block; a reorder decision instead holds it and
 * releases whatever was held from the previous call, after this block's own
 * output, which is what actually swaps the two on the wire. len must be
 * <= IMPAIR_MAX_BLOCK, the caller's responsibility same as any fixed-size
 * buffer contract in this codebase. */
void impair_process(struct impair_state *st, const uint8_t *buf, size_t len,
                     impair_emit_t emit_cb, void *user);

/* Emits a block still held for reorder, if any, e.g. at the end of a test
 * run so nothing is lost just because no further block ever arrived to
 * swap it with. No-op when nothing is held. */
void impair_flush(struct impair_state *st, impair_emit_t emit_cb, void *user);

#endif /* TRANSPORT_IMPAIR_H */
