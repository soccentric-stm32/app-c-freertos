/**
 * @file    identity.h
 * @brief   Factory provisioning (PRV1) record reader and field accessors.
 * @details Reads the read-only PRV1 record that the factory station programs
 *          into bank 2 sector 13 and exposes its fields to the application.
 *          The record is bound to the chip by its 96-bit unique ID, so a
 *          blank, torn or foreign record is reported as absent and is never
 *          treated as valid identity.
 *
 *          Sector layout (STM32F767ZIT6, dual-bank, nDBANK = 0):
 *          - 0x08104000 - identity sector, bank 2 sector 13.
 *          - 0x08004000 - the same sector through the bank-swap alias, i.e.
 *            while bank 2 is mapped at 0x08000000.
 *
 *          The record starts with a 32-byte header:
 *          - 0  4  magic 0x31565250, the bytes "PRV1" (little-endian)
 *          - 4  2  format, must be 1
 *          - 6  2  provisioning revision, returned by
 *            @ref app_identity_revision
 *          - 8  4  TLV area length in bytes, 2..4096
 *          - 12 4  CRC-32 of the TLV area
 *          - 16 12 chip UID words 0..2 in flash byte order
 *          - 28 4  CRC-32 of header bytes 0..27
 *
 *          The TLV area follows immediately at +32 and is a sequence of
 *          (type, length, value) triples with 8-bit type and length; type 0
 *          ends the list.  This reader consumes serial (0x01), product
 *          (0x02), hardware revision (0x03), profile (0x06) and EUI-48 MAC
 *          (0x10); manufacturing time (0x04), station/fixture (0x05) and
 *          firmware version (0x07) are skipped.
 *
 *          Validation, in order: magic, format/length sanity, header CRC,
 *          UID match, TLV CRC, then a non-empty serial and product.  Only a
 *          record that passes every step is cached as valid.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-04
 * @target  STM32F767ZIT6
 * @srs     identity contract PRV1, factory A-02/A-03/A-05/A-06
 * @ingroup identity
 */

#ifndef IDENTITY_H
#define IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup identity Factory provisioning identity
 * @brief PRV1 record accessors used to build the HelloReply detail string.
 * @details The module keeps one cached copy of the record in RAM.  Every
 *          accessor loads it lazily on first use, so no explicit init call is
 *          required; @ref app_identity_refresh forces a re-read.  Text fields
 *          are copied into fixed cache buffers (32 characters for serial and
 *          product, 8 for the hardware revision, 64 for the profile) and stay
 *          NUL-terminated; longer TLV values are truncated rather than
 *          rejected.
 *
 *          The EUI-48 is copied byte for byte from its 0x10 TLV, so no bit
 *          manipulation is applied: the stored bytes keep the order the
 *          factory wrote, and no multicast or local-administered bits are set
 *          or cleared by this module.
 *
 *          Returned field strings point into module state and stay valid until
 *          the next @ref app_identity_refresh; copy them if they must outlive
 *          that call.
 * @{
 */

/**
 * @brief Reload and validate the PRV1 record from the identity sector.
 * @details Clears the cache, invalidates the data cache over the 4 KiB record
 *          window, then parses the primary copy at 0x08104000 and, when that
 *          fails, the bank-swap alias at 0x08004000.  Each attempt re-runs the
 *          full validation chain described in @ref identity.h, so a rejected
 *          copy never leaves the cache marked valid.
 *
 * @return true when either copy parsed and validated.
 * @retval true  A valid, UID-bound record is now cached.
 * @retval false Both copies are blank, torn, CRC-invalid or belong to another
 *               chip; all cached fields are cleared.
 *
 * @post On failure @ref app_identity_valid reports false and every accessor
 *       returns its empty value.
 * @note Read-only: the function never erases or programs flash and does not
 *       touch the bootloader metadata record.
 * @warning Not thread-safe.  The cache is rewritten in place, so the call must
 *          not overlap any other identity accessor.  Call it from task context
 *          before the comms task is serving requests, or serialise access
 *          externally.
 * @see app_crc32, app_identity_valid
 */
bool app_identity_refresh(void);

/**
 * @brief Report whether a valid, UID-bound record is cached.
 * @details Triggers the lazy load on first call, so the result reflects the
 *          flash contents even when @ref app_identity_refresh was never called
 *          explicitly.
 *
 * @return true when a record passed magic, CRC, UID and content checks.
 * @retval false No record is present, or the refresh already failed.
 *
 * @note A failed first load is remembered: the sector is not probed again
 *       until @ref app_identity_refresh is called.
 * @see app_identity_refresh
 */
bool app_identity_valid(void);

/**
 * @brief Serial number of the provisioned device.
 * @details Read from the 0x01 TLV, truncated to 32 characters.
 *
 * @return NUL-terminated serial number, or an empty string when no valid
 *         record is cached.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 * @see app_identity_product
 */
const char *app_identity_serial(void);

/**
 * @brief Product name of the provisioned device.
 * @details Read from the 0x02 TLV, truncated to 32 characters.  A record is
 *          only accepted when this field is non-empty.
 *
 * @return NUL-terminated product name, or an empty string when no valid record
 *         is cached.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 * @see app_identity_serial
 */
const char *app_identity_product(void);

/**
 * @brief Hardware revision string of the provisioned assembly.
 * @details Read from the 0x03 TLV, truncated to 8 characters.  Optional field:
 *          an absent TLV yields an empty string even for a valid record.
 *
 * @return NUL-terminated hardware revision, or an empty string when the field
 *         or the whole record is absent.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 */
const char *app_identity_hw_revision(void);

/**
 * @brief Provisioning profile name (for example a build or feature variant).
 * @details Read from the 0x06 TLV, truncated to 64 characters.  Optional field:
 *          an absent TLV yields an empty string even for a valid record.
 *
 * @return NUL-terminated profile name, or an empty string when the field or
 *         the whole record is absent.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 */
const char *app_identity_profile(void);

/**
 * @brief Provisioning record revision.
 * @details Taken from the 16-bit revision field of the 32-byte record header,
 *          not from a TLV.  It versions the provenance payload written by the
 *          factory station (A-02/A-03) and is independent of the firmware
 *          version.
 *
 * @return Record revision, or 0 when no valid record is cached.
 *
 * @warning Revision 0 is also a legal provisioned value, so this value alone
 *          cannot distinguish "unprovisioned" from "revision 0".  Check
 *          @ref app_identity_valid first when that matters.
 * @see app_identity_valid
 */
uint16_t app_identity_revision(void);

/**
 * @brief Copy the provisioned EUI-48 into a caller buffer.
 * @details The six bytes are read from the 0x10 TLV and copied verbatim; the
 *          module performs no byte swapping and no unicast/multicast or
 *          universal/local bit handling, so the caller receives the address
 *          exactly as the factory stored it.  A TLV whose length is not 6 is
 *          ignored, which leaves the MAC absent even for an otherwise valid
 *          record.
 *
 * @param[out] mac Buffer that receives the 6 address bytes.
 *
 * @return true when an EUI-48 was copied.
 * @retval true  A valid record with a 6-byte 0x10 TLV was cached.
 * @retval false No valid record, or the MAC field is absent or malformed;
 *               @p mac is left untouched.
 *
 * @pre @p mac addresses at least 6 writable bytes; the function does not check
 *      for NULL.
 * @note The record is not consulted again after the copy, so the buffer stays
 *       valid across @ref app_identity_refresh.
 * @see app_identity_detail
 */
bool app_identity_mac(uint8_t mac[6]);

/**
 * @brief Write a compact key=value identity summary for HelloReply (A-05).
 * @details Produces a single line with fixed key order:
 *          `PRV1 serial=...;mac=...;hw=...;product=...;profile=...;revision=N`.
 *          The MAC is rendered as six uppercase hex bytes separated by
 *          colons; when no MAC is present the `;mac=` key is emitted with an
 *          empty value.  The detail is informational only - it duplicates
 *          fields that also travel as dedicated HelloReply members.
 *
 * @param[out] out      Destination buffer for the NUL-terminated summary.
 * @param[in]  capacity Size of @p out in bytes, including the terminator.
 *
 * @return true when an identity was present and the summary was written.
 * @retval true  Valid record cached; @p out holds the key=value line.
 * @retval false @p out is NULL or @p capacity is 0, or no valid record is
 *               cached; in the latter case @p out holds "no identity" (or as
 *               much of it as fits).
 *
 * @pre @p out addresses @p capacity writable bytes when both are non-zero.
 * @post @p out is NUL-terminated on every path that receives a valid buffer.
 * @note Truncation is silent and not reported: with a small @p capacity the
 *       line is cut at a key or value boundary and still returns true.  The
 *       comms task passes a 192-byte buffer, which fits the longest fields
 *       (serial 32, product 32, profile 64, revision 5) only in the common
 *       case; an unusual record is shortened without warning.
 * @see app_identity_mac, app_identity_serial, app_identity_revision
 */
bool app_identity_detail(char *out, size_t capacity);

/** @} */

#endif /* IDENTITY_H */
