/**
 * @file    reset_cause.h
 * @brief   RCC reset flags captured at start-up and decoded into a cause.
 * @details The application needs to know why it rebooted before anything
 *          clears or overwrites the evidence, so main() calls
 *          @ref app_reset_cause_capture once, before HAL_Init() and before the
 *          scheduler or the watchdog starts.  That single call snapshots
 *          @c RCC_CSR into @ref app_reset_cause_flags, decodes the most
 *          specific cause into @ref app_reset_cause_text and then clears the
 *          hardware flags (DGEN-005).
 *
 *          The raw mask is retained so @ref comms_task can forward it in the
 *          HWLINK @c Status message (@c reset_cause), while the decoded text is
 *          only printed in the start-up log line.  The decoder covers these
 *          sources, in priority order:
 *          -# independent watchdog reset (@c RCC_CSR_IWDGRSTF)
 *          -# window watchdog reset (@c RCC_CSR_WWDGRSTF)
 *          -# low-power reset (@c RCC_CSR_LPWRRSTF)
 *          -# software reset (@c RCC_CSR_SFTRSTF)
 *          -# power-on reset (@c RCC_CSR_PORRSTF)
 *          -# brown-out reset (@c RCC_CSR_BORRSTF)
 *          -# external reset pin (@c RCC_CSR_PINRSTF)
 *          -# none of the above, or an all-zero mask: "unknown"
 *
 *          The module holds no locks, allocates nothing and is expected to run
 *          exactly once on the single start-up thread.  It only touches
 *          @c RCC->CSR and two globals, so it works before the HAL is
 *          initialised (LED-005).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DGEN-005, LED-005
 * @ingroup reset_cause
 */

#ifndef RESET_CAUSE_H
#define RESET_CAUSE_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup reset_cause Reset cause snapshot
 * @brief One-shot RCC reset-cause capture, its decoded text and an error test.
 * @details The group exposes the retained flag mask, the decoded cause string
 *          and the predicate used by main() to latch LD3.  All three are
 *          produced by @ref app_reset_cause_capture and are read-only
 *          afterwards; the module has no configuration macros.
 * @{
 */

/**
 * @brief Reset flags read from @c RCC_CSR at start-up, kept for Status.
 * @details Bit mask of the reset-cause bits defined by CMSIS for the STM32F7:
 *          @c RCC_CSR_IWDGRSTF, @c RCC_CSR_WWDGRSTF, @c RCC_CSR_LPWRRSTF,
 *          @c RCC_CSR_SFTRSTF, @c RCC_CSR_PORRSTF, @c RCC_CSR_BORRSTF and
 *          @c RCC_CSR_PINRSTF.  All other bits of the register (for example the
 *          LSI enable and ready bits) are masked out.  The value is forwarded
 *          unchanged as @c Status.reset_cause by @ref comms_task, so the host
 *          sees exactly the hardware bits and applies its own priority.
 * @note Zero until @ref app_reset_cause_capture runs.  A power-on normally sets
 *       several bits at once (POR, BOR and PIN), so the mask alone is
 *       ambiguous; the text decoder applies the fixed priority documented in
 *       @ref reset_cause.h.
 * @warning Treat the variable as read-only after the capture.  Writing it
 *          would desynchronise Status from @ref app_reset_cause_text.
 */
extern uint32_t app_reset_cause_flags;

/**
 * @brief NUL-terminated, human-readable name of the decoded reset cause.
 * @details Always points to a string literal - "independent watchdog",
 *          "window watchdog", "low power", "software", "power-on", "brown-out",
 *          "reset pin" or "unknown" - so the pointer is never NULL and the
 *          string must not be freed or modified.  The start-up log line prints
 *          it; it is not sent in Status, where the host derives its own text
 *          from @ref app_reset_cause_flags.
 * @note Initialised to "unknown" at definition and left unchanged until
 *       @ref app_reset_cause_capture stores the decoded value.
 */
extern const char *app_reset_cause_text;

/**
 * @brief Snapshot @c RCC_CSR, decode the reset cause and clear the flags.
 * @details Reads @c RCC_CSR once, masks the seven reset-cause bits into
 *          @ref app_reset_cause_flags, decodes the highest-priority cause into
 *          @ref app_reset_cause_text and finally sets @c RCC_CSR_RMVF through
 *          __HAL_RCC_CLEAR_RESET_FLAGS().
 *
 *          The decoder tests sources from most specific to least specific.  A
 *          power-on sets POR, BOR and PIN together, so POR is tested before
 *          BOR and BOR before PIN; otherwise a normal power-on would be
 *          reported as a brown-out (RM0410 6.3.17).  Watchdog, low-power and
 *          software resets are tested first because they are deliberate or
 *          diagnostic causes rather than power-up artefacts.
 *
 * @pre Called once from main() on the start-up thread, before HAL_Init(), the
 *      log, the scheduler and the IWDG are started.
 * @post @ref app_reset_cause_flags and @ref app_reset_cause_text describe the
 *       reset that just happened and the hardware flags are cleared, so the
 *       next reboot is unambiguous.
 *
 * @note DGEN-005 requires the flags to remain available for Status; clearing
 *       the hardware bits here does not lose information because the copy is
 *       retained in RAM.
 * @warning Not idempotent and not reentrant.  A second call reads an all-zero
 *          @c RCC_CSR, overwrites the retained mask with 0 and leaves the text
 *          at "unknown".  Do not call it from a task or an ISR.
 * @see app_reset_was_error
 * @see app_reset_cause_flags
 */
void app_reset_cause_capture(void);

/**
 * @brief Report whether the last reset came from a watchdog.
 * @details Tests the captured mask for @c RCC_CSR_IWDGRSTF or
 *          @c RCC_CSR_WWDGRSTF only.  main() uses the result to latch
 *          @ref APP_ERROR_WATCHDOG, which holds LD3 solid until ClearFault
 *          (LED-005).  A fault reset is a separate condition: main() detects it
 *          from the no-init fault record through app_fault_is_valid() and
 *          latches @ref APP_ERROR_FAULT instead.
 *
 * @return true when either watchdog flagged the reset, false otherwise.
 * @retval true  @c RCC_CSR_IWDGRSTF or @c RCC_CSR_WWDGRSTF is set in
 *               @ref app_reset_cause_flags.
 * @retval false Only power-on, brown-out, pin, software or low-power bits are
 *               set, or no capture has run yet.
 *
 * @pre @ref app_reset_cause_capture has run; before that the mask is zero and
 *      the function returns false.
 * @note Reads the retained mask only, so it does not touch the hardware, may
 *       be called repeatedly and is safe from any context.
 * @see app_error_set
 */
bool app_reset_was_error(void);

/** @} */

#endif /* RESET_CAUSE_H */
