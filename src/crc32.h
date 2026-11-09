/**
 * @file    crc32.h
 * @brief   CRC-32/ISO-HDLC (the zlib polynomial) with no lookup table in RAM.
 * @details Implements CRC-32/ISO-HDLC, also known as CRC-32, CRC-32/ADCCP and
 *          the zlib/PNG CRC.  The parameter set is fixed by the bootloader
 *          contract and must not be changed:
 *          - width: 32 bits
 *          - polynomial: 0x04C11DB7, reflected form 0xEDB88320
 *          - initial value: 0xFFFFFFFF
 *          - input/output reflection: yes (refin = refout = true)
 *          - final XOR value: 0xFFFFFFFF
 *
 *          The checksum is computed bit by bit from the reflected polynomial
 *          instead of from a 1 KiB table, so the module keeps the CRC cost in
 *          code and uses no data RAM (a deliberate choice for the small,
 *          table-free image).  Results are bit-identical to zlib's crc32().
 *
 *          The CRC covers bytes in memory order and is the trailer of the
 *          HWLINK frame (`LNK-003`, `LNK-004`), the last word of the BOOT
 *          Appendix B CONFIRMED metadata record and the TLV trailer of the
 *          identity record.  The header exposes one function and defines no
 *          configuration macros.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     HWLINK LNK-003/004, BOOT Appendix B
 * @ingroup crc32
 */

#ifndef CRC32_H
#define CRC32_H

#include <stdint.h>
#include <stddef.h>

/**
 * @defgroup crc32 CRC-32 checksum
 * @brief Table-free CRC-32/ISO-HDLC shared by HWLINK, BOOT metadata and identity.
 * @details One algorithm instance is used everywhere so that a frame, a
 *          CONFIRMED record and a provisioning record all verify against the
 *          same polynomial, initial value and final XOR.
 * @{
 */

/**
 * @brief Compute the CRC-32/ISO-HDLC checksum of a byte buffer.
 * @details Walks @p len bytes, XORs each byte into the running register and
 *          then performs eight right shifts against the reflected polynomial
 *          0xEDB88320.  The register starts at 0xFFFFFFFF and the result is
 *          returned complemented, matching the standard definition above.
 *
 *          The checksum is defined over bytes in memory order, so callers that
 *          pass multi-byte word arrays (for example the CONFIRMED record in
 *          @ref flash_confirm.c) depend on the little-endian STM32F7 flash and
 *          RAM layout: the CRC must always be produced and compared over the
 *          same byte order, never over host-converted integers.
 *
 * @param[in] data Pointer to the first byte to checksum.  May be NULL only
 *                 when @p len is 0.
 * @param[in] len  Number of bytes to checksum.
 *
 * @return CRC-32 checksum of the first @p len bytes of @p data.
 * @retval 0x00000000 @p len is 0, i.e. the complement of the initial value.
 *
 * @pre   @p data addresses at least @p len readable bytes.
 * @post  @p data is left unchanged and no global state is modified.
 * @note  The function is pure and reentrant: it uses only locals, never
 *        blocks, allocates, locks or calls into the HAL, and may be called
 *        from task context or from an ISR.  Cost is about eight shift/XOR
 *        steps per byte, which is adequate for the <= 2 KiB HWLINK frames and
 *        the 32-byte metadata record it serves.
 * @note  The standard check value over the ASCII string "123456789" is
 *        0xCBF43926; any change to the constants above breaks HWLINK
 *        (`LNK-003`, `LNK-004`) and boot compatibility (BOOT Appendix B).
 * @warning No NULL check and no length clamp are performed.  Passing NULL
 *          @p data together with a non-zero @p len dereferences address 0.
 *          Callers are responsible for validating their buffers, as
 *          @ref hwlink_decode_raw does before verifying a frame.
 * @see hwlink_build_raw, hwlink_decode_raw, app_flash_confirm_running_slot,
 *      app_identity_refresh
 */
uint32_t app_crc32(const void *data, size_t len);

/** @} */

#endif /* CRC32_H */
