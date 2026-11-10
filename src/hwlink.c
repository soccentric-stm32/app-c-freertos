/**
 * @file    hwlink.c
 * @brief   HWLINK framing: COBS, CRC-32 and the 8-byte frame overhead.
 * @details Implements the four entry points declared in @ref hwlink.h, which
 *          describes the protocol contract; this file supplies the local
 *          sizing constants and the byte-level code.
 *
 *          A raw frame, before COBS encoding, is
 *          `proto[1] flags[1] seq[2] payload[n] crc32[4]`.  Byte 0 carries
 *          @ref HWLINK_PROTOCOL_MAJOR, byte 1 the HWLINK_FLAG_* mask, bytes
 *          2..3 the sequence number, bytes 4..(3+n) the payload and the last
 *          four bytes the CRC-32 of everything before them.  Only the two
 *          single-byte fields are endian-neutral; every multi-byte field is
 *          serialised with explicit shifts and masks, so the wire image is
 *          little-endian and does not depend on the core's native byte order
 *          or on struct packing.  Nothing is padded or aligned: the payload
 *          starts at byte @ref HWLINK_HEADER_SIZE and the CRC at byte
 *          `4 + n`.
 *
 *          The CRC covers bytes `0..(4+n-1)`, the header plus the payload.
 *          The stored CRC word and the 0x00 delimiter appended by
 *          @ref hwlink_cobs_encode are outside the covered range, so the
 *          checksum never depends on its own encoding and a receiver can
 *          verify the frame before it looks at the trailer.  Both ends use
 *          @ref app_crc32, whose polynomial and initial/final XOR are fixed by
 *          the bootloader contract.
 *
 *          All four functions are single-shot, non-blocking and
 *          allocation-free; this file holds no state at all, so every buffer
 *          is owned by the caller.  Frames rejected at any stage are dropped
 *          silently by @ref comms_task.c rather than answered with a NAK
 *          (`LNK-004`), so link noise cannot be amplified into a NAK storm.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     HWLINK LNK-001..LNK-005 (SRS-GUI-F767 Appendix A)
 * @ingroup hwlink
 */

#include <string.h>
#include "crc32.h"
#include "hwlink.h"

/**
 * @brief Size of the fixed frame header in bytes: proto, flags and seq.
 * @details Also the offset at which the payload starts.
 */
#define HWLINK_HEADER_SIZE 4U
/** @brief Size of the trailing little-endian CRC-32 in bytes. */
#define HWLINK_CRC_SIZE 4U
/**
 * @brief Fixed per-frame overhead in bytes, header plus CRC trailer.
 * @details The historical name "8-byte header" counts the 4-byte header and
 *          the 4-byte CRC trailer together.  A frame therefore occupies
 *          `HWLINK_OVERHEAD + payload_len` bytes and can carry at most
 *          `HWLINK_MAX_FRAME - HWLINK_OVERHEAD` payload bytes.  It is the
 *          minimum legal length of a decoded frame, and the size that
 *          @ref hwlink_build_raw and @ref hwlink_decode_raw agree on.
 */
#define HWLINK_OVERHEAD (HWLINK_HEADER_SIZE + HWLINK_CRC_SIZE)

/**
 * @brief Build an unencoded HWLINK frame: header, payload and CRC-32.
 * @details Computes `total = HWLINK_OVERHEAD + payload_len`, validates the
 *          arguments and only then writes anything.  Byte 0 is set to
 *          @ref HWLINK_PROTOCOL_MAJOR and byte 1 to @p flags verbatim; the
 *          sequence number is split with `seq & 0xFF` into byte 2 and
 *          `seq >> 8` into byte 3, which fixes the wire order to
 *          little-endian on any host.  The payload is copied unchanged to
 *          offset @ref HWLINK_HEADER_SIZE with @c memcpy; it is not
 *          inspected, escaped, padded or aligned.
 *
 *          The CRC is then taken over the header and payload with
 *          @ref app_crc32 and stored as four individually shifted bytes at
 *          offset `HWLINK_HEADER_SIZE + payload_len`, lowest byte first.  The
 *          CRC word is written after the checksum is complete, so it can
 *          never be part of its own input, and no delimiter is appended here
 *          (see @ref hwlink_cobs_encode).
 *
 *          Failure paths, evaluated in this order: @p raw, @p payload or
 *          @p raw_len is NULL; `total` exceeds @p raw_capacity; `total`
 *          exceeds @ref HWLINK_MAX_FRAME, i.e. @p payload_len is larger than
 *          2040.  All three return false before the first store, so @p raw is
 *          untouched and @p raw_len still holds whatever the caller stored.
 *
 * @param[in]  payload       Payload bytes; must be non-NULL even when empty.
 * @param[in]  payload_len   Payload length in bytes, 0 to 2040.
 * @param[in]  flags         Raw value for header byte 1, stored unvalidated.
 * @param[in]  seq           Sequence number, serialised little-endian.
 * @param[out] raw           Destination buffer for the raw frame.
 * @param[in]  raw_capacity  Writable bytes available at @p raw.
 * @param[out] raw_len       Receives `HWLINK_OVERHEAD + payload_len`.
 *
 * @return true when a complete frame was written to @p raw.
 * @retval true  Frame built; @p raw_len holds its length, delimiter excluded.
 * @retval false A pointer argument is NULL.
 * @retval false @p raw_capacity is smaller than the frame.
 * @retval false @p payload_len would push the frame past @ref HWLINK_MAX_FRAME.
 *
 * @pre   @p raw addresses at least @p raw_capacity writable bytes and does
 *        not overlap @p payload, because the payload is copied with @c memcpy.
 * @post  On success @p raw holds exactly @p raw_len bytes, beginning with the
 *        protocol version and ending with the CRC word.
 * @warning @p flags is not masked against HWLINK_FLAG_RESPONSE and
 *          HWLINK_FLAG_EVENT, while @ref hwlink_decode_raw rejects those
 *          reserved bits.  A caller that sets any other bit builds a frame
 *          this link will refuse to decode.
 * @note  The payload length is range-checked before the overhead is added, so
 *        the `HWLINK_OVERHEAD + payload_len` sum can never wrap `size_t` and a
 *        huge request is rejected instead of passing the capacity test.
 * @see hwlink_cobs_encode, hwlink_decode_raw, app_crc32
 */
bool hwlink_build_raw(const uint8_t *payload, size_t payload_len, uint8_t flags, uint16_t seq, uint8_t *raw, size_t raw_capacity, size_t *raw_len)
{
    if(raw == NULL || payload == NULL || raw_len == NULL || payload_len > (HWLINK_MAX_FRAME - HWLINK_OVERHEAD))
    {
        return false;
    }
    size_t total = HWLINK_OVERHEAD + payload_len;
    if(total > raw_capacity)
    {
        return false;
    }

    raw[0] = HWLINK_PROTOCOL_MAJOR;
    raw[1] = flags;
    raw[2] = (uint8_t)(seq & 0xFFU);
    raw[3] = (uint8_t)(seq >> 8U);
    memcpy(&raw[4], payload, payload_len);

    uint32_t crc                               = app_crc32(raw, HWLINK_HEADER_SIZE + payload_len);
    raw[HWLINK_HEADER_SIZE + payload_len + 0U] = (uint8_t)(crc & 0xFFU);
    raw[HWLINK_HEADER_SIZE + payload_len + 1U] = (uint8_t)((crc >> 8U) & 0xFFU);
    raw[HWLINK_HEADER_SIZE + payload_len + 2U] = (uint8_t)((crc >> 16U) & 0xFFU);
    raw[HWLINK_HEADER_SIZE + payload_len + 3U] = (uint8_t)((crc >> 24U) & 0xFFU);

    *raw_len = total;
    return true;
}

/**
 * @brief COBS-encode a raw frame and append the 0x00 delimiter.
 * @details Consistent Overhead Byte Stuffing, emitted into @p out as blocks
 *          that each start with a code byte.  The code byte states the
 *          distance from itself to the next zero byte, counting itself:
 *          0x01 means "zero bytes then a zero", 0x02 means "one byte then a
 *          zero".  Because every zero byte is dropped and its position is
 *          encoded in the preceding code byte, the encoded body contains no
 *          0x00 at all, so 0x00 is unambiguous as a frame delimiter on the
 *          wire.
 *
 *          The loop keeps @c code_pos at the current (not yet final) code byte
 *          and rewrites it when the next zero closes the block.  A block that
 *          fills with 254 non-zero bytes is closed with the 0xFF escape code,
 *          which means "254 bytes, no implied zero", and a fresh code byte is
 *          opened, so a run of non-zero bytes cannot push the distance past
 *          one byte.  After the input is consumed the last code byte is
 *          finalised and one 0x00 delimiter is appended; the result is
 *          already wire-ready and can be passed straight to
 *          @ref app_uart3_send.
 *
 *          Edge cases: an input whose length is an exact multiple of 254 ends
 *          with a 0xFF block followed by a final 0x01 code byte, i.e. a
 *          legitimate empty trailing block, rather than being truncated.  An
 *          input ending in a zero byte also leaves a final empty block, so
 *          that zero is implied by the preceding code byte and never copied.
 *          A raw frame of @c n bytes with no 254-byte full block encodes to
 *          `n + 1` bytes, plus one extra code byte per full block, so the
 *          all-non-zero frame is the worst case and is what
 *          @ref HWLINK_MAX_ENCODED bounds.
 *
 *          Failure paths: @p raw, @p out or @p out_len is NULL; @p raw_len is
 *          0 (nothing to encode, never a valid frame); @p out_capacity is
 *          below 2, the minimum for one code byte plus the delimiter; or a
 *          write would reach @p out_capacity - whether when opening a code
 *          byte after a zero, when opening one after a 0xFF block, when
 *          copying a data byte, or when appending the delimiter.
 *
 * @param[in]  raw           Unencoded frame from @ref hwlink_build_raw.
 * @param[in]  raw_len       Length of @p raw in bytes; must be at least 1.
 * @param[out] out           Destination buffer for the encoded body.
 * @param[in]  out_capacity  Writable bytes available at @p out; at least 2.
 * @param[out] out_len       Receives the encoded length, delimiter included.
 *
 * @return true when the whole frame was encoded and terminated with 0x00.
 * @retval true  @p out holds @p out_len bytes ending in the delimiter.
 * @retval false A pointer argument is NULL, or @p raw_len is 0.
 * @retval false @p out_capacity is smaller than 2.
 * @retval false @p out_capacity cannot hold the body plus delimiter.
 *
 * @pre   @p raw holds @p raw_len readable bytes; @p out addresses
 *        @p out_capacity writable bytes and must not overlap @p raw, because
 *        bytes are copied one at a time while both indices advance.
 * @post  On success @p out holds @p out_len bytes and the last one is 0x00.
 * @warning On failure @p out may hold a partially written, unterminated body
 *          and the in-progress code byte may already have been finalised, so
 *          the buffer must not be transmitted; @p out_len is never modified,
 *          so a stale length cannot be mistaken for a result.  Size @p out to
 *          at least @ref HWLINK_MAX_ENCODED for the largest legal frame.
 * @note  The delimiter is an artefact of framing, not of the frame: it is not
 *        covered by the CRC and is stripped by the receiver before
 *        @ref hwlink_cobs_decode.
 * @see hwlink_cobs_decode, hwlink_build_raw, app_uart3_send
 */
bool hwlink_cobs_encode(const uint8_t *raw, size_t raw_len, uint8_t *out, size_t out_capacity, size_t *out_len)
{
    size_t read     = 0U;
    size_t write    = 0U;
    size_t code_pos = 0U;

    if(raw == NULL || out == NULL || out_len == NULL || raw_len == 0U)
    {
        return false;
    }
    if(out_capacity < 2U)
    {
        return false;
    }

    code_pos      = write++;
    out[code_pos] = 1U;

    while(read < raw_len)
    {
        if(raw[read] == 0U)
        {
            out[code_pos] = (uint8_t)(write - code_pos);
            code_pos      = write++;
            if(write >= out_capacity)
            {
                return false;
            }
            out[code_pos] = 1U;
            read++;
        }
        else
        {
            if(write >= out_capacity)
            {
                return false;
            }
            out[write++] = raw[read++];
            if((uint8_t)(write - code_pos) == 0xFFU)
            {
                out[code_pos] = 0xFFU;
                code_pos      = write++;
                if(write >= out_capacity)
                {
                    return false;
                }
                out[code_pos] = 1U;
            }
        }
    }

    out[code_pos] = (uint8_t)(write - code_pos);
    if(write >= out_capacity)
    {
        return false;
    }
    out[write++] = 0U; /* frame delimiter */

    *out_len = write;
    return true;
}

/**
 * @brief COBS-decode one frame whose 0x00 delimiter has already been removed.
 * @details Walks the code bytes of the body: each one is read, rejected if it
 *          is 0x00, and the `code - 1` bytes that follow it are copied
 *          verbatim to @p out.  A code other than 0xFF implies a zero byte
 *          after its block, so the block is followed by one 0x00 - but only
 *          when bytes remain, because the final block of a frame is never
 *          followed by an implied zero.  A code of 0xFF denotes a full
 *          254-byte block with no implied zero, which is how a run of more
 *          than 253 non-zero bytes is split into blocks.
 *
 *          Failure paths, all returning -1: any pointer argument is NULL; a
 *          code byte equals 0x00, which can only happen if the caller failed
 *          to split the stream on the delimiter; a block would extend past
 *          @p in_len, i.e. the body is truncated; the block copy would exceed
 *          @p out_capacity; or an implied zero cannot be stored because
 *          @p out_capacity is already reached.  The capacity tests use
 *          @c > and @c >= against @p out_capacity, so the decoder never writes
 *          past @p out, but blocks already copied before a later failure stay
 *          in the buffer.
 *
 *          An empty body (@p in_len 0) is accepted and yields @p out_len 0;
 *          it carries no frame and is rejected by @ref hwlink_decode_raw, so
 *          the caller's delimiter-splitting loop can ignore the case.
 *
 * @param[in]  in            Encoded body, delimiter already removed.
 * @param[in]  in_len        Length of @p in in bytes.
 * @param[out] out           Destination buffer for the decoded frame.
 * @param[in]  out_capacity  Writable bytes available at @p out.
 * @param[out] out_len       Receives the decoded length in bytes.
 *
 * @return 0 on success, -1 on a malformed or oversized body.
 * @retval 0  Body decoded; @p out_len holds the raw frame length.
 * @retval -1 @p in, @p out or @p out_len is NULL.
 * @retval -1 A code byte is 0x00, i.e. an embedded delimiter.
 * @retval -1 A block extends past the end of @p in (truncated body).
 * @retval -1 A block or its implied zero would overflow @p out_capacity.
 *
 * @pre   @p in points at exactly one complete COBS body; splitting the byte
 *        stream on 0x00 and discarding empty bodies is the caller's job (see
 *        @ref comms_task.c).
 * @post  On success @p out holds the raw frame and @p out_len its length; on
 *        failure @p out_len is left untouched, so no stale length is exposed.
 * @note  A successful decode says nothing about frame validity: the result
 *        still has to pass @ref hwlink_decode_raw for version, flags and CRC.
 *        The @c int return type is kept from the original API so that -1 and 0
 *        stay distinguishable from a valid zero length.
 * @see hwlink_cobs_encode, hwlink_decode_raw
 */
int hwlink_cobs_decode(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_capacity, size_t *out_len)
{
    size_t read  = 0U;
    size_t write = 0U;

    if(in == NULL || out == NULL || out_len == NULL)
    {
        return -1;
    }

    while(read < in_len)
    {
        uint8_t code = in[read++];
        if(code == 0U)
        {
            return -1;
        }

        size_t block = (size_t)code - 1U;
        if(block > in_len - read)
        {
            return -1;
        }
        if(block > out_capacity - write)
        {
            return -1;
        }
        memcpy(&out[write], &in[read], block);
        write += block;
        read += block;

        if((code != 0xFFU) && (read < in_len))
        {
            if(write >= out_capacity)
            {
                return -1;
            }
            out[write++] = 0U;
        }
    }

    *out_len = write;
    return 0;
}

/**
 * @brief Validate a raw frame's header and CRC, then return its payload.
 * @details Checks run in a fixed order, cheapest and most decisive first:
 *          1. @p raw, @p payload, @p payload_len, @p flags and @p seq are
 *             non-NULL.
 *          2. @p raw_len is between @ref HWLINK_OVERHEAD and
 *             @ref HWLINK_MAX_FRAME, so the frame is large enough to hold a
 *             header and a CRC and small enough to be a legal frame.
 *          3. Byte 0 equals @ref HWLINK_PROTOCOL_MAJOR, so a frame from an
 *             incompatible protocol revision is discarded before any CRC
 *             work.
 *          4. Header byte 1 has no bit set outside the 0x03 mask of
 *             @ref HWLINK_FLAG_RESPONSE and @ref HWLINK_FLAG_EVENT.
 *          5. The little-endian CRC word stored after the payload matches
 *             @ref app_crc32 over the header and payload, i.e. bytes
 *             `0..(raw_len - 5)`.
 *          6. The payload fits in @p payload_capacity.
 *
 *          Only after all six pass is the payload copied out and are the
 *          three decoded fields written, so the copy is never made from a
 *          frame that could still be corrupt, truncated or mis-sized.  The
 *          CRC is reassembled byte by byte with explicit shifts, mirroring
 *          the encoder and staying independent of host endianness; the
 *          sequence number is rebuilt the same way from bytes 2 and 3.
 *
 *          Every failure returns false with no out parameter modified, so a
 *          caller cannot act on a half-filled header.
 *
 * @param[in]  raw               Decoded frame from @ref hwlink_cobs_decode.
 * @param[in]  raw_len           Length of @p raw in bytes.
 * @param[out] payload           Destination for the payload copy.
 * @param[in]  payload_capacity  Writable bytes available at @p payload.
 * @param[out] payload_len       Receives the payload length in bytes.
 * @param[out] flags             Receives header byte 1, the HWLINK_FLAG_* set.
 * @param[out] seq               Receives the little-endian sequence number.
 *
 * @return true when the frame is valid and its payload has been copied.
 * @retval true  Frame accepted; all four out parameters are written.
 * @retval false A pointer argument is NULL.
 * @retval false @p raw_len is below 8 or above @ref HWLINK_MAX_FRAME.
 * @retval false Byte 0 is not @ref HWLINK_PROTOCOL_MAJOR.
 * @retval false Byte 1 has a bit set outside the 0x03 flag mask.
 * @retval false The stored CRC disagrees with @ref app_crc32.
 * @retval false The payload does not fit in @p payload_capacity.
 *
 * @pre   @p raw holds @p raw_len readable bytes of an already decoded frame;
 *        @p payload addresses @p payload_capacity writable bytes and does not
 *        overlap @p raw, because the payload is copied with @c memcpy.
 * @post  On success @p payload holds a private copy of @p payload_len bytes and
 *        the frame can be released; on failure no out parameter is touched.
 * @note  The flag mask is spelled 0x03 here rather than composed from the
 *        HWLINK_FLAG_* macros, so a new flag bit must be added in both places
 *        or the decoder will reject frames that set it.
 * @note  The sequence number is not range-checked and not compared with any
 *        previous value; duplicate suppression, the direction rule and the
 *        reply cache all live in @ref comms_task.c.
 * @note  Rejection is silent by design (`LNK-004`): the caller drops the frame
 *        instead of sending a NAK, so a noisy link produces no reply traffic.
 * @see hwlink_cobs_decode, hwlink_build_raw, app_crc32
 */
bool hwlink_decode_raw(const uint8_t *raw, size_t raw_len, uint8_t *payload, size_t payload_capacity, size_t *payload_len, uint8_t *flags, uint16_t *seq)
{
    if(raw == NULL || payload == NULL || payload_len == NULL || flags == NULL || seq == NULL)
    {
        return false;
    }
    if(raw_len < HWLINK_OVERHEAD || raw_len > HWLINK_MAX_FRAME)
    {
        return false;
    }
    if(raw[0] != HWLINK_PROTOCOL_MAJOR || (raw[1] & ~0x03U) != 0U)
    {
        return false;
    }

    size_t   body_len = raw_len - HWLINK_CRC_SIZE;
    uint32_t expected = (uint32_t)raw[body_len] | ((uint32_t)raw[body_len + 1U] << 8U) | ((uint32_t)raw[body_len + 2U] << 16U) | ((uint32_t)raw[body_len + 3U] << 24U);
    if(app_crc32(raw, body_len) != expected)
    {
        return false;
    }

    size_t len = body_len - HWLINK_HEADER_SIZE;
    if(len > payload_capacity)
    {
        return false;
    }
    memcpy(payload, &raw[HWLINK_HEADER_SIZE], len);

    *payload_len = len;
    *flags       = raw[1];
    *seq         = (uint16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8U));
    return true;
}
