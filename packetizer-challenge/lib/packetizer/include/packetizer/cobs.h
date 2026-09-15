#ifndef PACKETIZER_COBS_H
#define PACKETIZER_COBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Consistent Overhead Byte Stuffing, PROTOCOL.md section 3. Strips every
 * 0x00 byte out of a frame so a single 0x00 delimiter can mark frame
 * boundaries on the wire without ambiguity. Pure functions over caller-owned
 * buffers, no state kept here, no delimiter byte handled on either side.
 */

/* Worst case encoded size for len input bytes: one overhead byte per run of
 * up to 254 non-zero bytes, plus the one overhead byte always emitted (even
 * for len = 0). Does not include the trailing 0x00 delimiter. */
#define COBS_MAX_ENCODED(len) ((len) + (len) / 254 + 1)

/* Encodes src into dst, without the trailing 0x00 delimiter (the caller
 * appends that when framing). Returns the encoded length, or 0 if dst does
 * not have cap bytes to hold it. */
size_t cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);

/* Decodes src, an encoded frame with the delimiter already stripped, into
 * dst. Fails on a 0x00 byte anywhere in src, on a run code that would read
 * past the end of src, or on decoded output that would not fit in cap
 * bytes. On success writes the decoded length to *out_len. */
bool cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *out_len);

#endif /* PACKETIZER_COBS_H */
