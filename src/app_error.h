/**
 * @file    app_error.h
 * @brief   Latched application error state reported on LD3.
 * @details A single volatile bit mask records why LD3 is latched.  Producers
 *          set only their own bit: @ref app_error_set with @ref APP_ERROR_FAULT
 *          or @ref APP_ERROR_WATCHDOG at start-up, and the health task with
 *          @ref APP_ERROR_HEALTH.  The LED task observes the mask and drives
 *          LD3, using a solid light for FAULT/WATCHDOG and a fast blink for
 *          HEALTH.  Bits remain set until explicitly cleared.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI
 * @srs     LED-005, DGEN-004, DGEN-005, HLT-003
 * @ingroup app_error
 */

#ifndef APP_ERROR_H
#define APP_ERROR_H

#include <stdint.h>

/**
 * @defgroup app_error Application error latch
 * @brief Latched error bits that drive LD3 and report health state.
 * @details The group exposes the shared mask plus the set and clear helpers.
 *          The mask is not task-private: every accessor manipulates
 *          @ref app_error_flags directly.
 * @{
 */

/**
 * @brief Saved fault or assert from a previous boot (LED-005).
 * @details Set by main() before the scheduler starts when
 *          app_fault_is_valid() reports a recorded fault.  LD3 is driven
 *          solid while this bit is set.
 */
#define APP_ERROR_FAULT      (1UL << 0U)

/**
 * @brief Last reset was caused by the IWDG or WWDG (LED-005).
 * @details Set by main() before the scheduler starts when
 *          app_reset_was_error() reports a watchdog reset.
 */
#define APP_ERROR_WATCHDOG   (1UL << 1U)

/**
 * @brief Pending-slot health check failed (HLT-003).
 * @details Set by the health task when a check fails.  LD3 blinks fast while
 *          this bit is set, and the bit survives ClearFault so a failed
 *          pending-slot health check is not hidden (LED-005).
 */
#define APP_ERROR_HEALTH     (1UL << 2U)

/**
 * @brief Global latch holding a bit mask of APP_ERROR_* values.
 * @details Only the module that owns a bit sets it; each macro documents its
 *          producer.  The LED task copies the mask into a local accumulator
 *          and ORs this variable into it on every iteration, so new bits
 *          become visible without blocking.
 * @note The accessors mask interrupts around the read-modify-write, so this
 *       variable is safe to update from both task and ISR context.
 * @warning Do not assign to this variable directly.  Use app_error_set(),
 *          app_error_clear(), or app_error_clear_fault() so updates stay
 *          atomic and do not race with the LED task.
 */
extern volatile uint32_t app_error_flags;

/**
 * @brief Set one or more latched error bits.
 * @details Atomically ORs @p flags into @ref app_error_flags.  Interrupts are
 *          disabled for the read-modify-write and the previous PRIMASK state
 *          is restored, so the call is safe from tasks and ISRs.
 *
 * @param[in] flags Bit mask of APP_ERROR_* values to latch.
 *
 * @note The caller should set only the bits it owns.
 * @post Every bit present in @p flags remains set until explicitly cleared.
 * @see app_error_clear_fault
 */
void app_error_set(uint32_t flags);

/**
 * @brief Clear the whole error latch.
 * @details Atomically writes zero to @ref app_error_flags while interrupts are
 *          disabled.  Used at start-up and by tests.
 *
 * @post @ref app_error_flags is zero.
 * @note This also clears the HEALTH bit; ClearFault must use
 *       app_error_clear_fault() to preserve HEALTH (LED-005).
 * @see app_error_clear_fault
 */
void app_error_clear(void);

/**
 * @brief Clear FAULT and WATCHDOG while preserving HEALTH (LED-005).
 * @details Atomically applies @c app_error_flags &= APP_ERROR_HEALTH.  The
 *          HWLINK ClearFault handler calls this so ClearFault cannot hide a
 *          pending-slot health failure; health_check_fail() is the only
 *          producer of HEALTH and runs once for the pending slot.
 *
 * @post Only @ref APP_ERROR_HEALTH can remain set.
 * @warning The mask keeps HEALTH and drops every other bit, including any
 *          error bit added in the future.  New bits must be added to the mask,
 *          or they will be cleared by ClearFault.
 * @note For the current three-bit set this is equivalent to clearing FAULT
 *       and WATCHDOG, which is what the SRS requires.
 * @see app_error_clear
 * @see app_error_set
 */
void app_error_clear_fault(void);

/** @} */

#endif /* APP_ERROR_H */
