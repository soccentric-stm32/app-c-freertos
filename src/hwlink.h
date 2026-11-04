/*
 * hwlink.h - HWLINK framing: COBS, CRC-32 and the 8-byte header
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     HWLINK LNK-001..LNK-005 (SRS-GUI-F767 Appendix A)
 */

#ifndef HWLINK_H
#define HWLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HWLINK_PROTOCOL_MAJOR     1U
#define HWLINK_PROTOCOL_MINOR     0U
#define HWLINK_MAX_FRAME          2048U
#define HWLINK_MAX_ENCODED        (HWLINK_MAX_FRAME + (HWLINK_MAX_FRAME / 254U) + 2U)

#define HWLINK_FLAG_RESPONSE      (1U << 0U)
#define HWLINK_FLAG_EVENT         (1U << 1U)

/* Build the unencoded frame (proto, flags, seq, payload, crc32). */
bool hwlink_build_raw(const uint8_t *payload, size_t payload_len,
                      uint8_t flags, uint16_t seq,
                      uint8_t *raw, size_t raw_capacity, size_t *raw_len);

/* COBS-encode a raw frame and append the 0x00 delimiter. */
bool hwlink_cobs_encode(const uint8_t *raw, size_t raw_len,
                        uint8_t *out, size_t out_capacity, size_t *out_len);

/* COBS-decode one frame (without the delimiter).  Returns -1 on error. */
int hwlink_cobs_decode(const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_capacity, size_t *out_len);

/* Validate the header and CRC of an unencoded frame and return its payload. */
bool hwlink_decode_raw(const uint8_t *raw, size_t raw_len,
                       uint8_t *payload, size_t payload_capacity,
                       size_t *payload_len, uint8_t *flags, uint16_t *seq);

#endif /* HWLINK_H */
