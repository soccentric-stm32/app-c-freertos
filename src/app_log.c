/**
 * @file    app_log.c
 * @brief   Line-oriented debug text output on USART6 (PG14, Arduino D1).
 * @details Implements the public API of the @ref app_log group declared in
 *          @ref app_log.h.  USART6 is the debug port shared with BOOT, so the
 *          pin (PG14, Arduino D1, alternate function 8), the 115200 8N1 frame
 *          and the `[tick] C-FRT <level> <message>` line format are fixed by
 *          the bootloader contract (DLOG-001); one terminal therefore shows
 *          the whole boot chain from BOOT through this application.
 *
 *          Transmission model (DLOG-002): a 1 KB ring buffer is drained by
 *          @ref USART6_IRQHandler on the TXE flag.  The handler makes no
 *          FreeRTOS call, so it sits at the lowest NVIC urgency
 *          (@ref LOG_IRQ_PRIORITY).  That urgency is numerically above
 *          @c configMAX_SYSCALL_INTERRUPT_PRIORITY, that is it lies inside the
 *          priority range that the kernel's critical sections mask, so the
 *          kernel can delay the UART.  In the other direction the handler,
 *          being the lowest urgency, cannot delay Comms, Health or the
 *          watchdog by more than its own bounded work and cannot delay any
 *          other interrupt at all.  Writers take PRIMASK only for the copy of
 *          one line into the ring, and masking interrupts also masks PendSV,
 *          so that copy is atomic against every other context on this
 *          single-core part.
 *
 *          Loss accounting (DLOG-003): a line that does not fit into the free
 *          space of the ring is discarded whole, never split, and counted
 *          once in @ref log_dropped.  A line that exceeds the stack line
 *          buffer is truncated on the character level and still transmitted,
 *          so truncation is silent while a drop is counted.
 *
 *          Formatter: this file carries its own, because newlib-nano's
 *          @c snprintf pulls in malloc and the application has no C heap (the
 *          FreeRTOS heap_4 pool owns the RAM).  It supports `%%`, `%c`, `%s`
 *          with an optional `.N` precision, `%d`, `%i`, `%u` and `%x` with an
 *          optional `l` modifier, and zero-padded widths such as `%08lx`.
 *          There is no floating point, no `*` width and no `%o`, `%b` or
 *          `%p`.  Nothing in the transmit path allocates or blocks; only
 *          @ref app_log_init touches the HAL, and @ref app_log calls
 *          @c HAL_GetTick() for the timestamp.
 *
 *          Execution contexts: @ref app_log is callable from any task, from
 *          any maskable interrupt handler and before the scheduler starts;
 *          @ref app_log_init is a start-up call made once from @c main();
 *          @ref app_log_flush busy-waits with interrupts masked and belongs
 *          to the start-up and fault paths; @ref USART6_IRQHandler is the
 *          transmit ISR.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-06
 * @target  STM32F767ZIT6
 * @srs     DLOG-001..DLOG-004
 * @ingroup app_log
 *
 * @note The module keeps no persistent state: @ref log_ready is a plain
 *       @c .bss flag and the ring counters restart from zero after a reset.
 * @see app_log.h, app_log, app_log_flush, app_log_dropped
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include "stm32f7xx_hal.h"
#include "app_log.h"

/**
 * @brief USART6 baud rate in bit/s.
 * @details 8N1 follows from the register writes in @ref app_log_init @c CR1
 *          receives only TE and UE, so the reset word length (8 data bits, no
 *          parity) and 16x oversampling are kept, and @c CR2 is never touched,
 *          so the reset stop-bit setting (one stop bit) remains.
 * @note Fixed by the bootloader contract and shared with BOOT (DLOG-001).
 *       Changing it makes the line unreadable for the terminal that captured
 *       the boot messages.
 */
#define LOG_BAUD          115200U
/**
 * @brief Size of the transmit ring in bytes.
 * @details Must stay a power of two: both the producer and the consumer wrap
 *          their index with `& (LOG_RING_SIZE - 1U)` instead of a modulo.
 * @note The ring holds 1024 bytes, not 1023: the counters are free-running
 *       counts, so a completely full ring is still distinguishable from an
 *       empty one (see @ref log_push).
 */
#define LOG_RING_SIZE     1024U
/**
 * @brief Size of the stack buffer that one @ref app_log call formats into.
 * @details Two bytes are reserved for CR LF, so at most 126 bytes of prefix
 *          plus message survive in a line.
 * @note The buffer is allocated on the caller's stack on every call, which is
 *       why task stacks in this application are sized with this value in mind.
 */
#define LOG_LINE_MAX      128U
/**
 * @brief Fixed tag placed between the tick and the level in every line.
 * @note Part of the line format shared with BOOT (DLOG-001); the terminal
 *       filters on it to tell application lines from bootloader lines.
 */
#define LOG_TAG           "C-FRT"
/**
 * @brief NVIC priority of @c USART6_IRQn, the lowest possible urgency.
 * @details Numerically equal to @c configLIBRARY_LOWEST_INTERRUPT_PRIORITY and
 *          above @c configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, so the
 *          handler is masked by every kernel critical section and by every
 *          other interrupt, and it calls no FreeRTOS API.
 */
#define LOG_IRQ_PRIORITY  15U

/**
 * @brief USART6 interrupt handler; drains the transmit ring one byte at a time.
 * @internal
 * @details The name matches the vector in the CMSIS start-up file, so this
 *          definition overrides the weak default without a registration call.
 *          Behaviour is documented at the definition below.
 */
void USART6_IRQHandler(void);

/**
 * @brief Transmit ring shared by the writers and by the TXE interrupt.
 * @details Only the byte range `[log_tail, log_head)` modulo the ring size is
 *          meaningful; the array is neither a string nor NUL-terminated, and
 *          its content is never cleared, only overwritten.
 * @note Written by @ref log_push with interrupts masked and read by
 *       @ref USART6_IRQHandler; the masking is what makes the pair consistent.
 * @see log_push, USART6_IRQHandler
 */
static char log_ring[LOG_RING_SIZE];
/**
 * @brief Free-running count of bytes ever queued into @ref log_ring.
 * @details Not masked to the ring size, so the number of queued bytes is
 *          `log_head - log_tail` in unsigned 32-bit arithmetic.  Advanced only
 *          by @ref log_push, which runs with interrupts masked.
 * @note The counter wraps naturally after 2^32 bytes (several days of
 *       continuous 115200 baud traffic); the difference stays valid across the
 *       wrap because it is computed modulo 2^32.
 */
static volatile uint32_t log_head;
/**
 * @brief Free-running count of bytes ever handed to @c USART6->TDR.
 * @details Written only by @ref USART6_IRQHandler and by the polling loop in
 *          @ref app_log_flush; both run with interrupts masked, so no further
 *          protection is required.
 */
static volatile uint32_t log_tail;
/**
 * @brief Number of lines rejected because the ring was full (DLOG-003).
 * @details Incremented once per rejected line, never decremented and never
 *          cleared, so it accumulates from reset and wraps after 2^32 lines.
 * @note Read by @ref app_log_dropped without masking; an aligned 32-bit load is
 *       atomic on Cortex-M7.
 */
static volatile uint32_t log_dropped;
/**
 * @brief True once @ref app_log_init has programmed PG14 and USART6.
 * @details Guards @ref app_log and @ref app_log_flush, so that a call from the
 *          start-up failure path before initialisation does nothing instead of
 *          writing to an unclocked peripheral or spinning on a dead UART.
 * @note Set once at the end of @ref app_log_init and never cleared.
 */
static bool log_ready;

/**
 * @brief Configure PG14 as USART6_TX and start the interrupt-driven output.
 * @details Enables the GPIOG and USART6 clocks, drives PG14 as alternate
 *          function 8, push-pull, no pull, medium speed - the Arduino D1 pin
 *          that reaches the ST-LINK virtual COM port (DLOG-001).  @c CR1 is
 *          cleared first so that UE is 0 while @c BRR is written, which the
 *          reference manual requires; @c BRR is then programmed with
 *          `(PCLK2 + LOG_BAUD/2) / LOG_BAUD`.  With 16x oversampling the
 *          numeric BRR value is the oversampled divisor itself, because the
 *          hardware splits it into the 12-bit mantissa and the 4-bit
 *          fraction, so 108 MHz and 115200 baud give 938 and the rounding
 *          error is below 0.1 %.  @c CR1 finally enables TE and UE.  The TXE
 *          interrupt is enabled in the NVIC only, so the first queued line
 *          is what sets @c TXEIE and starts the ISR.
 *
 *          @ref log_ready is set last, so a concurrent @ref app_log either
 *          sees a fully programmed port or returns immediately.
 *
 * @pre  Thread context, after @c HAL_Init() and after @ref app_clock_init has
 *       established PCLK2 = 108 MHz; @c main() calls it once, after
 *       @c app_handover_read() and before the first @c LOG_I.
 * @post USART6 transmits at 115200 8N1, @c USART6_IRQn is enabled at the
 *       lowest urgency, @c TXEIE is still clear and @ref app_log starts
 *       accepting lines.
 *
 * @note No FreeRTOS object is created or touched and nothing blocks, so the
 *       call is valid before @c vTaskStartScheduler().
 * @note @c HAL_GetTick() must already work, because every later line is
 *       timestamped with it; without @c HAL_Init() the tick source is not
 *       configured and every timestamp would read 0.
 * @warning Call it once.  A second call clears @c CR1 (stopping the
 *          transmitter without waiting for TC), re-programs the GPIO and
 *          clears @c TXEIE, so a line still in the ring stays there until a
 *          later @ref app_log re-enables the interrupt.
 * @see app_log, app_log_flush, USART6_IRQHandler
 */
void app_log_init(void)
{
    __HAL_RCC_GPIOG_CLK_ENABLE();
    __HAL_RCC_USART6_CLK_ENABLE();

    GPIO_InitTypeDef tx = {
        .Pin = GPIO_PIN_14, .Mode = GPIO_MODE_AF_PP, .Pull = GPIO_NOPULL,
        .Speed = GPIO_SPEED_FREQ_MEDIUM, .Alternate = GPIO_AF8_USART6
    };
    HAL_GPIO_Init(GPIOG, &tx);

    USART6->CR1 = 0U;
    USART6->BRR = (HAL_RCC_GetPCLK2Freq() + (LOG_BAUD / 2U)) / LOG_BAUD;
    USART6->CR1 = USART_CR1_TE | USART_CR1_UE;

    HAL_NVIC_SetPriority(USART6_IRQn, LOG_IRQ_PRIORITY, 0U);
    HAL_NVIC_EnableIRQ(USART6_IRQn);
    log_ready = true;
}

/**
 * @brief Copy one formatted line into the transmit ring (DLOG-002, DLOG-003).
 * @details Free space is `LOG_RING_SIZE - (log_head - log_tail)`.  Both
 *          counters are free-running 32-bit byte counts rather than masked
 *          indices, so the subtraction yields the queued byte count exactly,
 *          including across a wrap of either counter, provided the queue never
 *          exceeds @ref LOG_RING_SIZE.  The test is `len > free`, not
 *          `len >= free`, so a line that exactly fills the ring is accepted;
 *          a full ring is still distinguishable from an empty one because the
 *          counters are compared at full width instead of after masking.
 *
 *          When the line does not fit, nothing is copied, @ref log_dropped is
 *          incremented once and the ring is left byte-identical, so a line is
 *          never split across a drop.  Otherwise the bytes are copied with
 *          the index masked by `LOG_RING_SIZE - 1U`, @ref log_head advances by
 *          @p len and @c TXEIE is set so that @ref USART6_IRQHandler starts
 *          draining.  Enabling @c TXEIE while interrupts are still masked
 *          keeps the ISR from observing a half-updated @ref log_head.
 *
 * @param[in] text Formatted bytes; not NUL-terminated, so @p len is the only
 *                 length information.
 * @param[in] len  Number of bytes to queue, in practice 1..@ref LOG_LINE_MAX.
 *
 * @pre  @p text addresses @p len readable bytes and does not overlap
 *       @ref log_ring.
 * @post Exactly one of: the bytes are queued with @c TXEIE set, or
 *       @ref log_dropped has advanced by one and the ring is unchanged.
 *
 * @note Saves PRIMASK and disables interrupts for the whole copy, that is for
 *       at most @ref LOG_LINE_MAX byte stores.  Masking interrupts also masks
 *       PendSV, so no task switch can occur inside the copy and the sequence
 *       is atomic against every other context of this single-core part.
 * @note The critical section is bounded by the line length, not by the ring
 *       size or the queued amount, so a busy ring does not lengthen it.
 * @warning NMI and HardFault are not masked by PRIMASK.  A call to
 *          @ref app_log from those handlers could interleave with a copy that
 *          is already in progress and corrupt the queue.
 * @see app_log, USART6_IRQHandler, app_log_flush
 */
static void log_push(const char *text, uint32_t len)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    if (len > LOG_RING_SIZE - (log_head - log_tail)) {
        log_dropped++;
    } else {
        for (uint32_t i = 0U; i < len; i++) {
            log_ring[(log_head + i) & (LOG_RING_SIZE - 1U)] = text[i];
        }
        log_head += len;
        USART6->CR1 |= USART_CR1_TXEIE;
    }
    __set_PRIMASK(primask);
}

/**
 * @brief Bounded output sink used by the local formatter.
 * @details Every formatter output goes through @ref out_char, so one capacity
 *          check covers all conversions and the sink can never overrun the
 *          caller's stack buffer.  The object lives on the stack of
 *          @ref app_log for the duration of one line and is never shared.
 */
typedef struct {
    char *buf;  /**< Destination buffer; never NUL-terminated by the sink. */
    size_t len; /**< Characters written so far, also the next write index. */
    size_t cap; /**< Capacity in bytes; writes at or past it are discarded. */
} log_out_t;

/**
 * @brief Append one character to the sink when there is room.
 * @details The single write path into @ref log_out_t, so the bound check is
 *          implemented once and inherited by every conversion.  A character
 *          that does not fit is discarded silently: there is no error return
 *          and no counter, which is what makes truncation a free side effect
 *          rather than a branch in every conversion.
 *
 * @param[in,out] out Sink to append to.
 * @param[in]     c   Character to append, stored verbatim (including '\0').
 *
 * @pre  `out->buf` addresses at least `out->cap` writable bytes.
 * @post `out->len` is one larger when the character fit, otherwise the sink is
 *       unchanged.
 * @note Does not NUL-terminate, so callers must carry the length next to the
 *       buffer; @ref app_log relies on that when it passes the line to
 *       @ref log_push.
 * @see log_format, log_out_t
 */
static void out_char(log_out_t *out, char c)
{
    if (out->len < out->cap) {
        out->buf[out->len] = c;
        out->len++;
    }
}

/**
 * @brief Render one integer into the sink in the given radix.
 * @details The digits are produced least significant first into a local
 *          reversed array, the '-' is appended last when @p negative is set,
 *          `width - n` copies of @p pad are emitted, and the array is then
 *          written back to front so the digits come out in reading order.
 *          The array holds 10 decimal digits plus the sign, which covers every
 *          32-bit magnitude including 2147483648, the magnitude of INT32_MIN.
 *
 * @param[in,out] out      Sink receiving the characters; anything past
 *                         `out->cap` is dropped by @ref out_char.
 * @param[in]     value    Magnitude to render, already unsigned, so a negative
 *                         @c int is passed as its two's-complement value.
 * @param[in]     base     Radix; every current call site passes 10U or 16U.
 * @param[in]     negative True to append a '-'; the sign is handled here
 *                         because @p value carries only the magnitude.
 * @param[in]     width    Minimum field width in characters, 0 for none.
 * @param[in]     pad      Fill character, ' ' or '0'.
 *
 * @pre   @p base is between 2 and 16, otherwise the division loop cannot
 *        terminate or produces digits outside the 'a'..'f' range.
 * @post  No global state changes; @p out is only appended to.
 *
 * @note Digits above 9 are lower case (`a`..`f`), matching `%x`.
 * @note Zero padding is emitted before the sign, so `%06d` renders -42 as
 *       `000-42` rather than printf's `-00042`; no current caller pads a
 *       negative decimal, but the difference is visible if one ever does.
 * @note The width is a minimum only; a value wider than @p width is printed in
 *       full and the sink truncates if the line runs out.
 * @see out_char, log_format
 */
static void out_number(log_out_t *out, uint32_t value, uint32_t base,
                       bool negative, uint32_t width, char pad)
{
    char digits[11];
    uint32_t n = 0U;
    do {
        uint32_t d = value % base;
        digits[n] = (char)(d < 10U ? '0' + d : 'a' + d - 10U);
        n++;
        value /= base;
    } while (value != 0U);
    if (negative) {
        digits[n] = '-';
        n++;
    }
    for (uint32_t i = n; i < width; i++) {
        out_char(out, pad);
    }
    while (n > 0U) {
        n--;
        out_char(out, digits[n]);
    }
}

/**
 * @brief Expand the supported printf subset into the output sink.
 * @details Grammar accepted after a '%':
 *          -# an optional `0` flag; only '0' is recognised, and it is consumed
 *             before the width so that `%08lx` keeps its 8 and pads with '0',
 *          -# an optional decimal width; there is no `*` width,
 *          -# an optional '.' followed by a decimal precision; there is no
 *             `*` precision,
 *          -# an optional `l`, ignored because long and int are both 32 bits
 *             in this ABI,
 *          -# exactly one conversion out of `c`, `s`, `d`, `i`, `u`, `x`, `%`.
 *
 *          The precision defaults to @c UINT32_MAX and applies to `%s` only,
 *          where it is the maximum number of characters copied (NULL prints
 *          nothing because the pointer is tested first).  The width and the
 *          pad apply to `%d`, `%i`, `%u` and `%x` only; `%c` ignores both.
 *          `%d` and `%i` are identical and handle INT32_MIN by negating
 *          `value + 1` before the conversion, so the magnitude never
 *          overflows.
 *
 *          An unsupported conversion is copied through verbatim (`%p` prints
 *          `%p`), consumes no argument and returns from the formatter
 *          immediately, so no later @c va_arg is read and an unknown
 *          conversion cannot shift the following arguments.  There is no
 *          `%f`, `%o`, `%b`, `%n` and no length modifier other than `l`.  A
 *          format string that ends in a lone '%' emits '%' and returns, so
 *          the loop never reads past the terminator.
 *
 * @param[in,out] out  Sink receiving the expanded characters.
 * @param[in]     fmt  NUL-terminated format string; must not be NULL.
 * @param[in]     args Argument list already positioned after @p fmt; the
 *                     caller owns the @c va_start / @c va_end pair.
 *
 * @pre   Formatting stops at the first unsupported conversion, so text after
 *        it is not emitted and no argument beyond that point is read.  A
 *        supported conversion must still match the argument actually passed.
 * @post  @p args has been advanced past the arguments consumed before the end
 *        of the format string or before the first unsupported conversion, and
 *        @p fmt is not modified.
 *
 * @note No allocation, no locale, no recursion; the cost is bounded by the
 *       length of @p fmt plus the characters that fit into @p out.
 * @see out_char, out_number, log_printf
 */
static void log_format(log_out_t *out, const char *fmt, va_list args)
{
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            out_char(out, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        uint32_t width = 0U;
        while (*fmt >= '0' && *fmt <= '9') {
            width = (width * 10U) + (uint32_t)(*fmt - '0');
            fmt++;
        }
        uint32_t precision = UINT32_MAX;
        if (*fmt == '.') {
            precision = 0U;
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = (precision * 10U) + (uint32_t)(*fmt - '0');
                fmt++;
            }
        }
        if (*fmt == 'l') {
            fmt++;      /* long and int are both 32 bits here */
        }
        switch (*fmt) {
        case 'c':
            out_char(out, (char)va_arg(args, int));
            break;
        case 's': {
            const char *text = va_arg(args, const char *);
            for (uint32_t i = 0U; text != NULL && text[i] != '\0' &&
                                  i < precision; i++) {
                out_char(out, text[i]);
            }
            break;
        }
        case 'd':
        case 'i': {
            int32_t value = va_arg(args, int32_t);
            out_number(out, value < 0 ? (uint32_t)(-(value + 1)) + 1U
                                      : (uint32_t)value,
                       10U, value < 0, width, pad);
            break;
        }
        case 'u':
            out_number(out, va_arg(args, uint32_t), 10U, false, width, pad);
            break;
        case 'x':
            out_number(out, va_arg(args, uint32_t), 16U, false, width, pad);
            break;
        case '%':
            out_char(out, '%');
            break;
        default:        /* unsupported: echo it, then stop before later args */
            out_char(out, '%');
            if (*fmt == '\0') {
                return;
            }
            out_char(out, *fmt);
            return;
        }
    }
}

/**
 * @brief Start an argument list and hand it to @ref log_format.
 * @details Exists so @ref app_log can format the prefix and the message with
 *          two separate calls while sharing one sink, which keeps the fixed
 *          prefix independent of the caller's format string while the
 *          truncation limit still applies to the concatenation.
 *
 * @param[in,out] out Sink receiving the expanded characters.
 * @param[in]     fmt NUL-terminated format string; must not be NULL.
 * @param[in]     ... Arguments referenced by @p fmt.
 *
 * @pre   The format string may contain unsupported conversions;
 *        @ref log_format stops at the first one without reading later
 *        arguments.
 * @post  The sink may have grown; no other state changes.
 * @note Deliberately not annotated with @c format(printf, 2, 3): the
 *       implementation accepts a subset and would not honour every
 *       conversion the attribute permits.
 * @see log_format, app_log
 */
static void log_printf(log_out_t *out, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    log_format(out, fmt, args);
    va_end(args);
}

/**
 * @brief Format one debug line and queue it for transmission on USART6.
 * @details Builds `[<tick>] C-FRT <level> <message><CR><LF>` in a 128-byte
 *          stack buffer and hands the buffer to @ref log_push.  The tick is
 *          @c HAL_GetTick() in milliseconds, right-aligned in seven columns
 *          by `%7lu`, which is valid here because @c unsigned long and
 *          @c uint32_t are both 32-bit on this ABI; @ref LOG_TAG supplies the
 *          fixed tag and @p level the single-character label.
 *
 *          The sink capacity is set to `sizeof(line) - 2` while the content
 *          is formatted, so prefix plus message are truncated at 126
 *          characters and the trailing CR LF always fit.  The capacity is
 *          then raised to the full buffer size for the two line-ending
 *          characters.  The buffer is never NUL-terminated: @ref log_push
 *          copies exactly the produced length.
 *
 * @param[in] level Severity label, 'E', 'W', 'I' or 'D' by convention,
 *                  copied into the line verbatim; nothing is filtered, so any
 *                  other character is accepted and transmitted.
 * @param[in] fmt   printf-style format string for the message part.
 * @param[in] ...   Arguments referenced by @p fmt.
 *
 * @pre  @ref app_log_init() has returned; otherwise the call returns before
 *       formatting, so the line is lost silently although the caller's
 *       arguments were still evaluated.
 * @post Exactly one of: the line is queued and @c TXEIE is set, or the line
 *       was rejected whole and @ref log_dropped advanced by one.
 *
 * @note Safe from tasks, from maskable interrupts and before the scheduler
 *       starts: the only critical section is the bounded copy in
 *       @ref log_push.
 * @note Uses roughly @ref LOG_LINE_MAX bytes of the caller's stack on every
 *       call, so a task that logs needs stack headroom for a full line on top
 *       of its own frame (see the stack sizing in @c button_task.c).
 * @warning A NULL @p fmt is the only unchecked input and dereferences address
 *          0 inside @ref log_format.
 * @warning Lines are never split, but the order between two contexts follows
 *          who reaches @ref log_push first, so a task and an ISR can interleave
 *          whole lines unpredictably.
 * @see LOG_E, LOG_W, LOG_I, LOG_D, log_push, app_log_flush
 */
void app_log(char level, const char *fmt, ...)
{
    if (!log_ready) {
        return;
    }
    char line[LOG_LINE_MAX];
    /* Leave room for CR LF at the end of a truncated line. */
    log_out_t out = { line, 0U, sizeof(line) - 2U };
    log_printf(&out, "[%7lu] " LOG_TAG " %c ", HAL_GetTick(), level);
    va_list args;
    va_start(args, fmt);
    log_format(&out, fmt, args);
    va_end(args);
    out.cap = sizeof(line);
    out_char(&out, '\r');
    out_char(&out, '\n');
    log_push(line, (uint32_t)out.len);
}

/**
 * @brief Drain the ring and wait for the last stop bit, polling with
 *        interrupts masked.
 * @details The loop condition is "ring non-empty, or TC still clear".  Every
 *          iteration that finds queued data and TXE set writes the byte at
 *          @ref log_tail to @c TDR and advances @ref log_tail, exactly as
 *          @ref USART6_IRQHandler would.  Because PRIMASK is set, the handler
 *          cannot run during the wait, so this loop is the only consumer and
 *          no further locking is needed.  Once the ring is empty the loop
 *          keeps waiting on TC, which guarantees that the last byte has left
 *          the shift register before the caller resets the part.
 *
 *          The budget is a loop count, not a time: @p timeout_ms is
 *          multiplied by 50000 at about 4 cycles per iteration and 216 MHz,
 *          because this function must also work from the fault path, where
 *          neither the TXE interrupt nor the HAL tick advances, so
 *          @c HAL_GetTick() cannot be used as a deadline.
 *
 *          On exit @c TXEIE is cleared unconditionally and PRIMASK is
 *          restored to the value saved on entry, so a caller that entered
 *          with interrupts enabled does not leave them disabled.
 *
 * @param[in] timeout_ms Approximate wait budget in milliseconds.  0 performs
 *                       no polling at all and only clears @c TXEIE.
 *
 * @pre  @ref app_log_init() has returned; otherwise the call returns at once
 *       without touching the peripheral.
 * @post @c TXEIE is clear, interrupts are back in their previous state, and
 *       the ring is empty unless the budget expired first.
 *
 * @note Interrupts are masked before the first read of @ref log_head, so a
 *       writer that is mid-copy is never observed half updated.
 * @warning Runs with interrupts disabled: no other handler and no RTOS tick
 *          executes during the wait, the watchdog is not fed and every pending
 *          interrupt is delayed by the whole budget.
 * @warning If the budget expires with bytes still queued, @c TXEIE stays clear
 *          and the remainder is only sent when a later @ref app_log re-enables
 *          the interrupt.
 * @note The loop count saturates at @c UINT32_MAX instead of wrapping, so a
 *       timeout above about 86 s waits for the longest representable budget
 *       rather than returning immediately.
 * @see app_log, USART6_IRQHandler, log_push
 */
void app_log_flush(uint32_t timeout_ms)
{
    if (!log_ready) {
        return;
    }
    /* Drain by polling with interrupts off: this also runs from the fault
     * path, where neither the TXE interrupt nor the HAL tick advances.
     * The bound is a loop count for the same reason (about 4 cycles each
     * at 216 MHz). */
    uint32_t loops = timeout_ms > (UINT32_MAX / 50000U)
                         ? UINT32_MAX : timeout_ms * 50000U;
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    while ((log_head != log_tail || (USART6->ISR & USART_ISR_TC) == 0U) &&
           loops-- > 0U) {
        if (log_head != log_tail && (USART6->ISR & USART_ISR_TXE) != 0U) {
            USART6->TDR = (uint8_t)log_ring[log_tail & (LOG_RING_SIZE - 1U)];
            log_tail++;
        }
    }
    USART6->CR1 &= ~USART_CR1_TXEIE;
    __set_PRIMASK(primask);
}

/**
 * @brief Return the number of lines rejected by @ref log_push.
 * @details A single aligned load of a @c volatile @c uint32_t, which is atomic
 *          on Cortex-M7, so the value can be read from any context without
 *          masking.  The counter counts lines, not bytes, and is never cleared
 *          by this module, so it accumulates from reset.
 *
 * @return Lines dropped since reset (DLOG-003).
 * @retval 0 Every line so far was queued.
 *
 * @note A non-zero value means the captured terminal output is incomplete;
 *       nothing retries or reorders the rejected lines, because the logger
 *       owns no storage beyond the ring.
 * @see app_log, log_push
 */
uint32_t app_log_dropped(void)
{
    return log_dropped;
}

/**
 * @brief USART6 TXE interrupt: move one byte from the ring to the wire.
 * @details Runs in interrupt context at the lowest NVIC urgency
 *          (@ref LOG_IRQ_PRIORITY) whenever @c TXEIE is set and TXE is
 *          asserted.  The flag is cleared by the write to @c TDR, so no
 *          explicit clear is needed.  While queued data exists, exactly one
 *          byte is transferred per entry; when the queue is empty @c TXEIE is
 *          cleared, which ends the burst and lets the transmitter go idle
 *          after the stop bit.  The handler makes no FreeRTOS call, no HAL
 *          call, no allocation and no logging of its own: its work is two
 *          status reads, one masked index, one @c TDR store and one counter
 *          increment.
 *
 *          No critical section is needed even though @ref log_head is written
 *          by @ref log_push the producer only raises @ref log_head under
 *          PRIMASK, and this handler cannot run while PRIMASK is set, so it
 *          always observes a consistent `[log_tail, log_head)` range.
 *
 * @pre  @ref app_log_init() has programmed USART6 and enabled the vector in
 *       the NVIC; a @c TDR write before TE is set would be ignored.
 * @post @c TXEIE is left set when data remains and cleared when the ring is
 *       empty; @ref log_tail has advanced by at most one.
 *
 * @note The error flags (ORE, FE, NE) and TC are ignored: reception is not
 *       enabled, so no overrun can occur, and TXE is the only enabled
 *       interrupt source.
 * @note The handler is not reentrant against @ref app_log_flush, which masks
 *       interrupts before it starts polling; do not call the flush from here.
 * @see app_log, app_log_flush, log_push
 */
void USART6_IRQHandler(void)
{
    if ((USART6->ISR & USART_ISR_TXE) == 0U) {
        return;
    }
    if (log_tail != log_head) {
        USART6->TDR = (uint8_t)log_ring[log_tail & (LOG_RING_SIZE - 1U)];
        log_tail++;
    } else {
        USART6->CR1 &= ~USART_CR1_TXEIE;
    }
}
