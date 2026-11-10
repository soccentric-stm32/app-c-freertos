/**
 * @file    identity.c
 * @brief   Factory provisioning record reader (A-03).
 * @details The factory station writes a PRV1 record into bank 2 sector 13
 *          (0x08104000, or 0x08004000 while bank 2 is swapped in).  This
 *          reader checks the 32-byte header, both CRC-32 values and the chip
 *          UID binding, then caches the serial, product, revision, profile
 *          and MAC for the rest of the application.
 *
 *          The record is bound to the chip by its 96-bit unique ID.  A blank,
 *          torn, CRC-invalid or foreign record is reported as absent and is
 *          never treated as valid identity.  Validation runs in the order:
 *          magic, format and length sanity, header CRC, UID match, TLV CRC,
 *          then a non-empty serial and product.  The parser stops at the
 *          first failure.
 *
 *          Sector layout (STM32F767ZIT6, dual-bank, nDBANK = 0):
 *          - 0x08104000 - identity sector, bank 2 sector 13.
 *          - 0x08004000 - the same sector through the bank-swap alias, i.e.
 *            while bank 2 is mapped at 0x08000000.
 *
 *          The public API and the full record layout are declared in
 *          @ref identity.h and belong to the @ref identity group.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-04
 * @target  STM32F767ZIT6
 * @srs     identity contract PRV1, factory A-02/A-03/A-05/A-06
 * @ingroup identity
 */

#include <string.h>
#include "stm32f7xx_hal.h"
#include "services/crc32.h"
#include "services/identity.h"

/** PRV1 magic word 0x31565250, the bytes "PRV1" in little-endian memory. */
#define ID_MAGIC 0x31565250UL /* "PRV1" little-endian */

/**
 * @brief Record format version accepted by this reader.
 * @details The only accepted value is 1.  A different format is rejected as
 *          absent rather than parsed with a compatibility fallback.
 */
#define ID_FORMAT 1U

/**
 * @brief Size of the PRV1 record header in bytes.
 * @details The TLV area starts immediately after this 32-byte header.  The
 *          header CRC covers the first 28 bytes, so the trailing CRC field
 *          itself is excluded.
 */
#define ID_HEADER_SIZE 32U

/**
 * @brief Maximum accepted TLV area length in bytes (4 KiB).
 * @details A header @c length outside 2..@ref ID_MAX_TLV is rejected.
 *          @ref parse_from first invalidates only @ref ID_HEADER_SIZE bytes to
 *          check the header, then invalidates @ref ID_HEADER_SIZE +
 *          @ref ID_MAX_TLV bytes before reading the TLV area, so both the
 *          header and a maximal TLV are read from flash.
 */
#define ID_MAX_TLV 4096U

/** Serial number TLV type (0x01), cached as a 32-character text field. */
#define ID_T_SERIAL 0x01U
/** Product name TLV type (0x02), cached as a 32-character text field. */
#define ID_T_PRODUCT 0x02U
/** Hardware revision TLV type (0x03), cached as an 8-character text field. */
#define ID_T_HW_REV 0x03U
/** Manufacturing time TLV type (0x04); recognised but skipped by this reader. */
#define ID_T_MFG_TIME 0x04U
/** Station/fixture TLV type (0x05); recognised but skipped by this reader. */
#define ID_T_STATION_FIXTURE 0x05U
/** Provisioning profile TLV type (0x06), cached as a 64-character text field. */
#define ID_T_PROFILE 0x06U
/** Firmware version TLV type (0x07); recognised but skipped by this reader. */
#define ID_T_FIRMWARE_VERSION 0x07U
/** EUI-48 MAC TLV type (0x10), cached only when its length is exactly 6. */
#define ID_T_MAC 0x10U

/**
 * @brief Primary PRV1 record address: bank 2, 0x08104000.
 * @details This is the address the factory station programs and the first
 *          address @ref app_identity_refresh tries.
 */
#define ID_IDENTITY_A 0x08104000UL

/**
 * @brief Bank-swap alias of @ref ID_IDENTITY_A (0x08004000) 0x08004000.
 * @details Used when BOOT has mapped bank 2 at 0x08000000 through
 *          @c SYSCFG_MEMRMP.SWP_FB.  It is tried only when the primary copy
 *          fails validation.
 */
#define ID_IDENTITY_SWAPPED 0x08004000UL

/**
 * @brief Decoded PRV1 record header, 32 bytes on flash.
 * @details The record header is copied out of flash into this structure before
 *          validation.  The field order and natural alignment produce the
 *          on-flash layout; the structure must not be reordered.
 * @note @ref header_crc covers bytes 0..27, so @c header_crc32 is the only
 *       field outside the checksum.
 */
typedef struct
{
    uint32_t magic;        /**< @ref ID_MAGIC ("PRV1") when a record is present. */
    uint16_t format;       /**< Record format; must equal @ref ID_FORMAT. */
    uint16_t revision;     /**< Provisioning revision reported to the host. */
    uint32_t length;       /**< TLV area length in bytes, 2..@ref ID_MAX_TLV. */
    uint32_t tlv_crc32;    /**< CRC-32/ISO-HDLC of the TLV area. */
    uint8_t  uid[12];      /**< Chip UID words 0..2 in flash byte order. */
    uint32_t header_crc32; /**< CRC-32/ISO-HDLC of header bytes 0..27. */
} id_header_t;

/**
 * @brief Cached, validated identity fields.
 * @details The structure is filled only after every validation step succeeds.
 *          Text fields are NUL-terminated and may be shorter than the TLV
 *          value because @ref copy_text truncates to the field capacity.  An
 *          all-zero structure with @c valid == false is the failure state.
 * @note @c valid is the authority for every accessor.  Fields may hold partial
 *       data after a failed parse and must not be used directly.
 */
typedef struct
{
    bool     valid;          /**< True only after a fully validated record. */
    uint16_t revision;       /**< Header revision, 0 when not valid. */
    char     serial[33];     /**< 0x01 TLV, up to 32 characters plus NUL. */
    char     product[33];    /**< 0x02 TLV, up to 32 characters plus NUL. */
    char     hw_revision[9]; /**< 0x03 TLV, up to 8 characters plus NUL. */
    char     profile[65];    /**< 0x06 TLV, up to 64 characters plus NUL. */
    uint8_t  mac[6];         /**< 0x10 TLV EUI-48, copied verbatim. */
    bool     mac_valid;      /**< True when a 6-byte MAC TLV was copied. */
} app_identity_t;

/** Single cached identity record; owned by this translation unit. */
static app_identity_t identity;

/**
 * @brief True once @ref app_identity_refresh has run, successfully or not.
 * @details Keeps a failed first load from probing flash again on every
 *          accessor call.  Only @ref app_identity_refresh clears it.
 */
static bool refreshed;

/**
 * @brief Compute the CRC-32 of the first 28 bytes of a raw record header.
 * @details The factory stores the result in @c header_crc32, which lies at
 *          offset 28 and is therefore not part of the checksummed range.
 *
 * @param[in] raw Pointer to at least @ref ID_HEADER_SIZE raw header bytes.
 *
 * @return CRC-32/ISO-HDLC over @c raw[0..27].
 *
 * @pre @p raw addresses at least 28 readable bytes; there is no NULL check.
 * @post @p raw is left unchanged and no global state is modified.
 * @note The function is pure and reentrant; it delegates to @ref app_crc32.
 * @see app_crc32, id_header_t
 */
static uint32_t header_crc(const uint8_t *raw) { return app_crc32(raw, 28U); }

/**
 * @brief Copy a TLV value into a fixed-size NUL-terminated text buffer.
 * @details Copies at most @p length bytes, truncating to @p cap - 1 when the
 *          TLV value is longer than the destination, then appends a NUL.
 *          Truncation is silent and is not reported to the caller.
 *
 * @param[out] out    Destination text buffer.
 * @param[in]  cap    Size of @p out in bytes, including the terminator.
 * @param[in]  data   TLV value bytes; the range need not contain a NUL.
 * @param[in]  length TLV value length in bytes, 0..255.
 *
 * @pre @p out addresses @p cap writable bytes and @p cap is at least 1.
 * @pre @p data addresses @p length readable bytes.
 * @post @p out is NUL-terminated at index min(@p length, @p cap - 1).
 * @note Embedded NUL bytes are copied like any other byte, so the resulting C
 *       string stops at the first embedded NUL even though the TLV continues.
 */
static void copy_text(char *out, size_t cap, const uint8_t *data, uint8_t length)
{
    size_t n = length;
    if(n >= cap)
    {
        n = cap - 1U;
    }
    memcpy(out, data, n);
    out[n] = '\0';
}

/**
 * @brief Validate and parse one PRV1 record at a given flash address.
 * @details Performs the complete validation chain and only then populates the
 *          module cache:
 *          1. Invalidate the data cache over the 32-byte header only.
 *          2. Copy the header and test @ref ID_MAGIC.
 *          3. Test format and TLV length sanity (2..@ref ID_MAX_TLV).
 *          4. Compare @ref header_crc with the stored header CRC.
 *          5. Compare the 96-bit chip UID with @c header.uid.
 *          6. Invalidate the data cache over the header plus the maximal TLV
 *             window before touching the TLV area.
 *          7. Compare @ref app_crc32 over the TLV area with @c tlv_crc32.
 *          8. Clear the cache, parse the TLV list and require a non-empty
 *             serial and product before setting @c identity.valid.
 *
 *          The TLV list is a sequence of 8-bit type, 8-bit length and value
 *          bytes.  Type 0 ends the list.  A TLV that would extend past
 *          @c header.length aborts the parse.  Recognised text types are
 *          truncated into their cache fields; a MAC TLV is accepted only when
 *          its length is exactly 6.  Unknown types, including 0x04, 0x05 and
 *          0x07, are skipped without error.
 *
 *          Failure paths return @c false without setting @c identity.valid.
 *          A failure before the cache is cleared returns without modifying
 *          @c identity; a structural TLV failure after it leaves partial
 *          fields behind, but @c identity.valid remains false so the public
 *          accessors never expose them.
 *
 * @param[in] address Flash address of the candidate record header.
 *
 * @return true when the record passed every check and is cached as valid.
 * @retval true  Valid, UID-bound record parsed and cached.
 * @retval false Magic, format, length, CRC, UID or content check failed.
 *
 * @pre @p address points to a readable, naturally aligned record and the
 *      cache maintenance API accepts the address.
 * @post On success @c identity.valid is true and all parsed fields are cached.
 * @post On failure @c identity.valid is false; the cache may be zeroed or
 *       partially populated but is never exposed as valid.
 *
 * @note This function does not touch flash programming, locks or the
 *       bootloader metadata record.
 * @note The header-only invalidation avoids touching the TLV window when the
 *       record is absent.  Before any TLV access the invalidation still covers
 *       the complete record window, @ref ID_HEADER_SIZE + @ref ID_MAX_TLV
 *       bytes, so a maximal TLV is read from flash rather than from stale
 *       cache lines.
 * @see app_identity_refresh, app_crc32
 */
static bool parse_from(uint32_t address)
{
    uint8_t         raw[ID_HEADER_SIZE];
    const uint32_t *words = (const uint32_t *)address;

    SCB_InvalidateDCache_by_Addr((uint32_t *)address, ID_HEADER_SIZE);
    memcpy(raw, (const void *)address, sizeof(raw));
    if(words[0] != ID_MAGIC)
    {
        return false;
    }

    id_header_t header;
    memcpy(&header, raw, sizeof(header));
    if(header.format != ID_FORMAT || header.length == 0U || header.length > ID_MAX_TLV || header.length < 2U)
    {
        return false;
    }
    if(header_crc(raw) != header.header_crc32)
    {
        return false;
    }

    /* UID binding: the record has the three UID words in flash byte order. */
    uint8_t  uid[12];
    uint32_t uid_words[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
    memcpy(uid, uid_words, sizeof(uid));
    if(memcmp(uid, header.uid, sizeof(uid)) != 0)
    {
        return false;
    }

    /* The header is valid, so read the TLV area.  Invalidate the fixed full
     * window first: a shorter length-based invalidation could leave stale tail
     * bytes in cache. */
    SCB_InvalidateDCache_by_Addr((uint32_t *)address, ID_HEADER_SIZE + ID_MAX_TLV);
    const uint8_t *tlv = (const uint8_t *)(address + ID_HEADER_SIZE);
    if(app_crc32(tlv, header.length) != header.tlv_crc32)
    {
        return false;
    }

    memset(&identity, 0, sizeof(identity));
    identity.revision = header.revision;
    size_t offset     = 0U;
    while(offset + 2U <= header.length)
    {
        uint8_t type   = tlv[offset];
        uint8_t length = tlv[offset + 1U];
        offset += 2U;
        if(type == 0U)
        {
            break;
        }
        if(offset + length > header.length)
        {
            return false;
        }
        const uint8_t *value = &tlv[offset];
        switch(type)
        {
        case ID_T_SERIAL: copy_text(identity.serial, sizeof(identity.serial), value, length); break;
        case ID_T_PRODUCT: copy_text(identity.product, sizeof(identity.product), value, length); break;
        case ID_T_HW_REV: copy_text(identity.hw_revision, sizeof(identity.hw_revision), value, length); break;
        case ID_T_PROFILE: copy_text(identity.profile, sizeof(identity.profile), value, length); break;
        case ID_T_MAC:
            if(length == 6U)
            {
                memcpy(identity.mac, value, 6U);
                identity.mac_valid = true;
            }
            break;
        default: break;
        }
        offset += length;
    }

    if(identity.serial[0] == '\0' || identity.product[0] == '\0')
    {
        return false;
    }
    identity.valid = true;
    return true;
}

/**
 * @brief Reload and validate the PRV1 record from the identity sector.
 * @details Marks the cache as refreshed, clears it, then tries the primary
 *          record at @ref ID_IDENTITY_A and, when that fails, the bank-swap
 *          alias at @ref ID_IDENTITY_SWAPPED.  Each attempt runs the full
 *          validation chain through @ref parse_from.  A failed load is
 *          remembered, so the sector is not probed again until this function
 *          is called again.
 *
 * @return true when either copy parsed and validated.
 * @retval true  A valid, UID-bound record is now cached.
 * @retval false Both copies are blank, torn, CRC-invalid, belong to another
 *               chip, or lack a serial or product; no record is exposed.
 *
 * @post On failure @ref app_identity_valid reports false and every accessor
 *       returns its empty value.
 * @note Read-only: the function never erases or programs flash and does not
 *       touch the bootloader metadata record.
 * @warning Not thread-safe.  The cache is rewritten in place, so the call must
 *          not overlap any other identity accessor.  Call it from task context
 *          before the comms task is serving requests, or serialise access
 *          externally.
 * @see parse_from, app_identity_valid, app_crc32
 */
bool app_identity_refresh(void)
{
    refreshed = true;
    memset(&identity, 0, sizeof(identity));
    if(parse_from(ID_IDENTITY_A))
    {
        return true;
    }
    return parse_from(ID_IDENTITY_SWAPPED);
}

/**
 * @brief Lazily load the record on first use and report cache validity.
 * @details Calls @ref app_identity_refresh exactly once per refresh cycle;
 *          later calls only read @c identity.valid.  A failed first load is
 *          remembered until the next explicit refresh.
 *
 * @return @c identity.valid.
 * @retval true  A valid record is cached.
 * @retval false No record has been loaded, or the last load failed.
 *
 * @note Not thread-safe when it triggers the first load, because that rewrites
 *       the shared cache.  Callers must not race the first access.
 * @see app_identity_refresh, app_identity_valid
 */
static bool ensure_loaded(void)
{
    if(!refreshed)
    {
        (void)app_identity_refresh();
    }
    return identity.valid;
}

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
 * @see ensure_loaded, app_identity_refresh
 */
bool app_identity_valid(void) { return ensure_loaded(); }

/**
 * @brief Return the provisioned serial number.
 * @details Reads the 0x01 TLV from the cache, truncated to 32 characters by
 *          @ref copy_text.  The record is loaded lazily on first use.
 *
 * @return NUL-terminated serial number, or an empty string when no valid
 *         record is cached.
 * @retval "" No valid record; the empty literal has static storage.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 * @see app_identity_product, app_identity_detail
 */
const char *app_identity_serial(void) { return ensure_loaded() ? identity.serial : ""; }

/**
 * @brief Return the provisioned product name.
 * @details Reads the 0x02 TLV from the cache, truncated to 32 characters by
 *          @ref copy_text.  A record is accepted only when this field is
 *          non-empty.
 *
 * @return NUL-terminated product name, or an empty string when no valid
 *         record is cached.
 * @retval "" No valid record; the empty literal has static storage.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 * @see app_identity_serial, app_identity_detail
 */
const char *app_identity_product(void) { return ensure_loaded() ? identity.product : ""; }

/**
 * @brief Return the hardware revision string of the provisioned assembly.
 * @details Reads the optional 0x03 TLV from the cache, truncated to 8
 *          characters.  An absent TLV yields an empty string even for an
 *          otherwise valid record.
 *
 * @return NUL-terminated hardware revision, or an empty string when the field
 *         or the whole record is absent.
 * @retval "" No valid record, or no hardware revision TLV.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 * @see app_identity_profile, app_identity_detail
 */
const char *app_identity_hw_revision(void) { return ensure_loaded() ? identity.hw_revision : ""; }

/**
 * @brief Return the provisioning profile name.
 * @details Reads the optional 0x06 TLV from the cache, truncated to 64
 *          characters.  An absent TLV yields an empty string even for an
 *          otherwise valid record.
 *
 * @return NUL-terminated profile name, or an empty string when the field or
 *         the whole record is absent.
 * @retval "" No valid record, or no profile TLV.
 *
 * @note The pointer refers to module-owned RAM and is invalidated by
 *       @ref app_identity_refresh.
 * @see app_identity_hw_revision, app_identity_detail
 */
const char *app_identity_profile(void) { return ensure_loaded() ? identity.profile : ""; }

/**
 * @brief Return the provisioning record revision.
 * @details Taken from the 16-bit revision field of the 32-byte record header,
 *          not from a TLV.  It versions the provenance payload written by the
 *          factory station (A-02/A-03) and is independent of the firmware
 *          version.
 *
 * @return Record revision, or 0 when no valid record is cached.
 * @retval 0 No valid record, or a valid record whose revision is 0.
 *
 * @warning Revision 0 is also a legal provisioned value, so this value alone
 *          cannot distinguish "unprovisioned" from "revision 0".  Check
 *          @ref app_identity_valid first when that matters.
 * @see app_identity_valid, app_identity_detail
 */
uint16_t app_identity_revision(void) { return ensure_loaded() ? identity.revision : 0U; }

/**
 * @brief Copy the provisioned EUI-48 into a caller buffer.
 * @details The six bytes are read from the 0x10 TLV and copied verbatim; the
 *          module performs no byte swapping and no unicast/multicast or
 *          universal/local bit handling, so the caller receives the address
 *          exactly as the factory stored it.  A TLV whose length is not 6 is
 *          ignored, leaving the MAC absent even for an otherwise valid record.
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
 * @post On success @p mac holds the six factory bytes in flash order.
 * @note The record is not consulted again after the copy, so the buffer stays
 *       valid across @ref app_identity_refresh.
 * @see app_identity_detail, parse_from
 */
bool app_identity_mac(uint8_t mac[6])
{
    if(!ensure_loaded() || !identity.mac_valid)
    {
        return false;
    }
    memcpy(mac, identity.mac, 6U);
    return true;
}

/**
 * @brief Append a NUL-terminated string to a bounded output buffer.
 * @details Copies while there is room for the next character plus the
 *          terminator, so the buffer is always terminated on return when
 *          @p cap is at least 1.  Truncation is silent.
 *
 * @param[out] out  Destination buffer.
 * @param[in]  cap  Capacity of @p out in bytes, including the terminator.
 * @param[in]  pos  Current write offset; must not exceed @p cap - 1.
 * @param[in]  text String to append; NULL is treated as an empty string.
 *
 * @return New write offset, i.e. the number of bytes before the terminator.
 *
 * @pre @p out addresses @p cap writable bytes and @p cap is at least 1.
 * @pre @p pos is at most @p cap - 1.
 * @post @p out[return value] is '\0'.
 * @note The helper writes at least the terminator even when nothing is copied,
 *       which is why the caller must keep @p cap non-zero.
 * @see app_identity_detail
 */
static size_t append_text(char *out, size_t cap, size_t pos, const char *text)
{
    while(text != NULL && *text != '\0' && pos + 1U < cap)
    {
        out[pos++] = *text++;
    }
    out[pos] = '\0';
    return pos;
}

/**
 * @brief Append an unsigned decimal integer to a bounded output buffer.
 * @details Converts @p value to decimal digits in a local 10-byte array and
 *          emits them in reverse order.  The array is large enough for the
 *          full @c unsigned range, so the value itself is never truncated;
 *          output is cut only when the destination is too small.
 *
 * @param[out] out   Destination buffer.
 * @param[in]  cap   Capacity of @p out in bytes, including the terminator.
 * @param[in]  pos   Current write offset; must not exceed @p cap - 1.
 * @param[in]  value Value to format, including 0.
 *
 * @return New write offset after the digits.
 *
 * @pre @p out addresses @p cap writable bytes and @p cap is at least 1.
 * @pre @p pos is at most @p cap - 1.
 * @post @p out[return value] is '\0'.
 * @note The helper does not pad, sign or separate digits and emits no leading
 *       zeros except for the value 0 itself.
 * @see app_identity_detail
 */
static size_t append_uint(char *out, size_t cap, size_t pos, unsigned value)
{
    char   digits[10];
    size_t count = 0U;
    do
    {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while(value != 0U && count < sizeof(digits));
    while(count > 0U && pos + 1U < cap)
    {
        out[pos++] = digits[--count];
    }
    out[pos] = '\0';
    return pos;
}

/**
 * @brief Append the cached MAC as colon-separated uppercase hexadecimal.
 * @details Renders the six bytes of @c identity.mac in order, for example
 *          `02:00:00:00:00:01`.  The caller is responsible for checking
 *          @c identity.mac_valid first; this helper reads the cache directly.
 *          If the buffer cannot hold another three characters plus the
 *          terminator, formatting stops and the partial result is terminated.
 *
 * @param[out] out Destination buffer.
 * @param[in]  cap Capacity of @p out in bytes, including the terminator.
 * @param[in]  pos Current write offset; must not exceed @p cap - 1.
 *
 * @return New write offset after the last character written.
 *
 * @pre @p out addresses @p cap writable bytes and @p cap is at least 1.
 * @pre @p pos is at most @p cap - 1.
 * @post @p out[return value] is '\0'.
 * @note No byte order change is performed: byte 0 of the TLV is formatted
 *       first, so the text matches the factory-programmed sequence.
 * @see app_identity_mac, app_identity_detail
 */
static size_t append_mac(char *out, size_t cap, size_t pos)
{
    static const char hex[] = "0123456789ABCDEF"; /**< Uppercase hex digits. */
    for(size_t i = 0U; i < 6U && pos + 3U < cap; i++)
    {
        if(i != 0U)
        {
            out[pos++] = ':';
        }
        out[pos++] = hex[(identity.mac[i] >> 4U) & 0xFU];
        out[pos++] = hex[identity.mac[i] & 0xFU];
    }
    out[pos] = '\0';
    return pos;
}

/**
 * @brief Write a compact key=value identity summary for HelloReply (A-05).
 * @details Produces one line with fixed key order:
 *          `PRV1 serial=...;mac=...;hw=...;product=...;profile=...;revision=N`.
 *          The MAC is rendered as six uppercase hex bytes separated by
 *          colons; when no MAC is present the `;mac=` key is emitted with an
 *          empty value.  The detail is informational only: it duplicates
 *          fields that also travel as dedicated HelloReply members.
 *
 *          The function first empties @p out, then handles the no-record case
 *          by writing `no identity` and returning false.  With a valid record
 *          it appends each key and value through @ref append_text,
 *          @ref append_mac and @ref append_uint.  Truncation is silent and
 *          does not change the return value.
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
 * @note The comms task passes a 192-byte buffer (see `comms_task.c`), which is
 *       enough for typical provisioned values; longer serial, product or
 *       profile fields are shortened without warning.
 * @note May trigger the lazy load through @ref ensure_loaded, so the first
 *       call is not safe to race with another identity accessor.
 * @see app_identity_mac, app_identity_serial, app_identity_revision
 */
bool app_identity_detail(char *out, size_t capacity)
{
    if(out == NULL || capacity == 0U)
    {
        return false;
    }
    out[0] = '\0';
    if(!ensure_loaded())
    {
        (void)append_text(out, capacity, 0U, "no identity");
        return false;
    }
    size_t pos = 0U;
    pos        = append_text(out, capacity, pos, "PRV1 serial=");
    pos        = append_text(out, capacity, pos, identity.serial);
    pos        = append_text(out, capacity, pos, ";mac=");
    if(identity.mac_valid)
    {
        pos = append_mac(out, capacity, pos);
    }
    pos = append_text(out, capacity, pos, ";hw=");
    pos = append_text(out, capacity, pos, identity.hw_revision);
    pos = append_text(out, capacity, pos, ";product=");
    pos = append_text(out, capacity, pos, identity.product);
    pos = append_text(out, capacity, pos, ";profile=");
    pos = append_text(out, capacity, pos, identity.profile);
    pos = append_text(out, capacity, pos, ";revision=");
    (void)append_uint(out, capacity, pos, identity.revision);
    return true;
}
