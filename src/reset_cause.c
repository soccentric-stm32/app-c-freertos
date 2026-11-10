/**
 * @file    reset_cause.c
 * @brief   RCC reset flags captured at start-up (implementation).
 * @details Implements the one-shot reset-cause snapshot declared in
 *          @ref reset_cause.h.  @ref app_reset_cause_capture reads @c RCC->CSR
 *          once, masks the seven reset-cause bits into
 *          @ref app_reset_cause_flags, decodes the highest-priority cause into
 *          @ref app_reset_cause_text and then clears the hardware flags through
 *          __HAL_RCC_CLEAR_RESET_FLAGS().  main() calls it immediately after
 *          @c app_fault_boot_start() and before @c HAL_Init(), so the evidence
 *          survives until it has been copied into RAM (DGEN-005).
 *
 *          The retained mask is forwarded unchanged to the host as
 *          @c Status.reset_cause by @ref comms_task, while the decoded text is
 *          only used in the start-up log line.  The implementation contains no
 *          macros or configuration knobs; all state is the two globals below.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DGEN-005, LED-005
 * @ingroup reset_cause
 */

#include "stm32f7xx_hal.h"
#include "reset_cause.h"

/**
 * @brief Reset flags read from @c RCC_CSR at start-up, kept for Status.
 * @details Set exactly once by @ref app_reset_cause_capture to the masked
 *          @c RCC->CSR value: @c RCC_CSR_IWDGRSTF, @c RCC_CSR_WWDGRSTF,
 *          @c RCC_CSR_LPWRRSTF, @c RCC_CSR_SFTRSTF, @c RCC_CSR_PORRSTF,
 *          @c RCC_CSR_BORRSTF and @c RCC_CSR_PINRSTF.  Non-reset bits such as
 *          the LSI enable and ready flags are masked out.  The value is
 *          forwarded unchanged as @c Status.reset_cause by @ref comms_task, so
 *          the host applies its own priority and text mapping.
 * @note Zero until @ref app_reset_cause_capture runs.  A power-on normally
 *       sets POR, BOR and PIN together, so the raw mask alone is ambiguous;
 *       @ref app_reset_cause_capture resolves it with a fixed priority, and
 *       @ref app_reset_was_error tests only the two watchdog bits.
 * @warning Treat the variable as read-only after the capture.  Writing it
 *          would desynchronise @c Status.reset_cause from
 *          @ref app_reset_cause_text.
 * @see app_reset_cause_capture
 * @see app_reset_was_error
 */
uint32_t app_reset_cause_flags;

/**
 * @brief NUL-terminated, human-readable name of the decoded reset cause.
 * @details Always points to a string literal, so it is never NULL and must not
 *          be freed or modified.  @ref app_reset_cause_capture assigns one of:
 *          "independent watchdog", "window watchdog", "low power", "software",
 *          "power-on", "brown-out", "reset pin" or "unknown".  main() prints it
 *          in the start-up log line; it is deliberately not sent in Status,
 *          where the host derives its own text from
 *          @ref app_reset_cause_flags.
 * @note Initialised to "unknown" at definition and left unchanged until
 *       @ref app_reset_cause_capture stores the decoded value.
 * @see app_reset_cause_capture
 */
const char *app_reset_cause_text = "unknown";

/**
 * @brief Snapshot @c RCC_CSR, decode the reset cause and clear the flags.
 * @details Reads @c RCC->CSR once and stores the seven reset-cause bits in
 *          @ref app_reset_cause_flags.  It then decodes the most specific cause
 *          into @ref app_reset_cause_text using this fixed priority:
 *          -# @c RCC_CSR_IWDGRSTF -> "independent watchdog"
 *          -# @c RCC_CSR_WWDGRSTF -> "window watchdog"
 *          -# @c RCC_CSR_LPWRRSTF -> "low power"
 *          -# @c RCC_CSR_SFTRSTF  -> "software"
 *          -# @c RCC_CSR_PORRSTF  -> "power-on"
 *          -# @c RCC_CSR_BORRSTF  -> "brown-out"
 *          -# @c RCC_CSR_PINRSTF  -> "reset pin"
 *          -# no bit set (or an all-zero mask) -> "unknown"
 *
 *          Watchdog, low-power and software resets are tested first because
 *          they are deliberate or diagnostic causes rather than power-up
 *          artefacts.  A power-on sets POR, BOR and PIN together, so POR is
 *          tested before BOR and BOR before PIN; otherwise a normal power-on
 *          would be reported as a brown-out (RM0410 6.3.17).  The function
 *          finally sets @c RCC_CSR_RMVF through __HAL_RCC_CLEAR_RESET_FLAGS().
 *
 *          Takes no parameters and returns no value.  It holds no locks,
 *          allocates nothing and touches only @c RCC->CSR and the two globals,
 *          so it is safe before the HAL, scheduler and IWDG are initialised.
 *
 * @pre Called once from main() on the start-up thread, after
 *      @c app_fault_boot_start() and before @c HAL_Init(), the log, the
 *      scheduler and the IWDG are started.
 * @post @ref app_reset_cause_flags and @ref app_reset_cause_text describe the
 *       reset that just happened and the hardware reset flags are cleared, so
 *       the next reboot is unambiguous.
 *
 * @note DGEN-005 requires the flags to remain available for Status; clearing
 *       the hardware bits here does not lose information because the copy is
 *       retained in @ref app_reset_cause_flags.
 * @warning Not idempotent and not reentrant.  A second call reads an all-zero
 *          @c RCC->CSR, overwrites the retained mask with 0 and leaves the text
 *          at "unknown".  Do not call it from a task or an ISR.
 * @see app_reset_was_error
 * @see app_reset_cause_flags
 * @see app_reset_cause_text
 */
void app_reset_cause_capture(void)
{
    app_reset_cause_flags = RCC->CSR & (RCC_CSR_LPWRRSTF | RCC_CSR_WWDGRSTF | RCC_CSR_IWDGRSTF | RCC_CSR_SFTRSTF | RCC_CSR_PORRSTF | RCC_CSR_PINRSTF | RCC_CSR_BORRSTF);

    /* Most specific cause first.  A power-on sets POR, BOR and PIN, so
     * POR must be tested before BOR, and BOR before PIN, or a normal
     * power-on is reported as a brown-out (RM0410 6.3.17). */
    if((app_reset_cause_flags & RCC_CSR_IWDGRSTF) != 0U)
    {
        app_reset_cause_text = "independent watchdog";
    }
    else if((app_reset_cause_flags & RCC_CSR_WWDGRSTF) != 0U)
    {
        app_reset_cause_text = "window watchdog";
    }
    else if((app_reset_cause_flags & RCC_CSR_LPWRRSTF) != 0U)
    {
        app_reset_cause_text = "low power";
    }
    else if((app_reset_cause_flags & RCC_CSR_SFTRSTF) != 0U)
    {
        app_reset_cause_text = "software";
    }
    else if((app_reset_cause_flags & RCC_CSR_PORRSTF) != 0U)
    {
        app_reset_cause_text = "power-on";
    }
    else if((app_reset_cause_flags & RCC_CSR_BORRSTF) != 0U)
    {
        app_reset_cause_text = "brown-out";
    }
    else if((app_reset_cause_flags & RCC_CSR_PINRSTF) != 0U)
    {
        app_reset_cause_text = "reset pin";
    }
    else
    {
        app_reset_cause_text = "unknown";
    }

    __HAL_RCC_CLEAR_RESET_FLAGS();
}

/**
 * @brief Report whether the last reset came from a watchdog.
 * @details Tests the retained mask for @c RCC_CSR_IWDGRSTF or
 *          @c RCC_CSR_WWDGRSTF only; every other reset source returns false.
 *          main() uses the result to latch @ref APP_ERROR_WATCHDOG, which holds
 *          LD3 solid until ClearFault (LED-005).  A fault reset is a separate
 *          condition detected from the no-init fault record through
 *          app_fault_is_valid(), which latches @ref APP_ERROR_FAULT instead.
 *
 *          Takes no parameters.  It reads only @ref app_reset_cause_flags and
 *          does not touch the hardware, so it may be called repeatedly and from
 *          any context, including after the capture has cleared @c RCC->CSR.
 *
 * @return true when either watchdog flagged the reset, false otherwise.
 * @retval true  @c RCC_CSR_IWDGRSTF or @c RCC_CSR_WWDGRSTF is set in
 *               @ref app_reset_cause_flags.
 * @retval false Only power-on, brown-out, pin, software or low-power bits are
 *               set, or no capture has run yet.
 *
 * @pre @ref app_reset_cause_capture has run; before that the mask is zero and
 *      the function returns false.
 * @note Reads the retained mask only, so it is free of side effects and safe
 *       from task or ISR context.
 * @see app_reset_cause_capture
 * @see app_error_set
 */
bool app_reset_was_error(void) { return (app_reset_cause_flags & (RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) != 0U; }
