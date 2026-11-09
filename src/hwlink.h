/**
 * @file    hwlink.h
 * @brief   HWLINK framing: COBS, CRC-32 and the fixed 8-byte frame overhead.
 * @details HWLINK is the binary protocol carried over the USART3 ST-LINK VCP
 *          transport (see @ref uart3.h).  This header exposes the framing layer
 *          only: it builds a raw frame, COBS-encodes it for the wire, decodes an
 *          incoming COBS body and validates a raw frame's header and CRC.  The
 *          payload itself is an opaque FlatBuffers document produced and
 *          consumed by @ref comms_task.c.
 *
 *          The raw frame, before COBS encoding, is:
 *
 *          | Offset | Size | Field                                |
 *          |--------|------|--------------------------------------|
 *          | 0      | 1    | Protocol major, HWLINK_PROTOCOL_MAJOR |
 *          | 1      | 1    | Flags, HWLINK_FLAG_* bit mask        |
 *          | 2      | 2    | Sequence number, little-endian       |
 *          | 4      | n    | Payload, @c n bytes, may be empty    |
 *          | 4 + n  | 4    | CRC-32/ISO-HDLC, little-endian       |
 *
 *          The 4-byte header plus the 4-byte CRC trailer are the fixed 8-byte
 *          overhead of a frame (@c HWLINK_OVERHEAD in @ref hwlink.c); the
 *          historical name "8-byte header" in the file history counts both
 *          parts.  The header is never padded or aligned, so the payload
 *          starts at byte 4 and the CRC at byte @c 4+n.
 *
 *          **CRC coverage.**  The CRC is computed over the header and the
 *          payload, i.e. bytes @c 0..(4+n-1) of the raw frame, and stored in
 *          little-endian order immediately after the payload.  The CRC word
 *          itself and the COBS delimiter are not covered.  Both ends use
 *          @ref app_crc32 so the polynomial and initial/final XOR match the
 *          bootloader contract.
 *
 *          **COBS framing.**  The raw frame is COBS-encoded so that 0x00 never
 *          occurs inside the encoded body, and a single 0x00 delimiter is
 *          appended as the frame terminator.  The receiver splits the serial
 *          stream on 0x00, feeds each non-empty body to
 *          @ref hwlink_cobs_decode and then to @ref hwlink_decode_raw.  An
 *          empty body (two adjacent delimiters) is ignored.  This lets the
 *          link resynchronise after any corruption, since a damaged frame can
 *          only cost the bytes up to the next delimiter.
 *
 *          **Error policy.**  All four entry points are non-blocking, perform
 *          no allocation and report failure through their return value only.
 *          @ref comms_task.c reacts to a rejected frame by dropping it
 *          silently (`LNK-004`): a bad CRC or a malformed body is never
 *          answered with a NAK, so a noisy link cannot be amplified into a
 *          NAK storm.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     HWLINK LNK-001..LNK-005 (SRS-GUI-F767 Appendix A)
 * @ingroup hwlink
 */

#ifndef HWLINK_H
#define HWLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup hwlink HWLINK framing layer
 * @brief Raw frame construction, COBS framing and CRC validation for HWLINK.
 * @details The group holds the whole framing surface: two size limits, the
 *          protocol version and flag bits, and four functions that pair up as
 *          build/encode on transmit and decode/validate on receive.
 *
 *          A transmit path is always
 *          @ref hwlink_build_raw followed by @ref hwlink_cobs_encode, and a
 *          receive path is always @ref hwlink_cobs_decode followed by
 *          @ref hwlink_decode_raw.  Only the Comms task calls these functions,
 *          so they carry no internal locking; nothing here may be called
 *          concurrently with itself on the same buffers.  None of them touch
 *          the HAL, so all of them are safe to call from task context and,
 *          except for the shared buffer caveat, are reentrant.
 * @see comms_task.h, uart3.h, crc32.h
 * @{
 */

/** @brief Major protocol version, copied into byte 0 of every frame. */
#define HWLINK_PROTOCOL_MAJOR     1U
/**
 * @brief Minor protocol version.
 * @details The minor version is not carried in the frame header; it is
 *          reported to the host in the FlatBuffers Status message and used for
 *          feature negotiation there.  A mismatch in the major version, on the
 *          other hand, makes @ref hwlink_decode_raw reject the frame.
 */
#define HWLINK_PROTOCOL_MINOR     1U

/** @brief Largest raw frame in bytes, header, payload and CRC included. */
#define HWLINK_MAX_FRAME          2048U
/**
 * @brief Worst-case COBS-encoded frame size in bytes, delimiter included.
 * @details A frame of @ref HWLINK_MAX_FRAME non-zero bytes needs one code byte
 *          per 254 payload bytes plus one code byte for the final short block,
 *          and one trailing 0x00 delimiter:
 *          `2048 + (2048 / 254) + 2` = 2058 bytes.  This is the size of the
 *          transmit and receive staging buffers in @ref comms_task.c.
 * @note The expression is exact for every frame size @c n > 0, because the
 *       encoder emits exactly `n / 254 + 1` code bytes; only the delimiter
 *       and that code-byte overhead are added to the size limit.
 */
#define HWLINK_MAX_ENCODED        (HWLINK_MAX_FRAME + (HWLINK_MAX_FRAME / 254U) + 2U)

/**
 * @brief Frame is a response to an earlier host request.
 * @details Set by the device on Ack, Nak, Status, Pong and HelloReply frames.
 *          The device ignores inbound frames that have this bit or
 *          @ref HWLINK_FLAG_EVENT set, so the two flags together mark the
 *          direction of a message.
 */
#define HWLINK_FLAG_RESPONSE      (1U << 0U)
/**
 * @brief Frame is an unsolicited device event.
 * @details Set by the device on ButtonEvent, Log and Progress frames, which
 *          carry sequence number 0 and are not answered.  See
 *          @ref HWLINK_FLAG_RESPONSE for the shared direction rule.
 */
#define HWLINK_FLAG_EVENT         (1U << 1U)

/**
 * @brief Build an unencoded HWLINK frame.
 * @details Writes header byte 0 from @ref HWLINK_PROTOCOL_MAJOR, byte 1 from
 *          @p flags, bytes 2..3 from @p seq little-endian, copies the payload
 *          at byte 4, then appends the little-endian CRC-32 of the header and
 *          payload.  The result is not yet wire-ready and contains no
 *          delimiter; pass it to @ref hwlink_cobs_encode.
 *
 *          Nothing is written to @p raw and @p raw_len is not updated when the
 *          function returns false, so a caller cannot mistake a stale length
 *          for a valid frame.
 *
 * @param[in]  payload       Pointer to the payload bytes.  Must not be NULL,
 *                           even when @p payload_len is 0.
 * @param[in]  payload_len   Payload length in bytes, 0 to
 *                           (@ref HWLINK_MAX_FRAME - 8).
 * @param[in]  flags         HWLINK_FLAG_* bit mask, stored verbatim.
 * @param[in]  seq           Sequence number copied into the header.
 * @param[out] raw           Destination buffer for the raw frame.
 * @param[in]  raw_capacity  Capacity of @p raw in bytes.
 * @param[out] raw_len       Receives the raw frame length on success.
 *
 * @return true on success, false when the frame cannot be built.
 * @retval true  Frame built; @p raw_len holds header + payload + CRC.
 * @retval false @p raw, @p payload or @p raw_len is NULL.
 * @retval false @p raw_capacity is smaller than 8 + @p payload_len.
 * @retval false The frame would exceed @ref HWLINK_MAX_FRAME.
 *
 * @pre   @p raw addresses at least @p raw_capacity writable bytes.
 * @post  On success @p raw holds exactly @p raw_len bytes and no delimiter.
 * @warning @p flags is not validated against the HWLINK_FLAG_* mask, while
 *          @ref hwlink_decode_raw rejects reserved bits.  A caller that sets
 *          any other bit builds a frame this link will refuse to decode.
 * @note  Not thread-safe when callers share @p raw or @p raw_len.  @p raw and
 *        @p payload must not overlap, because the payload is copied with
 *        @c memcpy.
 * @see hwlink_cobs_encode, hwlink_decode_raw, app_crc32
 */
bool hwlink_build_raw(const uint8_t *payload, size_t payload_len,
                      uint8_t flags, uint16_t seq,
                      uint8_t *raw, size_t raw_capacity, size_t *raw_len);

/**
 * @brief COBS-encode a raw frame and append the 0x00 delimiter.
 * @details Standard Consistent Overhead Byte Stuffing: every zero byte of
 *          @p raw is replaced by a code byte that states the distance to the
 *          next zero, and a block of 254 non-zero bytes is closed by the 0xFF
 *          escape code.  The encoded body therefore never contains 0x00, and
 *          the function finishes by appending one 0x00 terminator, which
 *          @p out_len includes.  The delimiter is not part of the CRC.
 *
 * @param[in]  raw           Pointer to the unencoded frame.
 * @param[in]  raw_len       Length of @p raw in bytes; must be at least 1.
 * @param[out] out           Destination buffer for the encoded body.
 * @param[in]  out_capacity  Capacity of @p out in bytes; must be at least 2.
 * @param[out] out_len       Receives the encoded length, delimiter included.
 *
 * @return true on success, false when the frame does not fit.
 * @retval true  Frame encoded and terminated with 0x00.
 * @retval false @p raw, @p out or @p out_len is NULL.
 * @retval false @p raw_len is 0 or @p out_capacity is smaller than 2.
 * @retval false @p out_capacity cannot hold the encoded frame plus delimiter.
 *
 * @pre   @p raw holds @p raw_len readable bytes and @p out at least
 *        @p out_capacity writable bytes; the two must not overlap.
 * @post  On success @p out holds @p out_len bytes ending in 0x00.
 * @warning On failure @p out may hold a partially written, unterminated body
 *          and @p out_len is left untouched.  Callers must not transmit the
 *          buffer in that state, and must size @p out to at least
 *          @ref HWLINK_MAX_ENCODED for the largest legal frame.
 * @note  The trailing delimiter makes the result directly usable with
 *        @ref app_uart3_send; the receiver strips it before
 *        @ref hwlink_cobs_decode.
 * @see hwlink_cobs_decode, hwlink_build_raw, app_uart3_send
 */
bool hwlink_cobs_encode(const uint8_t *raw, size_t raw_len,
                        uint8_t *out, size_t out_capacity, size_t *out_len);

/**
 * @brief COBS-decode one frame that has already had its delimiter removed.
 * @details Walks the code bytes of the encoded body, copying each block and
 *          re-inserting the 0x00 byte a code byte implies.  A code byte of
 *          0xFF means a full 254-byte block with no implied zero, and no zero
 *          is appended after the final block.
 *
 * @param[in]  in             Encoded body, without the 0x00 delimiter.
 * @param[in]  in_len         Length of @p in in bytes.
 * @param[out] out            Destination buffer for the decoded frame.
 * @param[in]  out_capacity   Capacity of @p out in bytes.
 * @param[out] out_len        Receives the decoded length in bytes.
 *
 * @return 0 on success, -1 on a malformed or oversized frame.
 * @retval 0  Frame decoded; @p out_len holds the raw frame length.
 * @retval -1 @p in, @p out or @p out_len is NULL.
 * @retval -1 A code byte is 0x00, i.e. an embedded delimiter.
 * @retval -1 A block extends past the end of @p in (truncated frame).
 * @retval -1 The decoded frame would overflow @p out_capacity.
 *
 * @pre   @p in points at one complete COBS body; the caller is responsible for
 *        splitting the byte stream on the 0x00 delimiter.
 * @post  On success @p out holds the raw frame and @p out_len its length; on
 *        failure @p out_len is left untouched.
 * @note  A successful decode says nothing about frame validity: the result
 *        still has to pass @ref hwlink_decode_raw.  The int return type is
 *        kept from the original API so that -1 and 0 stay distinguishable.
 * @see hwlink_cobs_encode, hwlink_decode_raw
 */
int hwlink_cobs_decode(const uint8_t *in, size_t in_len,
                       uint8_t *out, size_t out_capacity, size_t *out_len);

/**
 * @brief Validate the header and CRC of a raw frame and return its payload.
 * @details Checks, in order: the argument pointers, the size limits, the
 *          protocol major version and the reserved flag bits, then the CRC-32
 *          over header and payload.  Only when all of them pass is the payload
 *          copied out and the header fields returned.  The payload copy is
 *          therefore only made from a frame that is already known to be
 *          intact and correctly framed.
 *
 * @param[in]  raw               Raw frame from @ref hwlink_cobs_decode.
 * @param[in]  raw_len           Length of @p raw in bytes.
 * @param[out] payload           Destination for the payload copy.
 * @param[in]  payload_capacity  Capacity of @p payload in bytes.
 * @param[out] payload_len       Receives the payload length in bytes.
 * @param[out] flags             Receives header byte 1, the HWLINK_FLAG_* set.
 * @param[out] seq               Receives the little-endian sequence number.
 *
 * @return true when the frame is valid and its payload was copied.
 * @retval true  Frame accepted; all four out parameters are written.
 * @retval false @p raw, @p payload, @p payload_len, @p flags or @p seq is NULL.
 * @retval false @p raw_len is smaller than 8 or larger than
 *               @ref HWLINK_MAX_FRAME.
 * @retval false Byte 0 is not @ref HWLINK_PROTOCOL_MAJOR.
 * @retval false Header byte 1 has a bit set outside @ref HWLINK_FLAG_RESPONSE
 *               and @ref HWLINK_FLAG_EVENT.
 * @retval false The stored CRC does not match @ref app_crc32 over the header
 *               and payload.
 * @retval false The payload does not fit in @p payload_capacity.
 *
 * @pre   @p raw holds @p raw_len readable bytes of a decoded frame.
 * @post  On success @p payload holds a private copy of @p payload_len bytes;
 *        on failure no out parameter is modified.
 * @note  The flag mask is spelled 0x03 in @ref hwlink.c rather than built from
 *        the HWLINK_FLAG_* macros, so a new flag bit must be added in both
 *        places.  The sequence number is not range-checked here; duplicate
 *        suppression and the direction rule live in @ref comms_task.c.
 * @note  Rejection is silent by design: the caller drops the frame instead of
 *        sending a NAK (`LNK-004`).
 * @see hwlink_build_raw, hwlink_cobs_decode, app_crc32
 */
bool hwlink_decode_raw(const uint8_t *raw, size_t raw_len,
                       uint8_t *payload, size_t payload_capacity,
                       size_t *payload_len, uint8_t *flags, uint16_t *seq);

/** @} */

#endif /* HWLINK_H */
