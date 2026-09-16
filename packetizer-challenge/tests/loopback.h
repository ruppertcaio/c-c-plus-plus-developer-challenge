#ifndef LOOPBACK_H
#define LOOPBACK_H

#include "packetizer/pkt.h"
#include "transport/impair.h"

/* Two pkt_ctx_t wired together over in-memory frame queues, for tests that
 * need to drive both ends of a link without a real transport. The simulated
 * clock is just whatever now_ms the test passes to pkt_poll() directly, so
 * there is no clock field here.
 */

/* Generous enough to cover a hostile-channel soak (many in-flight fragments,
 * further multiplied by impair's dup/split) without the queue silently
 * dropping frames a slower test would never produce enough of to notice. */
#define LOOPBACK_MAX_FRAMES 512
#define LOOPBACK_MAX_MESSAGES 40

typedef struct {
    uint8_t data[PKT_MAX_WIRE_FRAME];
    size_t len;
} loopback_frame_t;

/* Runs once per frame as write() hands it off, with type/msg_id/frag_idx
 * already decoded so a test can target a specific fragment without
 * reimplementing COBS/frame parsing itself. Return false to drop the frame;
 * set *dup to additionally enqueue a second copy. NULL means pass everything
 * through once. */
typedef bool (*loopback_hook_t)(void *user, uint8_t type, uint16_t msg_id, uint16_t frag_idx,
                                 const uint8_t *frame, size_t len, bool *dup);

typedef struct {
    loopback_frame_t frames[LOOPBACK_MAX_FRAMES];
    size_t count;
    loopback_hook_t hook;
    void *hook_user;
    /* Statistical degradation, PROTOCOL.md's hostile-link failure modes
     * (loss/corruption/dup/reorder/split) rather than the hook's precise
     * one-fragment targeting. A direction uses one or the other: when
     * impair_active, queue_push() runs frames through impair_process()
     * and never consults hook. */
    struct impair_state impair;
    bool impair_active;
} loopback_queue_t;

/* One message captured by on_message(), snapshotted immediately: the
 * pointer pkt_ctx_t hands the callback only stays valid for that one call. */
typedef struct {
    uint8_t data[PKT_MAX_MESSAGE];
    size_t len;
} loopback_message_t;

typedef struct {
    pkt_ctx_t *ctx;
    loopback_message_t messages[LOOPBACK_MAX_MESSAGES];
    size_t message_count;
    uint16_t tx_done_ids[LOOPBACK_MAX_MESSAGES];
    pkt_tx_status_t tx_done_status[LOOPBACK_MAX_MESSAGES];
    size_t tx_done_count;
} loopback_endpoint_t;

typedef struct {
    loopback_endpoint_t a;
    loopback_endpoint_t b;
    loopback_queue_t a_to_b;
    loopback_queue_t b_to_a;
} loopback_t;

/* Calls pkt_init() on both a and b, wiring write() to queue into the other
 * side's direction (through that direction's hook, if set) and on_message/
 * on_tx_done to record into the matching loopback_endpoint_t. */
void loopback_init(loopback_t *lb, pkt_ctx_t *a, uint8_t epoch_a, pkt_ctx_t *b, uint8_t epoch_b);

/* Same wiring as loopback_init, but b is left untouched (no pkt_init call):
 * only its callbacks are repointed at lb. For tests simulating one side's
 * process restarting (a fresh a) against a peer (b) that kept running and
 * may still hold state — sessions, recent-ids cache, stats — from before. */
void loopback_rewire(loopback_t *lb, pkt_ctx_t *a, uint8_t epoch_a, pkt_ctx_t *b);

void loopback_set_hook(loopback_t *lb, bool a_to_b, loopback_hook_t hook, void *user);

/* Replaces the ad hoc hook for one direction with statistical impairment
 * (see transport/impair.h): every frame written that way is dropped,
 * corrupted, duplicated, reordered or split at random instead of passing
 * through hook. Call after loopback_init()/loopback_rewire(), which zero
 * the whole loopback_t including this state. */
void loopback_set_impair(loopback_t *lb, bool a_to_b, const struct impair_cfg *cfg);

/* Delivers every currently queued frame to the peer's pkt_feed(), in queue
 * order, then clears the queue. Tests needing a different delivery order
 * (reordering, interleaving) mutate frames[0..count) directly first. */
void loopback_flush_a_to_b(loopback_t *lb);
void loopback_flush_b_to_a(loopback_t *lb);

/* Polls both ends at the fixed now_ms, flushes both directions, and repeats
 * until neither flush enqueues anything new or max_rounds is hit. For tests
 * that just want a message to settle (delivered, timed out or rejected)
 * without hand-rolling the round-trip loop themselves. Tests asserting on
 * exact RTO timing still step now_ms and poll manually instead. */
void loopback_pump(loopback_t *lb, uint32_t now_ms, int max_rounds);

/* Same as loopback_pump(), but first releases anything either direction's
 * impair is still holding for a reorder swap. Without this, a block held
 * because its "next" never came (the test simply stopped sending) would sit
 * in impair forever and its message would never settle. */
void loopback_settle(loopback_t *lb, uint32_t now_ms, int max_rounds);

#endif /* LOOPBACK_H */
