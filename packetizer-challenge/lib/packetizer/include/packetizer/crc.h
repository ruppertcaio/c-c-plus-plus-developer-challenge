#ifndef PACKETIZER_CRC_H
#define PACKETIZER_CRC_H

#include <stddef.h>
#include <stdint.h>

/*
 * CRC-16/CCITT-FALSE (poly 0x1021) and CRC-32/ISO-HDLC (poly 0xEDB88320,
 * reflected), as required by PROTOCOL.md section 6. Both are pure functions
 * over caller-owned buffers, no state kept here.
 */

#define PKT_CRC16_INIT 0xFFFFu
#define PKT_CRC32_INIT 0xFFFFFFFFu

/* Feed one span of bytes into a running CRC-16. Start crc at PKT_CRC16_INIT;
 * the header + payload of a frame can be fed in as separate calls, the
 * result of one call is a valid crc argument for the next. No finalization
 * step, CRC-16/CCITT-FALSE has no output XOR. */
uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len);

/* Feed one span of bytes into a running CRC-32. Start crc at PKT_CRC32_INIT
 * and pass the result of each call as the crc argument of the next, so a
 * message split across fragments accumulates one chunk at a time. The
 * running value is not the final checksum, call crc32_finalize() once all
 * chunks are in. */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* Applies CRC-32/ISO-HDLC's output XOR to a running value from crc32_update(). */
uint32_t crc32_finalize(uint32_t crc);

#endif /* PACKETIZER_CRC_H */
