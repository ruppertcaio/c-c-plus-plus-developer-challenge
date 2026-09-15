#ifndef PACKETIZER_PKT_CONFIG_H
#define PACKETIZER_PKT_CONFIG_H

/*
 * Compile-time limits for the packetizer core. Every value is overridable
 * from the build (e.g. -DPKT_MAX_PAYLOAD=64) without touching this file.
 */

#ifndef PKT_MAX_PAYLOAD
#define PKT_MAX_PAYLOAD 128
#endif

#ifndef PKT_MAX_MESSAGE
#define PKT_MAX_MESSAGE 4096
#endif

#ifndef PKT_TX_WINDOW
#define PKT_TX_WINDOW 8
#endif

#ifndef PKT_MAX_TX_MSGS
#define PKT_MAX_TX_MSGS 4
#endif

#ifndef PKT_MAX_RX_SESSIONS
#define PKT_MAX_RX_SESSIONS 2
#endif

#ifndef PKT_RECENT_IDS
#define PKT_RECENT_IDS 8
#endif

#ifndef PKT_RTO_MS
#define PKT_RTO_MS 200
#endif

#ifndef PKT_RTO_MAX_MS
#define PKT_RTO_MAX_MS 2000
#endif

#ifndef PKT_MAX_RETRIES
#define PKT_MAX_RETRIES 8
#endif

#ifndef PKT_MSG_RETRIES
#define PKT_MSG_RETRIES 2
#endif

#ifndef PKT_RX_TIMEOUT_MS
#define PKT_RX_TIMEOUT_MS 5000
#endif

/* Wire layout: ver:4|type:4, epoch:u8, msg_id:u16, frag_idx:u16, frag_cnt:u16. */
#ifndef PKT_HEADER_SIZE
#define PKT_HEADER_SIZE 8
#endif

#ifndef PKT_CRC_SIZE
#define PKT_CRC_SIZE 2
#endif

/* Fragment 0 only: total_len:u32 | msg_crc32:u32, carved out of its payload. */
#ifndef PKT_META_SIZE
#define PKT_META_SIZE 8
#endif

#ifndef PKT_MAX_FRAME
#define PKT_MAX_FRAME (PKT_HEADER_SIZE + PKT_MAX_PAYLOAD + PKT_CRC_SIZE)
#endif

/* +1 COBS overhead byte, +1 zero delimiter. Only holds while PKT_MAX_FRAME <= 254. */
#ifndef PKT_MAX_WIRE_FRAME
#define PKT_MAX_WIRE_FRAME (PKT_MAX_FRAME + 1 + 1)
#endif

/* Worst case fragment count for a message of PKT_MAX_MESSAGE bytes: fragment 0
 * carries PKT_META_SIZE fewer payload bytes than the rest, so it needs padding
 * into the division before rounding up. */
#ifndef PKT_MAX_FRAGS
#define PKT_MAX_FRAGS (((PKT_MAX_MESSAGE) + (PKT_META_SIZE) + (PKT_MAX_PAYLOAD) - 1) / (PKT_MAX_PAYLOAD))
#endif

/* COBS encodes a run of up to 254 non-zero bytes behind a single overhead byte;
 * past that a frame needs a second overhead byte and PKT_MAX_WIRE_FRAME is wrong. */
_Static_assert(PKT_MAX_FRAME <= 254, "frame must fit a single COBS overhead byte");

/* Fragment 0 must have payload left over after stealing PKT_META_SIZE bytes for
 * total_len/msg_crc32, otherwise it can never carry any message data. */
_Static_assert(PKT_MAX_PAYLOAD > PKT_META_SIZE, "payload too small to hold fragment-0 metadata");

/* Retransmit and session tracking use a 64-bit bitmap indexed by frag_idx. */
_Static_assert(PKT_MAX_FRAGS <= 64, "fragment count exceeds the 64-bit tracking bitmap");

#endif /* PACKETIZER_PKT_CONFIG_H */
