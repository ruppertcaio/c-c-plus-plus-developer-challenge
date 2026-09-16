#ifndef PACKETIZER_FRAME_H
#define PACKETIZER_FRAME_H

#include "packetizer/pkt_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Frame encode/parse and the byte-stream deframer, PROTOCOL.md sections 3-6.
 * Used by the reassembly layer (pkt.c): nothing above it deals with struct
 * frame directly, only fully reassembled messages. Public only because
 * pkt_ctx_t embeds struct deframer and struct pkt_rx_errors by value.
 */

#define PKT_VERSION 1u

enum frame_type {
    FRAME_DATA = 1,
    FRAME_ACK = 2,
    FRAME_DONE = 3,
    FRAME_NACK = 4,
};

/* NACK payload is a single reason byte, PROTOCOL.md section 7. Values match the wire
 * encoding exactly, not just enum declaration order. */
enum frame_nack_reason {
    FRAME_NACK_TOO_BIG = 1,
    FRAME_NACK_BAD_CRC = 2,
    FRAME_NACK_NO_SESSION = 3,
    FRAME_NACK_MALFORMED = 4,
};

enum frame_err {
    FRAME_OK = 0,
    FRAME_ERR_SHORT,        /* fewer bytes than header + CRC-16 */
    FRAME_ERR_BAD_CRC,      /* CRC-16 mismatch, header fields cannot be trusted */
    FRAME_ERR_BAD_VERSION,  /* ver nibble != PKT_VERSION */
    FRAME_ERR_BAD_TYPE,     /* type nibble not one of DATA/ACK/DONE/NACK */
    FRAME_ERR_TOO_LONG,     /* more bytes than header + PKT_MAX_PAYLOAD + CRC-16 */
};

struct frame {
    uint8_t type;
    uint8_t epoch;
    uint16_t msg_id;
    uint16_t frag_idx;
    uint16_t frag_cnt;
    const uint8_t *payload; /* aliases the buffer frame_parse() was given, no copy */
    size_t payload_len;
};

/* Serializes f's header and payload little-endian, appends CRC-16, COBS-encodes
 * the result and appends the 0x00 delimiter. Returns the wire length written to
 * out, or 0 if f->payload_len exceeds PKT_MAX_PAYLOAD or the encoding does not
 * fit in cap bytes (cap should be at least PKT_MAX_WIRE_FRAME). */
size_t frame_encode(const struct frame *f, uint8_t *out, size_t cap);

/* Parses raw, a frame already stripped of COBS coding and its delimiter, into
 * f. f->payload points into raw itself and is valid only as long as raw is. */
enum frame_err frame_parse(const uint8_t *raw, size_t len, struct frame *f);

/* Rejection counters, one per frame_err plus the deframer's own failure modes,
 * for surfacing later as receive statistics. */
struct pkt_rx_errors {
    uint32_t cobs_error;  /* malformed COBS coding: bad run code or embedded zero */
    uint32_t short_frame;
    uint32_t bad_crc;
    uint32_t bad_version;
    uint32_t bad_type;
    uint32_t too_long;
    uint32_t overflow;    /* candidate frame exceeded PKT_MAX_WIRE_FRAME before a delimiter */
};

/* Accumulates COBS-encoded bytes between 0x00 delimiters. A run that would
 * overflow buf is discarded up to the next delimiter rather than truncated,
 * since a truncated frame would only fail CRC anyway and there is no reason
 * to pay for decoding it. */
struct deframer {
    uint8_t buf[PKT_MAX_WIRE_FRAME];
    uint8_t len;
    bool discarding;
};

/* Feeds one byte from the link into d. Returns true and fills *f when byte
 * completed a frame that decoded and parsed cleanly; f->payload then points
 * into d->buf and stays valid until the next deframer_push() call. Every
 * rejected candidate frame (and every discarded overflow) bumps the matching
 * counter in errs. Consecutive delimiters with nothing accumulated between
 * them are a no-op. */
bool deframer_push(struct deframer *d, uint8_t byte, struct frame *f, struct pkt_rx_errors *errs);

#endif /* PACKETIZER_FRAME_H */
