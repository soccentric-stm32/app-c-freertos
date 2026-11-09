/**
 * @file    handover.c
 * @brief   BOOT hand-over record decoding and the "stay in BOOT" request.
 * @details Implements the DEMO side of the BOOT/DEMO hand-over contract
 *          declared in @ref handover.h and defined in @ref boot_contract.h.
 *          BOOT leaves one packed word in @c RTC_BKP2R before it jumps to the
 *          application; @ref app_handover_read decodes that word once into
 *          @ref g_handover so the rest of the application can report the
 *          running slot and trial state (HND-003).  In the other direction,
 *          @ref app_handover_request_bootloader writes the "stay in BOOT"
 *          magic to @c RTC_BKP1R so the next reset is served by BOOT again
 *          (DBT-005, ENT-002).
 *
 *          The record is a snapshot of BOOT's decision, not live flash state.
 *          An absent or malformed record - including an erased (zero) word -
 *          is reported as slot @c UNKNOWN with no trial information, which
 *          tells the application that it was not started by BOOT (DBT-004).
 *
 *          VBAT is tied to VDD on the NUCLEO-F767ZI, so the backup registers
 *          survive a system reset (BOOT hand-over) but not a power cycle.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DBT-004, DBT-005, HND-003, ENT-002
 * @ingroup handover
 * @see     handover.h, boot_contract.h
 */

#include "stm32f7xx_hal.h"
#include "boot_contract.h"
#include "handover.h"

/**
 * @brief Decoded hand-over record shared by every task after start-up.
 * @details Definition of the @ref handover.h declaration.  It is populated
 *          once by @ref app_handover_read before the scheduler starts; after
 *          that @ref health_task is the only writer, raising @c trial from 2
 *          (pending) to 1 (confirmed) once the CONFIRMED record is in flash.
 *          @ref comms_task reads @c slot, @c trial and @c trial_count when it
 *          fills @c Status, and @c main() logs the decoded fields at start-up.
 *
 * @note The fields keep their BOOT wire values, so they can be cast directly
 *       to the HWLINK @c Slot and @c TrialState enumerators.
 * @warning No lock protects the record.  All users except the single Health
 *          update treat it as read-only, and that update is one aligned byte
 *          store, so a concurrent reader sees either the old or the new value.
 * @see app_handover_t, app_handover_read
 */
app_handover_t g_handover;

/**
 * @brief Decode the hand-over word from @c RTC_BKP2R into @ref g_handover.
 * @details The word is always copied to @c g_handover.raw for diagnostics,
 *          then validated and decoded in this order:
 *
 *          -# Presence: the low 24 bits must equal @c APP_HANDOVER_MAGIC when
 *             masked with @c APP_HANDOVER_MAGIC_MASK.  A mismatch - including
 *             an erased register that reads as zero after a power cycle -
 *             clears @c present and leaves @c slot as @c APP_SLOT_UNKNOWN with
 *             @c trial and @c trial_count zero (DBT-004).
 *          -# Slot: bits 24..25 are extracted with
 *             @c APP_HANDOVER_SLOT_SHIFT and @c APP_HANDOVER_SLOT_MASK.  Only
 *             @c APP_SLOT_A (1) and @c APP_SLOT_B (2) are accepted; the
 *             reserved values 0 and 3 are treated as a malformed record and
 *             reported exactly like an absent one.
 *          -# Trial state: bit 26 (@c APP_HANDOVER_PENDING_BIT) selects state
 *             2 ("on trial") when set and state 1 ("confirmed") when clear,
 *             matching @c hwlink_TrialState.
 *          -# Trial counter: bits 27..28 are extracted with
 *             @c APP_HANDOVER_TRIAL_SHIFT and @c APP_HANDOVER_TRIAL_MASK into
 *             @c trial_count (0..3).  BOOT increments it each time it starts
 *             a pending slot, so Health uses it to recognise a failed trial.
 *
 *          The function deliberately touches only the register read: it
 *          enables no clock, does not open the backup domain and writes no
 *          backup register, so it cannot disturb BOOT's trial counter or the
 *          RTC.
 *
 * @pre  Called once from @c main() after @c app_clock_init() and before
 *       @ref app_log_init() and @c vTaskStartScheduler().  The RTC APB clock is
 *       assumed to be left running by BOOT.
 * @post @ref g_handover reflects the register; this module never rewrites it,
 *       although @ref health_task may still update @c trial.
 *
 * @note The decoded record is the BOOT decision only.  Confirming the slot in
 *       flash is done separately by @c app_flash_confirm_running_slot().
 * @warning Not thread-safe and not re-entrant: it overwrites @ref g_handover
 *          unconditionally, so call it only from the single start-up thread.
 * @see app_handover_request_bootloader, app_handover_t, boot_contract.h
 */
void app_handover_read(void)
{
    /* Reading backup registers does not need DBP; only the BKP1R write
     * below does.  Keeping DBP enabled for the whole run would let a stray
     * write corrupt BOOT's trial counter or the RTC. */
    uint32_t raw = RTC->BKP2R;

    g_handover.raw = raw;
    g_handover.present = (raw & APP_HANDOVER_MAGIC_MASK) ==
                         APP_HANDOVER_MAGIC;
    if (!g_handover.present) {
        g_handover.slot = APP_SLOT_UNKNOWN;
        g_handover.trial = 0U;
        g_handover.trial_count = 0U;
        return;
    }

    g_handover.slot = (uint8_t)((raw >> APP_HANDOVER_SLOT_SHIFT) &
                                APP_HANDOVER_SLOT_MASK);
    if (g_handover.slot != APP_SLOT_A && g_handover.slot != APP_SLOT_B) {
        g_handover.slot = APP_SLOT_UNKNOWN;
        g_handover.present = false;
        g_handover.trial = 0U;
        g_handover.trial_count = 0U;
        return;
    }

    g_handover.trial = ((raw >> APP_HANDOVER_PENDING_BIT) & 1U) != 0U
                       ? 2U : 1U;
    g_handover.trial_count = (uint8_t)((raw >> APP_HANDOVER_TRIAL_SHIFT) &
                                       APP_HANDOVER_TRIAL_MASK);
}

/**
 * @brief Ask BOOT to serve the next reset instead of the application (DBT-005).
 * @details Enables the PWR and RTC clocks, opens the backup domain with
 *          @c HAL_PWR_EnableBkUpAccess() (which sets the DBP bit) and writes
 *          @c APP_STAY_IN_BOOT_MAGIC ("STAY") to @c RTC_BKP1R.  The trailing
 *          @c __DSB() orders the store before any following reset, so BOOT
 *          still observes the request when the caller resets immediately
 *          afterwards.
 *
 *          This function only records the request; it does not reset and does
 *          not flush the UART.  The caller (normally @ref comms_task after the
 *          @c EnterBootloader @c Ack has been sent, COM-006) owns both.
 *
 * @pre  Thread context, after the application has decided to hand control
 *       back to BOOT.
 * @post @c RTC_BKP1R holds @c APP_STAY_IN_BOOT_MAGIC.  The PWR and RTC clocks
 *       and backup-domain access remain enabled for the rest of this boot.
 *
 * @note The write needs the backup domain protection cleared, which is why the
 *       clocks and DBP are enabled here rather than at start-up.
 * @warning It overwrites @c RTC_BKP1R unconditionally and leaves the backup
 *          domain writable with the PWR/RTC clocks on.  That is only safe
 *          because the caller is expected to reset immediately; do not call it
 *          and then keep running.
 * @see app_handover_read, APP_STAY_IN_BOOT_MAGIC, boot_contract.h
 */
void app_handover_request_bootloader(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_RTC_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    RTC->BKP1R = APP_STAY_IN_BOOT_MAGIC;
    __DSB();
}
