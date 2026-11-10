/**
 * @file    flash_confirm.h
 * @brief   Program the single CONFIRMED metadata record of the running slot.
 * @details Public interface of @ref flash_confirm.c, the only module in DEMO
 *          that writes flash.  BOOT keeps one 32-byte Appendix B record per
 *          slot in the metadata sector and accepts a trial slot permanently
 *          only once that record is valid.  The Health task calls
 *          @ref app_flash_confirm_running_slot once, 5 s after start-up, when
 *          the slot is on trial and every health condition is met (`HLT-002`,
 *          `HLT-005`); the write is the application's proof that the image
 *          survived its trial boot.
 *
 *          **Record layout.**  The record is exactly
 *          @ref APP_META_RECORD_SIZE (32) bytes, that is eight 32-bit words:
 *          - word 0: @ref APP_META_CONFIRMED_MAGIC, `0x434F4E46` ("CONF")
 *          - words 1..6: `0x00000000`
 *          - word 7: CRC-32/ISO-HDLC over bytes 0..27 (words 0..6)
 *
 *          The checksum is produced with @ref app_crc32, the same table-free
 *          CRC-32/ISO-HDLC used for HWLINK frames, so a record verifies only
 *          when the magic matches @em and the CRC over the preceding 28 bytes
 *          matches the last word.  An all-`0xFF` record means "absent";
 *          anything else that fails the magic/CRC test is "torn".
 *
 *          **Fixed address.**  The target is @ref APP_CONFIRMED_ADDR
 *          (`0x08008140`), the CONFIRMED offset of the running slot's
 *          metadata sector.  BOOT maps the active bank to `0x08000000`, so the
 *          helper needs no slot or address argument and cannot be redirected
 *          at run time; this is the `DSAFE-002` restriction on the one write
 *          DEMO is allowed to make (`DBT-006`).
 *
 *          **No erase, ever.**  The metadata sector also holds BOOT's header,
 *          VERIFIED, ACTIVATED and REJECTED data, so the module never issues
 *          an erase.  It programs only into a record whose 32 bytes all read
 *          `0xFF`; a partially programmed record is left untouched so that
 *          BOOT, not the application, decides how to recover it.
 *
 *          **Idempotency.**  An already valid record is detected before any
 *          flash is unlocked, so repeated calls return success without
 *          writing.  Re-running the helper on every boot of a confirmed slot
 *          is therefore harmless.
 *
 *          The header declares one function and no types or configuration
 *          macros.  All addresses, sizes and magic words live in
 *          @ref boot_contract.h; the helper's record inspection stays private
 *          to @ref flash_confirm.c.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, dual-bank flash
 * @srs     HLT-002, HLT-005, DBT-006, DSAFE-002
 * @see     boot_contract.h
 * @see     crc32.h
 * @see     health_task.c
 * @ingroup flash_confirm
 */

#ifndef FLASH_CONFIRM_H
#define FLASH_CONFIRM_H

#include <stdbool.h>

/**
 * @defgroup flash_confirm CONFIRMED metadata writer
 * @brief Idempotent single-record flash write that confirms the running slot.
 * @details The group exposes one blocking, non-reentrant entry point that
 *          turns the running slot from "on trial" into "confirmed" by making
 *          the Appendix B CONFIRMED record valid in the metadata sector.  It
 *          is the only place where the application touches flash: it never
 *          erases, never writes any other record and never writes any other
 *          address (`DBT-006`).
 *
 *          The record is written as an eight-word image built entirely in RAM
 *          (32 bytes of stack) and then programmed word by word, each word
 *          read back and compared before the next one is written.  A valid
 *          record is recognised by magic plus CRC-32; a fully erased record is
 *          recognised by reading back as all `0xFF`; any other content is
 *          treated as a torn record from an interrupted earlier write and is
 *          neither used nor modified.
 * @see APP_CONFIRMED_ADDR
 * @see APP_META_CONFIRMED_MAGIC
 * @see APP_META_RECORD_SIZE
 * @see app_crc32
 * @{
 */

/**
 * @brief Program the CONFIRMED record for the running slot if it is absent.
 * @details Sequence performed by the implementation:
 *          -# Invalidate the D-cache for the 32-byte record, because the
 *             metadata sector may be cached, then read the record from flash.
 *          -# Return success immediately when magic and CRC are both valid;
 *             this is the idempotent no-write path.
 *          -# Return failure without writing when the record is not valid but
 *             also not entirely `0xFF`, that is when it is torn.
 *          -# Build the image: word 0 magic, words 1..6 zero, word 7 the
 *             CRC-32/ISO-HDLC of words 0..6, then unlock flash, clear
 *             `EOP`/all error flags and program word by word with
 *             `FLASH_TYPEPROGRAM_WORD`.  After each word the D-cache is
 *             invalidated and the word is read back; the first mismatch or
 *             HAL error stops the loop.  Flash is locked again on every path.
 *          -# Re-read the record and report success only when it now verifies,
 *             so a programming error can never be reported as confirmation.
 *
 *          The address is taken from @ref APP_CONFIRMED_ADDR internally, so
 *          there is no way for a caller to confirm a slot other than the one
 *          that is running (`DSAFE-002`).
 *
 * @return true when a valid CONFIRMED record is present after the call.
 * @retval true  The record was already valid, or it was absent, programmed
 *               and verified.
 * @retval false Flash could not be unlocked, a program or verify step failed,
 *               a HAL flash error was reported, or a non-erased record without
 *               a valid magic/CRC occupies the address.
 *
 * @pre   Called from task context after the health check has passed; the
 *        metadata sector of the running bank is reachable at `0x08000000`
 *        with the BOOT bank swap already applied.
 * @pre   Interrupts may be enabled, but nothing else may access flash, the
 *        metadata sector or @ref APP_CONFIRMED_ADDR concurrently.
 * @post  On @c true the CONFIRMED record is valid in flash; on @c false flash
 *        content is unchanged except after a partially failed write, which
 *        leaves a torn record.
 * @post  No RAM state is modified.  The cached hand-over snapshot is @em not
 *        updated; the caller must set the trial state itself, as
 *        @ref health_task does after a successful call.
 *
 * @note  The write is idempotent: calling it for an already confirmed slot
 *        performs no flash operation at all.
 * @note  The call blocks for the duration of up to eight word programs plus
 *        the HAL wait for each operation; it allocates nothing and holds no
 *        lock, and runs on the caller's stack with a 32-byte buffer.
 * @note  Only the Health task calls this helper, once per pending trial.  Any
 *        other caller must reproduce the health-check gate, or BOOT will see a
 *        slot confirmed by an image that never proved itself.
 * @warning Not reentrant and not ISR-safe.  There is no lock around the
 *          erase check and the programming loop, so two concurrent callers
 *          could both observe an erased record and interleave writes.
 * @warning A failed or interrupted programming loop leaves a torn record that
 *          this module will never overwrite again: all later calls return
 *          @c false and the slot cannot be confirmed until BOOT erases the
 *          metadata sector.  Do not add an erase here to "repair" it, because
 *          the erase would destroy the other BOOT-owned records.
 * @see APP_CONFIRMED_ADDR
 * @see APP_META_CONFIRMED_MAGIC
 * @see APP_META_RECORD_SIZE
 * @see app_crc32
 * @see health_task
 */
bool app_flash_confirm_running_slot(void);

/** @} */

#endif /* FLASH_CONFIRM_H */
