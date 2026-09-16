#ifndef PACKETIZER_PKT_H
#define PACKETIZER_PKT_H

#include "packetizer/frame.h"
#include "packetizer/pkt_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Fragment, send, reassemble, deliver, and the selective-repeat reliability
 * machinery on top: ACK/NACK/DONE, retry with exponential backoff, message
 * restart on BAD_CRC, and duplicate-of-a-completed-message detection
 * (PROTOCOL.md sections 7-8).
 */

typedef enum {
    PKT_OK = 0,
    PKT_ERR_TOO_BIG,  /* len > PKT_MAX_MESSAGE */
    PKT_ERR_NO_SLOT,  /* every tx slot already has a message in flight */
} pkt_result_t;

typedef enum {
    PKT_TX_DELIVERED = 0,  /* DONE received */
    PKT_TX_TIMEOUT,        /* a fragment exhausted PKT_MAX_RETRIES with no response at all */
    PKT_TX_REJECTED,       /* peer NACKed TOO_BIG/MALFORMED, or BAD_CRC past PKT_MSG_RETRIES */
} pkt_tx_status_t;

typedef struct {
    /* Hands one wire-encoded frame to the transport. Returns false to mean
     * backpressure: the frame is not considered sent, and pkt_poll() will
     * offer it again on a later call. */
    bool (*write)(void *user, const uint8_t *buf, size_t len);
    /* Fires once per reassembled message, data valid only for the call. */
    void (*on_message)(void *user, const uint8_t *data, size_t len);
    /* Fires once a send finishes locally (see pkt_tx_status_t). */
    void (*on_tx_done)(void *user, uint16_t msg_id, pkt_tx_status_t st);
    void *user;
} pkt_callbacks_t;

typedef struct {
    uint32_t frames_sent;
    uint32_t bytes_sent;         /* wire bytes on successful write() calls only */
    uint32_t frames_received;    /* frames that passed COBS + CRC-16 + version + type */
    uint32_t bytes_received;     /* total bytes ever passed to pkt_feed */
    uint32_t payload_sent;       /* message bytes for sends that fully completed */
    uint32_t payload_delivered;  /* message bytes handed to on_message */
    uint32_t duplicate_frags;    /* fragment bit already set in its session's bitmap */
    uint32_t frame_crc_errors;   /* CRC-16 mismatches, one frame at a time */
    uint32_t message_crc_errors; /* CRC-32 mismatch after a message's bitmap completed */
    uint32_t cobs_errors;
    uint32_t sessions_expired;   /* RX sessions freed by PKT_RX_TIMEOUT_MS */
    uint32_t retransmissions;    /* DATA fragments (including DONE-wait nudges) sent more than once */
    uint32_t acks_sent;
    uint32_t acks_received;
    uint32_t dones_sent;
    uint32_t dones_received;
    uint32_t nacks_sent;
    uint32_t nacks_received;
} pkt_stats_t;

/* One fragment currently sent-but-unacknowledged. Holds no message bytes:
 * TX is zero-copy (see pkt_send()), so a retransmit rebuilds the payload
 * from the slot's msg/total_len/frag_cnt, same as the original send. */
typedef struct {
    uint16_t frag_idx;
    uint32_t deadline_ms;
    uint8_t retry_cnt;   /* retransmissions already spent on this fragment */
    bool active;         /* false = free entry */
    /* pkt_feed() has no now_ms (PROTOCOL.md section 2). The NO_SESSION
     * reaction resends this fragment synchronously from inside pkt_feed()
     * but cannot compute a real deadline there; armed = false means "already
     * sent, just stamp deadline_ms with the real now_ms on the next
     * pkt_poll() call, don't send again" — the same deferred-stamp trick
     * pkt_rx_session_t.touched uses for last_rx_ms. */
    bool armed;
} pkt_flight_t;

/* One in-flight outbound message. */
typedef struct {
    const uint8_t *msg;   /* never copied, see pkt_send() */
    uint32_t total_len;
    uint32_t msg_crc32;
    uint64_t acked_mask;
    pkt_flight_t flight[PKT_TX_WINDOW];
    uint16_t msg_id;
    uint16_t frag_cnt;
    uint16_t next_frag;      /* first fragment index never yet handed to write() */
    uint8_t epoch;           /* this slot's own copy; BAD_CRC restart bumps only this one */
    uint8_t msg_retry_cnt;   /* whole-message restarts spent on BAD_CRC */
    uint8_t state;
    /* Every fragment acked, no DONE yet: flight[] is empty at this point
     * (every entry frees on ACK), so the retry timer for the last-fragment
     * nudge (PROTOCOL.md section 7) gets its own fields instead of reusing
     * a flight entry that no longer exists. */
    bool done_wait;
    bool done_wait_armed;
    uint32_t done_deadline_ms;
    uint8_t done_retry_cnt;
} pkt_tx_slot_t;

/* One in-flight inbound message, keyed by (epoch, msg_id) once fragment 0
 * arrives. received_mask has one bit per fragment index; a session is
 * complete when it equals (1 << frag_cnt) - 1. */
typedef struct {
    uint8_t data[PKT_MAX_MESSAGE];
    uint64_t received_mask;
    uint32_t total_len;
    uint32_t msg_crc32;
    uint32_t last_rx_ms;
    uint16_t msg_id;
    uint16_t frag_cnt;
    uint8_t epoch;
    uint8_t state;
    /* pkt_feed() has no now_ms of its own to stamp last_rx_ms with, so it
     * only flags progress here; pkt_poll() turns the flag into a timestamp
     * using the now_ms it was actually given. Costs nothing: it lands in
     * padding the struct already had. */
    bool touched;
} pkt_rx_session_t;

/* One completed (epoch, msg_id) pair, PROTOCOL.md section 8. */
typedef struct {
    uint16_t msg_id;
    uint8_t epoch;
} pkt_recent_entry_t;

/* Ring of the last PKT_RECENT_IDS delivered messages, so a fragment or a
 * DONE-lost nudge arriving after the session already closed gets recognized
 * as a duplicate instead of silently dropped or, worse, redelivered.
 * count (entries actually written, capped at PKT_RECENT_IDS) exists so a
 * freshly-zeroed cache doesn't spuriously match (epoch=0, msg_id=0), which
 * is exactly the very first message any fresh sender produces. */
typedef struct {
    pkt_recent_entry_t entries[PKT_RECENT_IDS];
    uint8_t cursor;
    uint8_t count;
} pkt_recent_cache_t;

typedef struct {
    pkt_rx_session_t rx_sessions[PKT_MAX_RX_SESSIONS];
    pkt_tx_slot_t tx_slots[PKT_MAX_TX_MSGS];
    pkt_recent_cache_t recent;
    struct deframer deframer;
    pkt_callbacks_t cb;
    pkt_stats_t stats;
    struct pkt_rx_errors rx_errors;
    uint16_t next_msg_id;
    uint8_t epoch;
    /* Round-robin starting point for pkt_poll()'s TX scheduler, so a long
     * message's many fragments don't get first dibs on every single call. */
    uint8_t tx_rr_cursor;
} pkt_ctx_t;

/* Zeroes ctx (valid initial state for every field: free slots, empty
 * deframer, zeroed stats) and records cb and this endpoint's epoch. */
void pkt_init(pkt_ctx_t *ctx, const pkt_callbacks_t *cb, uint8_t epoch);

/* Reserves a tx slot and computes CRC-32/frag_cnt once, up front. data is
 * not copied: it must stay valid and unchanged until on_tx_done fires for
 * the msg_id written to *id_out (when non-NULL). Fragments are actually
 * written out by later pkt_poll() calls, not by this call itself. */
pkt_result_t pkt_send(pkt_ctx_t *ctx, const uint8_t *data, size_t len, uint16_t *id_out);

/* Feeds raw bytes off the link into the deframer. May call on_message
 * synchronously, any number of times, before returning. */
void pkt_feed(pkt_ctx_t *ctx, const uint8_t *bytes, size_t len);

/* Drives outbound fragments and RX session timeouts. now_ms is the only
 * clock reference anywhere in this module; call it as often as the link
 * needs servicing. */
void pkt_poll(pkt_ctx_t *ctx, uint32_t now_ms);

void pkt_get_stats(const pkt_ctx_t *ctx, pkt_stats_t *out);

#endif /* PACKETIZER_PKT_H */
