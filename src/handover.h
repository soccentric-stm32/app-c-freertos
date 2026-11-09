/**
 * @file    handover.h
 * @brief   BOOT hand-over record kept in the RTC backup domain.
 * @details BOOT and DEMO exchange one 32-bit word through @c RTC_BKP2R
 *          (HND-003).  The low 24 bits hold the magic @c 0x444E48 (the bytes
 *          "HND") and the upper byte carries the running slot and the trial
 *          state:
 *
 *          - bits  0..23  magic @c 0x444E48 ("HND")
 *          - bits 24..25  slot: 0 unknown, 1 slot A, 2 slot B
 *          - bit  26      trial pending flag
 *          - bits 27..28  trial counter (0..3)
 *
 *          @ref app_handover_read decodes that word once at start-up into
 *          @ref g_handover.  @ref app_handover_request_bootloader writes the
 *          "stay in BOOT" magic to @c RTC_BKP1R so that the next reset is
 *          served by BOOT again (DBT-005, ENT-002).  A missing record means
 *          DEMO was not started by BOOT: it then reports slot @c UNKNOWN and
 *          skips confirmation (DBT-004).
 *
 *          On the NUCLEO-F767ZI VBAT is tied to VDD, so the backup registers
 *          survive a system reset - exactly the BOOT hand-over path - but not
 *          a power cycle.  After a power cycle the record reads as erased
 *          (zero) and is treated as missing.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DBT-004, DBT-005, HND-003, ENT-002
 * @ingroup handover
 */

#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup handover BOOT hand-over record
 * @brief Decoded @c RTC_BKP2R state and the "stay in BOOT" request.
 * @details The group owns the decoded record @ref g_handover and the two
 *          entry points that read it and that ask BOOT to keep control on the
 *          next reset.  The bit packing itself lives in @ref boot_contract.h
 *          (@c APP_HANDOVER_* and @c APP_STAY_IN_BOOT_MAGIC) and is shared
 *          with BOOT; this header only presents the decoded form consumed by
 *          @ref comms_task (Status), @ref health_task (trial handling) and the
 *          start-up log line in @c main().
 * @{
 */

/**
 * @brief Decoded hand-over record shared by start-up, Comms and Health.
 * @details Decoded once by @ref app_handover_read from the word BOOT left in
 *          @c RTC_BKP2R.  The numeric values of @c slot and @c trial are
 *          identical to the HWLINK @c Slot and @c TrialState enumerators, so
 *          @ref comms_task casts them directly when it fills @c Status.
 * @note This is a snapshot of the BOOT decision, not live flash state.  A
 *       pending slot becomes confirmed in flash through
 *       @ref app_flash_confirm_running_slot, but only @ref health_task patches
 *       @c trial locally; @c present and @c slot never change.
 * @note BOOT increments @c trial_count each time it starts a pending slot, so
 *       @ref health_task can treat a fault left by the previous boot with
 *       @c trial_count above 1 as a failed trial.
 * @see app_handover_read
 */
typedef struct {
    bool present;         /**< True when the magic matched and the slot is A/B. */
    uint8_t slot;         /**< @c APP_SLOT_UNKNOWN, @c APP_SLOT_A or @c APP_SLOT_B. */
    uint8_t trial;        /**< 0 unknown, 1 confirmed, 2 pending (hwlink_TrialState). */
    uint8_t trial_count;  /**< BOOT trial counter, 0..3; 0 when record absent. */
    uint32_t raw;         /**< Raw @c RTC_BKP2R word, magic and unused bits included. */
} app_handover_t;

/**
 * @brief Decoded hand-over record, shared by every task after start-up.
 * @details Defined in @ref handover.c and written once by
 *          @ref app_handover_read before the scheduler starts.  Afterwards
 *          only @ref health_task changes it, raising @c trial from 2 (pending)
 *          to 1 (confirmed) once the CONFIRMED record is in flash;
 *          @ref comms_task reads all fields when it fills @c Status.
 * @note No lock is used.  Every other user treats the record as read-only and
 *       the single Health update is one aligned byte store, so a reader sees
 *       either the old or the new value.
 */
extern app_handover_t g_handover;

/**
 * @brief Decode the hand-over record from @c RTC_BKP2R into @ref g_handover.
 * @details Reads the backup register without opening the backup domain, so it
 *          cannot disturb BOOT's trial counter or the RTC.  The word is
 *          accepted only if its low 24 bits equal @c APP_HANDOVER_MAGIC and the
 *          slot field yields A or B; anything else - including an erased
 *          (zero) register after a power cycle - is reported as slot
 *          @c UNKNOWN, trial 0 and no record (DBT-004).  For a valid record the
 *          pending bit selects trial 2, a clear bit means the slot was already
 *          confirmed and selects trial 1, and bits 27..28 become
 *          @c trial_count.
 *
 * @pre  Called once from @c main() after @c app_clock_init() and before
 *       @ref app_log_init() and @c vTaskStartScheduler().
 * @post @ref g_handover reflects the register; this module never rewrites it
 *       again, although @ref health_task may still update @c trial.
 *
 * @note The raw word is kept in @c raw for diagnostics because the call runs
 *       before the first log line and cannot report a rejected record.
 * @note Unlike @ref app_handover_request_bootloader this call enables no
 *       clock and no backup-domain access: it relies on the PWR/RTC clocks
 *       that BOOT leaves running before it jumps to the application.
 * @warning Not thread-safe: it overwrites @ref g_handover unconditionally, so
 *          call it only from the single start-up thread.
 * @see app_handover_request_bootloader, boot_contract.h
 */
void app_handover_read(void);

/**
 * @brief Ask BOOT to stay in control after the next reset (DBT-005).
 * @details Enables the PWR and RTC clocks, opens the backup domain with
 *          @c HAL_PWR_EnableBkUpAccess and writes @c APP_STAY_IN_BOOT_MAGIC
 *          ("STAY") to @c RTC_BKP1R.  The trailing @c __DSB() orders the store
 *          before the reset, so BOOT still sees the request when the caller
 *          resets immediately afterwards.
 *
 * @pre  Thread context, after the application has decided to hand control
 *       back - normally @ref comms_task once the @c EnterBootloader @c Ack has
 *       been sent (COM-006).
 * @post @c RTC_BKP1R holds the magic, and both clocks plus backup-domain
 *       access stay enabled for the remainder of this boot.
 *
 * @note Only the request is recorded; the reset itself belongs to the caller,
 *       which also owns flushing the UART before it.
 * @warning It overwrites @c RTC_BKP1R unconditionally and leaves the backup
 *          domain writable with the PWR/RTC clocks on.  That is only safe
 *          because the caller resets immediately, so do not call it and keep
 *          running.
 * @see app_handover_read, APP_STAY_IN_BOOT_MAGIC
 */
void app_handover_request_bootloader(void);

/** @} */

#endif /* HANDOVER_H */
