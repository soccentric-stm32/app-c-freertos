/**
 * @file    app_error.c
 * @brief   Latched application error state implementation.
 * @details Implements the single-word error latch declared in @ref app_error.h.
 *          Each module owns one or more bits of @ref app_error_flags.  main()
 *          latches @ref APP_ERROR_FAULT and @ref APP_ERROR_WATCHDOG before the
 *          scheduler starts, the health task latches @ref APP_ERROR_HEALTH, and
 *          the HWLINK ClearFault handler releases FAULT and WATCHDOG through
 *          @ref app_error_clear_fault.  The LED task reads the mask and drives
 *          LD3: solid for FAULT/WATCHDOG, fast blink for HEALTH.
 *
 *          Every mutator follows the same pattern: save PRIMASK, disable
 *          interrupts, perform the read-modify-write, then restore the saved
 *          PRIMASK.  That makes the updates atomic with respect to tasks and
 *          ISRs, keeps the functions safe in either context, and never leaves
 *          interrupts enabled when the caller had them masked.  The functions
 *          never block, never allocate and run in constant time, so they may be
 *          called from an ISR.  The file holds no state other than the mask
 *          itself; lifetime is the whole program run.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     LED-005, DGEN-004, DGEN-005, HLT-003
 * @ingroup app_error
 */

#include "stm32f7xx_hal.h"
#include "app_error.h"

/**
 * @brief Definition of the global latched error mask.
 * @details Holds a bitwise OR of the APP_ERROR_* bits documented in
 *          @ref app_error.h.  Written only by @ref app_error_set,
 *          @ref app_error_clear and @ref app_error_clear_fault; read without
 *          masking by the LED task, because a single aligned 32-bit load is
 *          atomic on Cortex-M7 and therefore never observes a torn value.
 *          @c volatile keeps the compiler from caching the value across the
 *          LED task loop and from removing stores that no other translation
 *          unit appears to read.
 * @note The object lives in .bss and is zeroed by the C start-up code, so the
 *       latch always starts empty after reset.  Persistent fault details live
 *       in @ref g_app_fault, not here.
 * @note Task and ISR safe only through the accessors; a direct assignment
 *       would race with a concurrent read-modify-write in another context.
 * @warning Do not write this variable directly.  Use @ref app_error_set,
 *          @ref app_error_clear or @ref app_error_clear_fault.
 * @see app_error_set
 * @see app_error_clear
 * @see app_error_clear_fault
 */
volatile uint32_t app_error_flags;

/**
 * @brief Set one or more bits in the latched error mask.
 * @details Saves PRIMASK, disables interrupts, ORs @p flags into
 *          @ref app_error_flags, then restores the saved PRIMASK.  Bits that
 *          are already set are unaffected, so the operation is idempotent and
 *          different producers do not overwrite each other.
 *
 *          Interrupt safety: the read-modify-write sequence is not atomic on
 *          its own, so it runs with interrupts masked.  Restoring the saved
 *          PRIMASK instead of unconditionally enabling interrupts means the
 *          function is callable both from tasks (interrupts re-enabled on
 *          return) and from ISRs or other critical sections (interrupts stay
 *          disabled on return).  The critical section is a few instructions
 *          long; interrupt latency is bounded and no interrupt is lost.
 *
 * @param[in] flags Bit mask of APP_ERROR_* bits to latch.  A value of 0 is
 *                  legal and performs an atomic no-op update.
 *
 * @pre  @p flags must only contain bits owned by the caller; see the macro
 *       documentation in @ref app_error.h for the producer of each bit.
 * @post Every bit present in @p flags is set in @ref app_error_flags, and all
 *       bits that were already set remain set.  PRIMASK is restored to its
 *       value on entry.
 *
 * @note main() calls this before vTaskStartScheduler() to latch the saved
 *       fault and the watchdog reset cause (LED-005); health_check_fail() calls
 *       it from the health task.
 * @note Not blocking and allocation-free, therefore ISR safe.
 * @see app_error_clear_fault
 * @see app_error_clear
 */
void app_error_set(uint32_t flags)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    app_error_flags |= flags;
    __set_PRIMASK(primask);
}

/**
 * @brief Clear the complete latched error mask.
 * @details Saves PRIMASK, disables interrupts, stores zero to
 *          @ref app_error_flags, then restores the saved PRIMASK.  The store is
 *          a plain 32-bit write because the whole word is replaced, so no
 *          read-modify-write hazard exists; interrupts are still masked to keep
 *          the clear indivisible with respect to concurrent setters.
 *
 *          Interrupt safety: same PRIMASK save/restore contract as
 *          @ref app_error_set - safe from tasks and ISRs, and interrupts are
 *          re-enabled on return only when the caller had them enabled.
 *
 * @pre  No assumption about the previous mask contents; any value is cleared.
 * @post @ref app_error_flags is zero and PRIMASK is restored to its entry
 *       value.
 *
 * @warning This also drops @ref APP_ERROR_HEALTH.  The HWLINK ClearFault
 *          handler must use @ref app_error_clear_fault so a failed pending-slot
 *          health check stays visible (LED-005).  Use this function only for a
 *          full reset of the latch.
 * @note The public header describes this function as used at start-up and by
 *       tests; the current firmware tree has no production caller, so the
 *       intended start-up reset is effectively the .bss zero initialisation.
 * @note Not blocking and allocation-free, therefore ISR safe.
 * @see app_error_clear_fault
 * @see app_error_set
 */
void app_error_clear(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    app_error_flags = 0U;
    __set_PRIMASK(primask);
}

/**
 * @brief Release FAULT and WATCHDOG while preserving HEALTH.
 * @details Saves PRIMASK, disables interrupts, applies
 *          @c app_error_flags &= APP_ERROR_HEALTH, then restores the saved
 *          PRIMASK.  After the call only @ref APP_ERROR_HEALTH can remain set.
 *          The comms task calls this from the HWLINK ClearFault handler,
 *          together with app_fault_clear() and led_post_clear_error(), so a
 *          pending-slot health failure cannot be hidden by a host ClearFault
 *          request (LED-005).
 *
 *          Interrupt safety: the AND is a read-modify-write, so it runs inside
 *          the PRIMASK-masked section.  The saved PRIMASK is restored instead
 *          of unconditionally enabling interrupts, which keeps the function
 *          usable from ISRs and from callers that already mask interrupts.
 *
 * @pre  None; the function tolerates any mask contents.
 * @post Only @ref APP_ERROR_HEALTH can be set in @ref app_error_flags;
 *       FAULT and WATCHDOG are always cleared.  PRIMASK is restored to its
 *       entry value.
 *
 * @warning The mask hard-codes @ref APP_ERROR_HEALTH.  Any error bit added in
 *          the future is silently dropped by ClearFault unless the mask is
 *          extended here, so new bits must be reviewed against this function.
 * @note The health task is the only producer of @ref APP_ERROR_HEALTH and sets
 *       it once for the pending slot, so the preserved bit cannot accumulate
 *       across checks.
 * @note Not blocking and allocation-free, therefore ISR safe.
 * @see app_error_clear
 * @see app_error_set
 */
void app_error_clear_fault(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    app_error_flags &= APP_ERROR_HEALTH;
    __set_PRIMASK(primask);
}
