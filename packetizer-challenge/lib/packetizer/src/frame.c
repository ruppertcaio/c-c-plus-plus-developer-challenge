#include "packetizer/frame.h"

#include "packetizer/cobs.h"
#include "packetizer/crc.h"

#include <string.h>

size_t frame_encode(const struct frame *f, uint8_t *out, size_t cap) {
    if (f->payload_len > (size_t)PKT_MAX_PAYLOAD || cap == 0) {
        return 0;
    }

    /* Header + payload + CRC-16 assembled here first: COBS needs one
     * contiguous source span, and the header fields do not live next to
     * the caller's payload in memory. */
    uint8_t plain[PKT_MAX_FRAME];
    size_t n = 0;

    plain[n++] = (uint8_t)((PKT_VERSION << 4) | (f->type & 0x0Fu));
    plain[n++] = f->epoch;
    plain[n++] = (uint8_t)(f->msg_id & 0xFFu);
    plain[n++] = (uint8_t)(f->msg_id >> 8);
    plain[n++] = (uint8_t)(f->frag_idx & 0xFFu);
    plain[n++] = (uint8_t)(f->frag_idx >> 8);
    plain[n++] = (uint8_t)(f->frag_cnt & 0xFFu);
    plain[n++] = (uint8_t)(f->frag_cnt >> 8);

    if (f->payload_len > 0) {
        memcpy(plain + n, f->payload, f->payload_len);
        n += f->payload_len;
    }

    uint16_t crc = crc16_ccitt(PKT_CRC16_INIT, plain, n);
    plain[n++] = (uint8_t)(crc & 0xFFu);
    plain[n++] = (uint8_t)(crc >> 8);

    /* Reserve one byte of cap for the delimiter cobs_encode does not add. */
    size_t enc_len = cobs_encode(plain, n, out, cap - 1);
    if (enc_len == 0) {
        return 0;
    }
    out[enc_len] = 0x00;
    return enc_len + 1;
}

enum frame_err frame_parse(const uint8_t *raw, size_t len, struct frame *f) {
    if (len < (size_t)PKT_HEADER_SIZE + (size_t)PKT_CRC_SIZE) {
        return FRAME_ERR_SHORT;
    }
    if (len > (size_t)PKT_MAX_FRAME) {
        return FRAME_ERR_TOO_LONG;
    }

    size_t crc_offset = len - (size_t)PKT_CRC_SIZE;
    uint16_t crc_calc = crc16_ccitt(PKT_CRC16_INIT, raw, crc_offset);
    uint16_t crc_wire = (uint16_t)((uint16_t)raw[crc_offset] | (uint16_t)((uint16_t)raw[crc_offset + 1] << 8));
    if (crc_calc != crc_wire) {
        return FRAME_ERR_BAD_CRC;
    }

    /* Header fields are only trustworthy past this point: CRC-16 just
     * confirmed nothing between here and crc_offset was corrupted. */
    uint8_t ver = (uint8_t)(raw[0] >> 4);
    uint8_t type = (uint8_t)(raw[0] & 0x0Fu);

    if (ver != PKT_VERSION) {
        return FRAME_ERR_BAD_VERSION;
    }
    if (type != FRAME_DATA && type != FRAME_ACK && type != FRAME_DONE && type != FRAME_NACK) {
        return FRAME_ERR_BAD_TYPE;
    }

    f->type = type;
    f->epoch = raw[1];
    f->msg_id = (uint16_t)((uint16_t)raw[2] | (uint16_t)((uint16_t)raw[3] << 8));
    f->frag_idx = (uint16_t)((uint16_t)raw[4] | (uint16_t)((uint16_t)raw[5] << 8));
    f->frag_cnt = (uint16_t)((uint16_t)raw[6] | (uint16_t)((uint16_t)raw[7] << 8));
    f->payload = raw + PKT_HEADER_SIZE;
    f->payload_len = crc_offset - (size_t)PKT_HEADER_SIZE;

    return FRAME_OK;
}

bool deframer_push(struct deframer *d, uint8_t byte, struct frame *f, struct pkt_rx_errors *errs) {
    if (byte != 0x00) {
        if (d->discarding) {
            return false;
        }
        if ((size_t)d->len >= sizeof(d->buf)) {
            d->discarding = true;
            errs->overflow++;
            return false;
        }
        d->buf[d->len++] = byte;
        return false;
    }

    bool was_discarding = d->discarding;
    size_t candidate_len = d->len;
    d->discarding = false;
    d->len = 0;

    /* Nothing accumulated (idle line, or the byte right after a discard):
     * back-to-back delimiters are a no-op, not an empty frame. */
    if (was_discarding || candidate_len == 0) {
        return false;
    }

    /* Decoded in place in d->buf: COBS decoding never expands data, and this
     * is exactly what keeps f->payload's lifetime tied to d instead of a
     * buffer that would go out of scope when this function returns. */
    size_t plain_len = 0;
    if (!cobs_decode(d->buf, candidate_len, d->buf, sizeof(d->buf), &plain_len)) {
        errs->cobs_error++;
        return false;
    }

    switch (frame_parse(d->buf, plain_len, f)) {
    case FRAME_OK:
        return true;
    case FRAME_ERR_SHORT:
        errs->short_frame++;
        return false;
    case FRAME_ERR_BAD_CRC:
        errs->bad_crc++;
        return false;
    case FRAME_ERR_BAD_VERSION:
        errs->bad_version++;
        return false;
    case FRAME_ERR_BAD_TYPE:
        errs->bad_type++;
        return false;
    case FRAME_ERR_TOO_LONG:
        errs->too_long++;
        return false;
    default:
        return false;
    }
}
