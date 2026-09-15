#ifndef PACKETIZER_PKT_H
#define PACKETIZER_PKT_H

#include "packetizer/frame.h"
#include "packetizer/pkt_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Data path only: fragment, send, reassemble, deliver. No ACK, retry, NACK
 * or duplicate-of-a-completed-message detection yet (PROTOCOL.md sections
 * 7-8) — that lands with the reliability commit. The one piece of that
 * machinery already in here is RX session timeout, since without it a
 * lossy transfer would pin one of only PKT_MAX_RX_SESSIONS slots forever.
 */

typedef enum {
    PKT_OK = 0,
    PKT_ERR_TOO_BIG,  /* len > PKT_MAX_MESSAGE */
    PKT_ERR_NO_SLOT,  /* every tx slot already has a message in flight */
} pkt_result_t;

typedef enum {
    PKT_TX_SENT = 0,  /* every fragment reached write() successfully */
    /* No delivery confirmation exists yet: DELIVERED/TIMEOUT/REJECTED
     * (PROTOCOL.md section 9) need ACK/DONE/NACK, which is not implemented
     * here. This value only means the local write side is done with it. */
} pkt_tx_status_t;

typedef struct {
    /* Hands one wire-encoded frame to the transport. Returns false to mean
     * backpressure: the fragment stays pending and pkt_poll() retries it
     * on the next call, in the same position, before moving on. */
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
} pkt_stats_t;

/* One in-flight outbound message: msg is never copied, see pkt_send(). */
typedef struct {
    const uint8_t *msg;
    uint32_t total_len;
    uint32_t msg_crc32;
    uint16_t msg_id;
    uint16_t frag_cnt;
    uint16_t next_frag;  /* first fragment not yet handed to write() successfully */
    uint8_t epoch;
    uint8_t state;
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

typedef struct {
    pkt_rx_session_t rx_sessions[PKT_MAX_RX_SESSIONS];
    pkt_tx_slot_t tx_slots[PKT_MAX_TX_MSGS];
    struct deframer deframer;
    pkt_callbacks_t cb;
    pkt_stats_t stats;
    struct pkt_rx_errors rx_errors;
    uint16_t next_msg_id;
    uint8_t epoch;
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
