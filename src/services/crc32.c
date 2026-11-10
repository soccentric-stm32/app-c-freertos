/**
 * @file    crc32.c
 * @brief   Table-free CRC-32/ISO-HDLC implementation shared by HWLINK, BOOT
 *          metadata and the factory identity record.
 * @details Implements the only function declared in @ref crc32.h.  The module
 *          owns no state: the whole checksum is computed bit by bit from the
 *          reflected polynomial, so there is no 1 KiB lookup table in RAM and
 *          no initialisation step to order against the rest of the firmware.
 *
 *          The algorithm parameters are fixed by the bootloader contract and
 *          must never be changed:
 *          - width: 32 bits
 *          - polynomial: 0x04C11DB7 (normal), 0xEDB88320 (reflected)
 *          - initial value: 0xFFFFFFFF
 *          - input reflection (refin): yes
 *          - output reflection (refout): yes
 *          - final XOR value (xorout): 0xFFFFFFFF
 *          - check value over "123456789": 0xCBF43926
 *
 *          This parameter set makes the result bit-identical to zlib's
 *          crc32(), which is what the host tooling and the bootloader expect.
 *          The checksum is defined over bytes in memory order, never over
 *          host-converted integers: on the little-endian STM32F7 the 32-bit
 *          result is therefore compared against, or stored as, a native
 *          little-endian word, and the HWLINK frame trailer serialises the
 *          same value explicitly little-endian (see @ref hwlink_build_raw and
 *          @ref hwlink_decode_raw).
 *
 *          Consumers: the HWLINK frame trailer (`LNK-003`, `LNK-004`), the
 *          last word of the BOOT Appendix B CONFIRMED metadata record
 *          (@ref flash_confirm.c) and both CRC fields of the PRV1 identity
 *          record (@ref identity.c).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6 (NUCLEO-F767ZI)
 * @srs     HWLINK LNK-003/004, BOOT Appendix B
 * @ingroup crc32
 */

#include "services/crc32.h"

/**
 * @brief Compute the CRC-32/ISO-HDLC checksum of a byte buffer.
 * @details The running register is loaded with 0xFFFFFFFF.  For every input
 *          byte, the byte is XORed into the low bits of the register and the
 *          register is then shifted right eight times; whenever the shifted
 *          out bit is 1 the reflected polynomial 0xEDB88320 is XORed back in.
 *          This is the bit-serial form of the reflected (LSB-first) update and
 *          is equivalent to a table-driven implementation using the same
 *          parameters.  Finally the register is complemented with a XOR of
 *          0xFFFFFFFF and returned.
 *
 *          The function walks @p data strictly in ascending memory order and
 *          treats the buffer as an opaque byte string.  It performs no
 *          alignment assumptions, so unaligned buffers and @c volatile flash
 *          windows are acceptable; the @c volatile qualifier on the caller
 *          side is dropped by the @c const @c void pointer here, which is fine
 *          because the function only reads.
 *
 * @param[in] data Pointer to the first byte to checksum.  May be NULL only
 *                 when @p len is 0.
 * @param[in] len  Number of bytes to checksum, in bytes.  A length of 0 is
 *                 valid and yields 0x00000000.
 *
 * @return CRC-32/ISO-HDLC checksum of the first @p len bytes of @p data.
 * @retval 0x00000000 @p len is 0 (the complement of the initial value).
 *
 * @pre   @p data addresses at least @p len readable bytes.  The function
 *        performs no bounds checking of its own.
 * @pre   The byte range must already be coherent: callers that read flash
 *        through the D-cache must invalidate the range first, as
 *        @ref app_flash_confirm_running_slot and @ref app_identity_refresh do.
 * @post  @p data is left unmodified and no global or static state changes.
 *
 * @note Pure and reentrant.  The function uses only automatic variables, never
 *       blocks, allocates, locks, or calls into the HAL, and is safe to call
 *       from task context or from an ISR, including concurrently from several
 *       contexts on different buffers.  It is not a synchronisation point and
 *       must not be relied on as one.
 * @note Execution cost is roughly eight shift/XOR iterations per input byte,
 *       which is acceptable for the frames and records this firmware checks
 *       (HWLINK frames up to @c HWLINK_MAX_FRAME, the 28-byte CONFIRMED
 *       payload and the PRV1 header/TLV blocks) but is not intended for bulk
 *       image verification.
 * @note The module is deliberately table-free: trading CPU cycles for RAM
 *       keeps the read-only image small and avoids a shared table that would
 *       have to be initialised or kept in flash.
 * @warning No NULL check and no length clamp are performed.  Passing NULL
 *          @p data together with a non-zero @p len dereferences address 0.
 *          Callers are responsible for validating their buffers before
 *          calling.
 * @warning Any change to the polynomial, initial value, reflection or final
 *          XOR breaks wire compatibility with the host tools (`LNK-003`,
 *          `LNK-004`), invalidates every stored CONFIRMED record and makes
 *          every fielded PRV1 identity record unreadable.  Do not "optimise"
 *          these constants locally.
 * @see crc32.h, hwlink_build_raw, hwlink_decode_raw,
 *      app_flash_confirm_running_slot, app_identity_refresh
 */
uint32_t app_crc32(const void *data, size_t len)
{
    const uint8_t *p   = (const uint8_t *)data;
    uint32_t       crc = 0xFFFFFFFFUL;

    for(size_t i = 0U; i < len; i++)
    {
        crc ^= p[i];
        for(uint32_t bit = 0U; bit < 8U; bit++)
        {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320UL : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}
