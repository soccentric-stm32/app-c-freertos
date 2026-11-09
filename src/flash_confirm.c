/**
 * @file    flash_confirm.c
 * @brief   Program the single CONFIRMED metadata record of the running slot.
 * @details Implementation of @ref flash_confirm.h, and the only code in DEMO
 *          that writes flash (`DBT-006`, `DSAFE-002`).  BOOT keeps one
 *          32-byte Appendix B record per slot in the metadata sector and
 *          accepts a trial slot permanently only once that record verifies,
 *          so this file turns the running slot from "on trial" into
 *          "confirmed" by making exactly one record valid.  The Health task
 *          calls @ref app_flash_confirm_running_slot once, 5 s after
 *          start-up, after every health condition has passed (`HLT-002`,
 *          `HLT-005`, see @ref health_task.c).
 *
 *          **Record layout.**  Appendix B defines a 32-byte record area,
 *          @ref APP_META_RECORD_SIZE, that is @ref CONFIRMED_WORDS (8)
 *          little-endian 32-bit words:
 *          - word 0: @ref APP_META_CONFIRMED_MAGIC, `0x434F4E46` ("CONF")
 *          - words 1..6: `0x00000000`
 *          - word 7: CRC-32/ISO-HDLC over bytes 0..27, that is words 0..6
 *
 *          The image is therefore built in RAM first (an eight-word stack
 *          buffer), because the word-7 checksum has to be known before the
 *          first word is programmed.  Both the write and the check use
 *          @ref app_crc32, the table-free CRC-32/ISO-HDLC shared with HWLINK
 *          and the identity record, computed over memory byte order; no
 *          integer is ever byte-swapped on the way in or out.
 *
 *          **Three record states.**  *Valid*: magic @em and CRC match, the
 *          slot is confirmed.  *Absent*: all 32 bytes read `0xFF`, nothing was
 *          ever programmed, so this file may write it.  *Torn*: anything else,
 *          for example a magic/CRC mismatch after an interrupted write; it is
 *          reported as a failure and left untouched, because BOOT owns the
 *          recovery decision for the metadata sector.  Only the absent state
 *          is ever programmed, and nothing here ever erases: the same sector
 *          also holds BOOT's header, VERIFIED, ACTIVATED and REJECTED
 *          records, and an application-side erase would destroy them.
 *
 *          **Address.**  @ref APP_CONFIRMED_ADDR (`0x08008140`, the CONFIRMED
 *          offset of the running slot's metadata sector) is compiled in and
 *          never comes from the caller, so the helper cannot be pointed at
 *          another slot or address at run time (`DSAFE-002`).
 *
 *          **State.**  This file has no file-scope variables: the record image
 *          and every loop counter live on the caller's stack and the function
 *          remembers nothing between calls.  Flash is the only state that
 *          changes, which is why the idempotency check has to read flash
 *          instead of trusting a RAM flag.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, dual-bank flash
 * @srs     HLT-002, HLT-005, DBT-006, DSAFE-002
 * @ingroup flash_confirm
 * @see     flash_confirm.h
 * @see     boot_contract.h
 * @see     crc32.h
 * @see     health_task.c
 */

#include "stm32f7xx_hal.h"
#include "boot_contract.h"
#include "crc32.h"
#include "flash_confirm.h"

/**
 * @brief Number of 32-bit words in one Appendix B metadata record: 8.
 * @details Derived from @ref APP_META_RECORD_SIZE (32) rather than written as
 *          a literal, so the stack image, the CRC length and both record
 *          loops follow @ref boot_contract.h automatically.  Word 0 carries
 *          the magic, words 1..6 are zero, and word
 *          `CONFIRMED_WORDS - 1` carries the CRC, which is why the two index
 *          expressions below read as `[0]` and `[CONFIRMED_WORDS - 1U]`.
 * @note The division is integer arithmetic, so a future
 *       @ref APP_META_RECORD_SIZE that is not a multiple of 4 would be
 *       truncated here silently instead of failing the build.
 * @see APP_META_RECORD_SIZE
 * @see APP_META_CONFIRMED_MAGIC
 */
#define CONFIRMED_WORDS (APP_META_RECORD_SIZE / 4U)

/**
 * @brief Test whether a CONFIRMED record in flash is valid.
 * @details Checks the magic word first, so an erased (`0xFFFFFFFF`) record or
 *          unrelated metadata is rejected without paying for the checksum.
 *          When the magic matches, CRC-32/ISO-HDLC is computed over the first
 *          `APP_META_RECORD_SIZE - 4U` (28) bytes, that is words 0..6, and the
 *          result is compared with the stored checksum in word 7.
 *
 *          The record is read in place from flash rather than copied, and the
 *          reads go through the @c volatile pointer so the compiler fetches
 *          them here instead of reusing a value it might already hold.  The
 *          cast to `const void *` for @ref app_crc32 drops @c volatile, which
 *          is safe because the record is only read and never written through
 *          that pointer.
 *
 * @param[in] record Pointer to the 32-byte record in flash, word aligned.  The
 *                   @c volatile qualifier is part of the contract: it keeps
 *                   the word loads out of the optimiser.
 *
 * @return true when both the magic and the stored CRC match the record.
 * @retval false The magic differs, or the CRC over words 0..6 does not match
 *               word 7.
 *
 * @pre   @p record addresses @ref APP_META_RECORD_SIZE readable bytes inside
 *        the metadata sector, and the D-cache has already been invalidated
 *        for that range by the caller; this helper does no cache maintenance.
 * @post  Flash and all RAM state are left unchanged.
 *
 * @note A matching magic is only a fast filter, not proof: the combined
 *       magic/CRC test is what the BOOT contract accepts, and a record with
 *       magic and CRC intact but non-zero words 1..6 still verifies.
 * @note The 28-byte @ref app_crc32 walk runs only after the magic matched, so
 *       an erased or unrelated record costs a single word read.
 * @see record_erased
 * @see app_crc32
 */
static bool record_valid(const volatile uint32_t *record)
{
    if (record[0] != APP_META_CONFIRMED_MAGIC) {
        return false;
    }
    return app_crc32((const void *)record, APP_META_RECORD_SIZE - 4U) ==
           record[CONFIRMED_WORDS - 1U];
}

/**
 * @brief Test whether a CONFIRMED record still reads as erased flash.
 * @details Walks the @ref CONFIRMED_WORDS words in ascending address order and
 *          returns at the first word that is not `0xFFFFFFFF`.  All `0xFF`
 *          means nothing has programmed this record since the metadata sector
 *          was last erased, which is the only state this file is allowed to
 *          program; anything else is treated as torn.
 *
 * @param[in] record Pointer to the 32-byte record in flash, word aligned.
 *
 * @return true only when every one of the @ref CONFIRMED_WORDS words reads
 *         `0xFFFFFFFF`.
 * @retval false At least one word contains a programmed bit, including records
 *               whose bytes are only partly written.
 *
 * @pre   @p record addresses @ref APP_META_RECORD_SIZE readable bytes and the
 *        D-cache has already been invalidated for that range.
 * @post  Flash and all RAM state are left unchanged.
 *
 * @note The test is word granular: one programmed byte makes its whole word
 *       differ from `0xFFFFFFFF`, so a half-written record is reported as torn
 *       rather than absent and is therefore never retried through
 *       @ref app_flash_confirm_running_slot.
 * @see record_valid
 */
static bool record_erased(const volatile uint32_t *record)
{
    for (uint32_t i = 0U; i < CONFIRMED_WORDS; i++) {
        if (record[i] != 0xFFFFFFFFUL) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Program the CONFIRMED record for the running slot if it is absent.
 * @details See @ref flash_confirm.h for the public contract.  The
 *          implementation runs these steps in order:
 *
 *          -# Take the address from @ref APP_CONFIRMED_ADDR (never from the
 *             caller) and invalidate the D-cache for the 32-byte record.
 *             The flash region has no MPU region of its own, so it keeps the
 *             default cacheable attributes and may be cached; stale lines
 *             would otherwise make the reads below report old contents.
 *          -# Return @c true when @ref record_valid already accepts the
 *             record.  This is the idempotent path: no unlock, no erase, no
 *             program, so calling the helper on every boot of a confirmed
 *             slot costs one cache invalidation and the record reads only.
 *          -# Return @c false when the record is not valid but also not
 *             entirely `0xFF`; that is a torn record.  It is neither used nor
 *             rewritten, because only BOOT may erase the metadata sector.
 *          -# Build the image in an @ref CONFIRMED_WORDS word stack buffer:
 *             all words zero, word 0 the magic, word 7 the CRC-32/ISO-HDLC
 *             over words 0..6.  Nothing is written to flash before the whole
 *             image, including its checksum, exists.
 *          -# `HAL_FLASH_Unlock()`, then clear `EOP` and every error flag so a
 *             failure left over from an earlier boot or from BOOT itself
 *             cannot abort the first program.
 *          -# Program the words one at a time with
 *             `FLASH_TYPEPROGRAM_WORD`, ascending address, at most
 *             @ref CONFIRMED_WORDS programs.  After every program the record's
 *             cache line is invalidated again and the word is read back; the
 *             first HAL error or read-back mismatch sets @c ok to false and
 *             leaves the loop, so a failing part is never retried word by
 *             word.
 *          -# `HAL_FLASH_Lock()` unconditionally after the loop, then one more
 *             invalidation, so the controller is never left unlocked and the
 *             final check reads flash rather than the cache.
 *          -# Return `ok && record_valid(record)`.  The record is verified a
 *             second time from flash, so a program or read-back step that
 *             passed cannot by itself turn into a false "confirmed"; the
 *             caller only ever sees the contract's magic/CRC verdict.
 *
 * @return true when a valid CONFIRMED record is present after the call.
 * @retval true  The record was already valid, or it was absent and has now
 *               been programmed and verified.
 * @retval false Flash could not be unlocked, a program or read-back step
 *               failed, or a non-erased record without a valid magic/CRC
 *               occupies @ref APP_CONFIRMED_ADDR.
 *
 * @pre   Called from task context (only the Health task does so), after
 *        start-up and with the BOOT bank swap applied, so that the running
 *        slot's metadata sector is visible at `0x08000000`.
 * @pre   @ref APP_CONFIRMED_ADDR is 32-byte aligned, as
 *        `SCB_InvalidateDCache_by_Addr()` requires; the constant satisfies
 *        this because the CONFIRMED offset `0x140` is a multiple of 32.
 * @pre   Nothing else accesses flash, the metadata sector or
 *        @ref APP_CONFIRMED_ADDR concurrently.  Interrupts may stay enabled.
 * @post  On @c true the CONFIRMED record verifies in flash and BOOT will
 *        treat the slot as permanent.
 * @post  On @c false flash is unchanged unless the programming loop started;
 *        an interrupted loop leaves a torn record that this module will never
 *        overwrite again.
 * @post  No RAM state is changed.  In particular the cached hand-over
 *        snapshot is not updated: the caller must update the trial state
 *        itself, as @ref health_task does on success.
 *
 * @note Blocking, but short: at most @ref CONFIRMED_WORDS word programs plus
 *       their HAL waits, with no allocation, no lock and no RTOS call.  The
 *       Health task calls it once, from its 20 ms supervision loop, and only
 *       while the slot is still on trial.
 * @note Idempotent by construction: the validity check sits in front of the
 *       unlock, so a confirmed slot never reaches the flash controller.
 * @note The cache maintenance calls are not followed by an explicit @c __DSB()
 *       barrier in this implementation; the flash reads after each invalidate
 *       rely on program order plus the HAL's own flash-busy wait.
 * @warning Not reentrant and not ISR-safe.  Nothing serialises the erase check
 *          against the programming loop, so two concurrent callers could both
 *          observe an erased record and interleave writes into it.
 * @warning Do not add an erase to "repair" a torn record.  The erase would
 *          have to cover the whole 16 KB metadata sector and would destroy the
 *          BOOT-owned header, VERIFIED, ACTIVATED and REJECTED records; only
 *          BOOT may decide to erase it.
 * @see record_valid
 * @see record_erased
 * @see app_crc32
 * @see APP_CONFIRMED_ADDR
 * @see APP_META_CONFIRMED_MAGIC
 * @see APP_META_RECORD_SIZE
 */
bool app_flash_confirm_running_slot(void)
{
    /* DSAFE-002: the helper has no caller-supplied address.  The address is
     * the CONFIRMED record of the running slot under the BOOT bank swap, so
     * the write can neither be redirected nor aimed at another slot. */
    const uint32_t address = APP_CONFIRMED_ADDR;
    volatile uint32_t *record = (volatile uint32_t *)address;

    /* The metadata sector may be cached by the D-cache, because flash keeps
     * the default cacheable attributes (the MPU only remaps the SRAM2 DMA
     * window).  Invalidate the 32-byte record first, so the validity and
     * erased checks below see what is actually in flash and not a stale
     * line. */
    SCB_InvalidateDCache_by_Addr((uint32_t *)address, APP_META_RECORD_SIZE);

    /* Idempotent path: a record that already verifies is left untouched and
     * flash is never unlocked. */
    if (record_valid(record)) {
        return true;
    }
    /* Torn path: the record is no longer erased but does not verify either,
     * so it is the remains of an interrupted write.  Report failure and leave
     * it for BOOT instead of overwriting it. */
    if (!record_erased(record)) {
        return false;
    }

    /* Assemble the whole record, checksum included, in RAM before touching
     * flash: word 0 magic, words 1..6 zero, last word the CRC over those 28
     * bytes. */
    uint32_t words[CONFIRMED_WORDS];
    for (uint32_t i = 0U; i < CONFIRMED_WORDS; i++) {
        words[i] = 0U;
    }
    words[0] = APP_META_CONFIRMED_MAGIC;
    words[CONFIRMED_WORDS - 1U] =
        app_crc32((const void *)words, APP_META_RECORD_SIZE - 4U);

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    /* Drop any status left by an earlier operation, so it cannot abort the
     * first program of this call. */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_ALL_ERRORS);

    bool ok = true;
    for (uint32_t i = 0U; i < CONFIRMED_WORDS; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + (i * 4U),
                              words[i]) != HAL_OK) {
            ok = false;
            break;
        }
        /* The program went through the flash controller, not the D-cache, so
         * invalidate again before reading the word back; otherwise this check
         * could compare the RAM copy against a cached copy of erased flash. */
        SCB_InvalidateDCache_by_Addr((uint32_t *)address,
                                     APP_META_RECORD_SIZE);
        if (*(volatile uint32_t *)(address + (i * 4U)) != words[i]) {
            ok = false;
            break;
        }
    }

    /* Locked after the loop on both the success and the failure path, so a
     * failed write never leaves flash unlocked for the rest of the firmware. */
    HAL_FLASH_Lock();
    /* Make the final verification read from flash rather than from a line the
     * loop may have refilled. */
    SCB_InvalidateDCache_by_Addr((uint32_t *)address, APP_META_RECORD_SIZE);
    return ok && record_valid(record);
}
