#include "packetizer/cobs.h"

size_t cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
    if (cap == 0) {
        return 0;
    }

    size_t code_pos = 0;
    size_t di = 1;
    uint8_t code = 1;

    for (size_t si = 0; si < len; si++) {
        if (src[si] == 0) {
            dst[code_pos] = code;
            if (di >= cap) {
                return 0;
            }
            code_pos = di++;
            code = 1;
            continue;
        }

        if (di >= cap) {
            return 0;
        }
        dst[di++] = src[si];
        code++;

        /* 0xFF is reserved to mean "254 bytes with no zero in between", so a
         * run has to be force-closed at 254 bytes even without a zero byte
         * to trigger it, or the code value would overflow its own encoding. */
        if (code == 0xFF) {
            dst[code_pos] = code;
            if (di >= cap) {
                return 0;
            }
            code_pos = di++;
            code = 1;
        }
    }

    dst[code_pos] = code;
    return di;
}

bool cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap, size_t *out_len) {
    size_t si = 0;
    size_t di = 0;

    while (si < len) {
        uint8_t code = src[si];
        if (code == 0) {
            return false;
        }
        si++;

        size_t run = (size_t)(code - 1);
        if (run > len - si) {
            return false;
        }

        for (size_t i = 0; i < run; i++) {
            if (src[si] == 0) {
                return false;
            }
            if (di >= cap) {
                return false;
            }
            dst[di++] = src[si++];
        }

        /* code == 0xFF closed a forced 254-byte run with no zero behind it,
         * so no delimiter byte gets reinserted for that case. si == len means
         * this was the final block, which never had a real zero after it. */
        if (code < 0xFF && si < len) {
            if (di >= cap) {
                return false;
            }
            dst[di++] = 0;
        }
    }

    *out_len = di;
    return true;
}
