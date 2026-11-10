/**
 * @file    app_log.h
 * @brief   Line-oriented debug text output on USART6 (PG14, Arduino D1).
 * @details USART3 carries HWLINK only, so debug lines go to USART6, which
 *          runs 115200 8N1, TX only.  BOOT and every application use the
 *          same port and the same line format, so one terminal shows the
 *          whole boot chain:
 *
 *              [   5012] C-FRT I health: slot A confirmed
 *
 *          Each line is `<[tick]> <tag> <level> <message><CR><LF>`, where
 *          `tick` is @c HAL_GetTick() in milliseconds, right-aligned in 7
 *          columns, `tag` is the fixed literal @c "C-FRT" (see @c LOG_TAG in
 *          @c app_log.c) and `level` is the single character passed to
 *          @ref app_log.
 *
 *          @ref app_log never blocks: it formats into a stack buffer, copies
 *          the line into a 1 KB ring buffer and returns; the USART6 TXE
 *          interrupt drains the ring to the wire.  A line that does not fit
 *          in the ring is dropped whole and counted (DLOG-003).  The API is
 *          safe from tasks, from interrupts and before the scheduler starts.
 *
 *          The levels are labels only: this module has neither a
 *          compile-time nor a run-time threshold, so every @c LOG_* call
 *          formats, queues and transmits its line, and the terminal does the
 *          filtering.  See @ref LOG_E for the cost implication.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-06
 * @target  STM32F767ZIT6
 * @srs     DLOG-001..DLOG-004
 * @ingroup app_log
 */

#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

/**
 * @defgroup app_log Application debug log
 * @brief Non-blocking, interrupt-driven debug text on USART6.
 * @details Public API of the debug logger.  Output is shared with BOOT so a
 *          single terminal captures the complete boot chain (DLOG-001), the
 *          ring is drained by the USART6 TXE interrupt at the lowest NVIC
 *          priority without any FreeRTOS call (DLOG-002), a full ring drops
 *          the line and counts it (DLOG-003), and @ref app_log_flush exists
 *          so a deliberate reset does not truncate the last lines.
 *
 *          Concurrency model: @ref app_log may be called from any task or
 *          any interrupt handler, and before the scheduler starts, because
 *          it only masks interrupts (PRIMASK) for the bounded copy of one
 *          line into the ring.  @ref app_log_dropped is a plain 32-bit read.
 *          @ref app_log_flush busy-waits with interrupts masked and is
 *          therefore intended for start-up and fault paths, not for normal
 *          task operation.
 * @{
 */

/**
 * @brief Configure PG14 as USART6_TX and start the interrupt-driven output.
 * @details Enables the GPIOG and USART6 clocks, drives PG14 as alternate
 *          function 8 (push-pull, no pull, medium speed), programs the
 *          baud-rate divisor from PCLK2 for 115200 baud, sets TE and UE, and
 *          enables @c USART6_IRQn at the lowest NVIC priority (15, no
 *          sub-priority).  Until this returns, @ref app_log and
 *          @ref app_log_flush return without touching the hardware.
 *
 * @pre  Thread context, after @c HAL_Init() and after the clock tree that
 *       feeds PCLK2 is programmed; @c main() calls it once, immediately
 *       after @c app_handover_read() and before the first @c LOG_I.
 *
 * @post USART6 is enabled for transmission, the TXE interrupt is enabled in
 *       the NVIC (but not yet in CR1) and @ref app_log starts accepting
 *       lines.
 *
 * @note The pin, baud rate, tag and interrupt priority are fixed at compile
 *       time and must stay identical to BOOT (DLOG-001).
 * @note No FreeRTOS object is created or touched and nothing blocks, so the
 *       call is valid before @c vTaskStartScheduler().
 * @warning Call it once.  A second call re-initialises the GPIO and USART
 *          registers, stops the transmitter and clears @c TXEIE, so any
 *          line still in the ring stays there until the next @ref app_log.
 * @see app_log, app_log_flush
 */
void app_log_init(void);

/**
 * @brief Format one debug line and queue it for transmission on USART6.
 * @details Builds `[tick] C-FRT <level> <message><CR><LF>` in a 128-byte
 *          stack buffer and copies it into the transmit ring; the USART6 TXE
 *          interrupt sends it later.  The call never blocks, never allocates
 *          and never calls into FreeRTOS.
 *
 *          The formatter is the module's own (newlib-nano's @c snprintf
 *          would pull in malloc and the application has no C heap); it
 *          supports `%%`, `%c`, `%s` with an optional `.N` precision, `%d`,
 *          `%i`, `%u` and `%x` with an optional @c l modifier, and
 *          zero-padded widths such as `%08lx`.  Width and zero padding apply
 *          to the numeric conversions; the precision applies to `%s` only.
 *          Unsupported conversions are copied literally (`%p` prints `%p`)
 *          and stop formatting immediately, so they consume no argument and
 *          no later argument is read; there is no `*` width and no `%f`,
 *          `%o` or `%b`.
 *
 *          The line is truncated once it reaches 126 characters including
 *          the prefix, so CR and LF always fit.  @c HAL_GetTick() supplies
 *          the timestamp, including when the call is made from an ISR.
 *
 * @param[in] level Severity label copied into the line: @c 'E', @c 'W',
 *                  @c 'I' or @c 'D' by convention.  Any other character is
 *                  accepted unchanged and nothing is filtered.
 * @param[in] fmt   printf-style format string; must not be NULL.
 * @param[in] ...   Arguments referenced by @p fmt.
 *
 * @pre  @ref app_log_init() has returned; otherwise the call returns
 *       without formatting or queueing, although the caller still evaluates
 *       the arguments it passes.
 * @post Exactly one of: the formatted line (possibly truncated) is queued,
 *       or the line is counted by @ref app_log_dropped (DLOG-003).
 *
 * @note Safe from tasks and from interrupt handlers of any priority, and
 *       before the scheduler starts.  The ring copy masks interrupts for at
 *       most one line (128 bytes), so the worst-case latency added to a
 *       higher-priority ISR is bounded by that copy, not by the UART.
 * @note Each call uses about 128 bytes of the caller's stack in addition to
 *       the formatter's small locals.
 * @warning The output can be truncated or interleaved line-by-line when
 *          several contexts log at once; lines are never split, but the
 *          order between contexts is not defined.
 * @see LOG_E, LOG_W, LOG_I, LOG_D, app_log_flush, app_log_dropped
 */
void app_log(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/**
 * @brief Wait until the queued characters have left the wire.
 * @details Polls USART6 with interrupts masked: while the ring is non-empty
 *          or the transmission-complete flag is clear, it writes the next
 *          byte to TDR whenever TXE is set, until the ring is empty and TC is
 *          set or the loop budget is exhausted.  It then clears @c TXEIE and
 *          restores the previous PRIMASK value.
 *
 *          The wait is bounded by a loop count rather than by the tick
 *          (@c timeout_ms * 50000 iterations, roughly 4 cycles each at
 *          216 MHz) because it is also used from the fault path, where the
 *          tick and the TXE interrupt do not advance.
 *
 * @param[in] timeout_ms Approximate upper bound of the wait in milliseconds.
 *                       The value is multiplied by 50000 to get a loop
 *                       budget, so the granularity is coarse and the real
 *                       duration depends on the clock and the flash wait
 *                       states; 0 sends nothing, it only clears @c TXEIE.
 *
 * @pre  @ref app_log_init() has returned; otherwise the call returns at once.
 * @post @c TXEIE is clear and the interrupts are back in their previous
 *       state.  The ring is empty when the wait succeeded.
 *
 * @note Intended for the start-up failure path and before a deliberate
 *       reset, so that the last lines are not lost; see @c startup_fail() in
 *       @c main.c.  It is also safe from the fault path, where interrupts
 *       are masked anyway.
 * @warning Busy-waits with PRIMASK set.  Do not call it from a task when a
 *          deadline or the watchdog matters, and not from a peripheral
 *          interrupt handler: no other interrupt, including the RTOS tick,
 *          runs during the wait.
 * @warning If the budget expires with bytes still queued, @c TXEIE stays
 *          disabled, so the remainder is only sent when a later @ref app_log
 *          call re-enables it.
 * @note The loop budget saturates at @c UINT32_MAX instead of wrapping, so a
 *       timeout above about 86 s waits for the longest representable budget
 *       rather than returning early.
 * @see app_log, app_log_init
 */
void app_log_flush(uint32_t timeout_ms);

/**
 * @brief Number of debug lines dropped because the ring was full.
 * @details Counts rejected lines, not bytes: a line that does not fit in the
 *          free space of the ring is discarded whole and increments the
 *          counter once.  The counter is never cleared by this module, so it
 *          accumulates from reset; it wraps naturally at 2^32.
 *
 * @return Lines dropped since reset (DLOG-003).
 * @retval 0 No line has been dropped.
 *
 * @note The implementation is a single aligned 32-bit read of a volatile
 *       counter, so it is safe to call from any task or interrupt and never
 *       blocks.  A non-zero value means the terminal output is incomplete.
 * @see app_log
 */
uint32_t app_log_dropped(void);

/**
 * @brief Log at error level (@c 'E').
 * @details Shorthand for `app_log('E', ...)`.  The character is only a label
 *          in the transmitted line; this module does not suppress any level.
 * @param[in] ... printf-style format string followed by its arguments,
 *                interpreted by @ref app_log.
 * @note The arguments are always evaluated and the line is always formatted
 *       and queued, so an error path pays the full cost even when nobody
 *       reads the terminal.  The call is a no-op only before
 *       @ref app_log_init.
 * @see app_log, LOG_W, LOG_I, LOG_D
 */
#define LOG_E(...) app_log('E', __VA_ARGS__)

/**
 * @brief Log at warning level (@c 'W').
 * @details Shorthand for `app_log('W', ...)`; the label appears in the line
 *          but no filtering is applied.
 * @param[in] ... printf-style format string followed by its arguments,
 *                interpreted by @ref app_log.
 * @note Arguments are always evaluated and the line is always queued.
 * @see app_log, LOG_E
 */
#define LOG_W(...) app_log('W', __VA_ARGS__)

/**
 * @brief Log at informational level (@c 'I').
 * @details Shorthand for `app_log('I', ...)`; used for the start-up banner
 *          and the fault summary in @c main.c.
 * @param[in] ... printf-style format string followed by its arguments,
 *                interpreted by @ref app_log.
 * @note Arguments are always evaluated and the line is always queued.
 * @see app_log, LOG_W
 */
#define LOG_I(...) app_log('I', __VA_ARGS__)

/**
 * @brief Log at debug level (@c 'D').
 * @details Shorthand for `app_log('D', ...)`; the cheapest way to add
 *          diagnostics from a task or an ISR.
 * @param[in] ... printf-style format string followed by its arguments,
 *                interpreted by @ref app_log.
 * @note There is no build switch that removes @c LOG_D from the image: the
 *       line is formatted and queued in every build, so this macro must not
 *       be used on a hot path where the cost of formatting matters.
 * @see app_log, LOG_I
 */
#define LOG_D(...) app_log('D', __VA_ARGS__)

/** @} */

#endif /* APP_LOG_H */
