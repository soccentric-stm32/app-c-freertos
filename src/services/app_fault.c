/**
 * @file    app_fault.c
 * @brief   No-init fault record, exception wrappers and fatal reset paths.
 * @details Implements the objects and entry points declared in
 *          @ref app_fault.h, that is the public group @ref app_fault.  The
 *          translation unit owns exactly two persistent objects, both placed
 *          in the @c .noinit section inside the 1 KB window at
 *          APP_NOINIT_ADDR: @ref g_app_fault, the visible record, and
 *          @c app_boot_id, the retained boot counter.  Start-up code clears
 *          only @c .bss, and BOOT never writes that window, so both survive a
 *          system reset, a watchdog reset and a fault reset (DGEN-004).
 *
 *          Publish protocol: @ref app_fault_save clears the magic word, issues
 *          a @c __DMB, fills the payload, issues a second @c __DMB and finally
 *          stores APP_FAULT_MAGIC followed by a @c __DSB.  A reset that lands
 *          anywhere before the last store therefore leaves an invalid record
 *          instead of a half-written one, and a reader never trusts a record
 *          it has not validated through @ref app_fault_is_valid.
 *
 *          Cache and ordering: the record lives in DTCM at 0x20000000, which
 *          is not cacheable on the Cortex-M7 and is not covered by the MPU
 *          region configured in @ref app_cache_mpu_init, so the record needs
 *          no clean or invalidate operation even after the D-cache is enabled
 *          (DGEN-003).  The barriers above order the stores against each other
 *          and against the reset that follows, they are not cache maintenance.
 *
 *          Fatal path: @ref app_fault_show_and_reset disables interrupts,
 *          reloads the IWDG once so the LD3 pattern starts with a full
 *          timeout, enables the GPIOB clock, turns PB14 into an output by
 *          writing @c RCC->AHB1ENR and @c GPIOB->MODER directly, flashes LD3
 *          three times in each of two rounds, and resets.  It works before
 *          the HAL, before the clock tree, before the GPIO driver and without a
 *          scheduler, which is what lets the exception wrappers, the FreeRTOS
 *          hooks, the assertion sink and the start-up failures share one path
 *          (DRT-002).  The file uses no libc, no heap and no formatting, so a
 *          fatal path cannot fault again on a corrupted heap or stack.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     DGEN-004, DRT-002, LED-005
 * @ingroup app_fault
 *
 * @note The record is the only state that crosses a reset in this module; the
 *       reason names and the code path themselves are constant.
 * @see app_fault.h
 * @see boot_contract.h
 * @see rtos_hooks.c
 */

#include "stm32f7xx_hal.h"
#include "app_board.h"
#include "services/app_fault.h"
#include "services/boot_contract.h"

/**
 * @brief The retained fault record, published to the whole application.
 * @details The explicit @c section attribute places the object in the
 *          @c .noinit input section, which @c ld/app.ld collects with
 *          @c KEEP into the @c NOLOAD output section mapped to the NOINIT
 *          memory region: 0x2001FC00, 1 KB long, above BOOT's 60 KB DTCM
 *          budget.  The section is not part of @c .data, has no load image and
 *          is not cleared by the C start-up code, so its contents survive
 *          every system reset (DGEN-004).
 *
 *          main() reads the record before the scheduler starts and latches
 *          @ref APP_ERROR_FAULT when @ref app_fault_is_valid is true
 *          (LED-005); the health task decides whether a trial boot failed from
 *          the boot-id predicates; the Comms task reports the fields in
 *          Status; the debugger may read the object directly.
 * @note Written only by @ref app_fault_save, which publishes it through the
 *       magic word last, and invalidated by @ref app_fault_clear.  The object
 *       is not @c volatile: the writers and the readers are separated by a
 *       reset, the module assumes a single writer at a time, and the save path
 *       provides its own barriers.
 * @warning The bytes are undefined at power-on, so no field may be used until
 *          @ref app_fault_is_valid returns true, and the character arrays are
 *          only known to be NUL-terminated once a save has completed.
 * @see app_fault_record_t
 */
app_fault_record_t g_app_fault __attribute__((section(".noinit")));

/**
 * @brief Retained boot counter, incremented once per boot.
 * @details Placed in @c .noinit next to the record, so the identifier of the
 *          boot before the current one is still readable after a reset.  That
 *          is what lets @ref app_fault_is_current_boot and
 *          @ref app_fault_is_previous_boot distinguish a fault recorded in
 *          this boot from one inherited from the previous boot.  Incremented
 *          by @ref app_fault_boot_start and copied into
 *          @ref app_fault_record_t.boot_id by @ref app_fault_save.
 * @note File scope and @c static: no other translation unit reads or writes
 *       the counter, which keeps @ref g_app_fault the only cross-module view
 *       of the state.
 * @note Not @c volatile, because every access is in this file and the reset,
 *       not the compiler, carries the value forward.
 * @see app_fault_boot_start
 */
static uint32_t app_boot_id __attribute__((section(".noinit")));

/**
 * @brief Compile-time proof that the no-init objects fit the reserved window
 *        (A-9).
 * @details BOOT reserves the top 1 KB of DTCM and @c ld/app.ld asserts the
 *          same geometry, so the record plus the boot counter must not exceed
 *          APP_NOINIT_SIZE.  Checking at compile time turns an oversized
 *          record into a build error instead of silent corruption of the main
 *          stack, which starts immediately below the window and grows down
 *          into it.  The assertion emits no code.
 * @note @c ld/app.ld repeats the check on the placed section, because the
 *       linker is the only one that sees the alignment padding added around
 *       the two objects.
 * @see APP_NOINIT_ADDR
 * @see APP_NOINIT_SIZE
 */
_Static_assert(sizeof(app_fault_record_t) + sizeof(uint32_t) <= APP_NOINIT_SIZE, "A-9: fault record exceeds the reserved no-init window");

/**
 * @brief Stable ASCII names for @ref app_fault_reason_t, indexed by value.
 * @details Entry @c i is the name of the reason whose enumerator value is
 *          @c i, so the table order must match the enumeration in
 *          @ref app_fault.h.  The strings are the wire vocabulary reported in
 *          Status, so they are part of the host contract and must not be
 *          renamed without a matching host-side update.
 *          @ref app_fault_reason_text is the only reader and range-checks the
 *          index against the table size before indexing.
 * @note @c const and @c static: the storage is read-only, the array is not
 *       exported, and the pointers handed out live for the whole program run.
 * @warning Add an entry only together with the matching enumerator; a missing
 *          entry silently shifts every following name.
 * @see app_fault_reason_text
 * @see app_fault_reason_t
 */
static const char *const fault_reason_names[] = {"none", "hard_fault", "mem_manage", "bus_fault", "usage_fault", "assert", "stack_overflow", "malloc_failed", "startup"};

/**
 * @brief Copy a string into a fixed-size record field, keeping only the
 *        basename.
 * @details Copies at most @p dst_size - 1 characters and always terminates
 *          the result with NUL.  When @p src looks like a path, everything up
 *          to and including the last '/' or '\\' is skipped first, so the
 *          32-byte @ref app_fault_record_t.file field identifies the file
 *          instead of spending its capacity on an absolute build path
 *          (DGEN-004).  The scan only reads the source.
 *
 * @param[out] dst      Destination buffer, normally @c g_app_fault.task or
 *                      @c g_app_fault.file.
 * @param[in]  dst_size Capacity of @p dst in bytes, including the terminator.
 * @param[in]  src      NUL-terminated source string, or NULL when the caller
 *                      has no name to record.
 *
 * @pre  @p dst points to at least @p dst_size writable bytes, or @p dst_size
 *       is 0 and @p dst is not dereferenced.
 * @post @p dst holds the NUL-terminated basename of @p src truncated to
 *       @p dst_size - 1 characters, or an empty string when @p src is NULL or
 *       @p dst_size is 0.
 *
 * @note No libc and no allocation; the copy loop is bounded by @p dst_size, so
 *       a source without a terminator cannot overrun the field.
 * @note Internal helper (file scope) used only by @ref app_fault_save, which
 *       has already invalidated the record, so no locking is needed.
 * @see app_fault_save
 */
static void fault_copy(char *dst, uint32_t dst_size, const char *src)
{
    uint32_t i = 0U;

    if(dst_size == 0U)
    {
        return;
    }
    if(src != NULL)
    {
        /* __FILE__ may be an absolute build path.  Keep the basename so the
         * 31-character field identifies the file (DGEN-004). */
        const char *base = src;
        for(const char *p = src; *p != '\0'; p++)
        {
            if(*p == '/' || *p == '\\')
            {
                base = p + 1;
            }
        }
        while((i + 1U) < dst_size && base[i] != '\0')
        {
            dst[i] = base[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/**
 * @brief Increment the retained boot identifier for the current boot.
 * @details Post-increments @c app_boot_id and maps the wrap value 0 back to 1,
 *          so 0 never identifies a boot and the current/previous comparisons
 *          stay unambiguous.  main() calls this before any interrupt is
 *          enabled and before anything can record a fault, so every record
 *          written during this boot carries the new value.
 * @pre  The @c .noinit window still holds the value left by the previous boot;
 *       after a power-on the content is arbitrary, which is acceptable because
 *       only relative comparisons are made.
 * @post @c app_boot_id differs from its value in the previous boot and is
 *       never 0.
 * @note Not atomic and not reentrant, but main() is the only caller and runs
 *       before the scheduler, so no critical section is needed.  It performs
 *       no blocking and no allocation.
 * @see app_fault_is_current_boot
 * @see app_fault_is_previous_boot
 */
void app_fault_boot_start(void)
{
    app_boot_id++;
    if(app_boot_id == 0U)
    {
        app_boot_id = 1U;
    }
}

/**
 * @brief Report whether @ref g_app_fault holds a complete, plausible record.
 * @details Tests the magic word and the reason range 0..APP_FAULT_STARTUP.
 *          The boot identifier is deliberately not compared, so a valid record
 *          from any earlier boot qualifies; the boot predicates narrow the
 *          answer when the caller needs the age.  There is no checksum, so the
 *          test is a heuristic that relies on the magic word being written
 *          last.
 * @return true when the record can be reported.
 * @retval true  @c magic is APP_FAULT_MAGIC and @c reason is a known
 *               @ref app_fault_reason_t value.
 * @retval false The record was never written, has been cleared, or holds an
 *               out-of-range reason.
 * @note Two plain loads, no masking and no blocking, so it is safe from tasks,
 *       ISRs, the debugger and the fault path itself.
 * @see app_fault_is_current_boot
 * @see app_fault_is_previous_boot
 */
bool app_fault_is_valid(void) { return g_app_fault.magic == APP_FAULT_MAGIC && g_app_fault.reason <= (uint32_t)APP_FAULT_STARTUP; }

/**
 * @brief Report whether the valid record was written during this boot.
 * @details Combines @ref app_fault_is_valid with a comparison of
 *          @ref app_fault_record_t.boot_id against @c app_boot_id.  The health
 *          task uses it to fail a trial boot in which the application faulted
 *          after start-up.
 * @return true when a valid record carries the current boot identifier.
 * @retval true  The record is valid and @c boot_id equals @c app_boot_id.
 * @retval false No valid record, or the record belongs to an older boot.
 * @pre  @ref app_fault_boot_start has run for the current boot.
 * @note Reads only, no blocking, no allocation; safe from tasks and ISRs.
 * @see app_fault_is_previous_boot
 */
bool app_fault_is_current_boot(void) { return app_fault_is_valid() && g_app_fault.boot_id == app_boot_id; }

/**
 * @brief Report whether the valid record was written during the previous boot.
 * @details Derives the previous identifier as @c app_boot_id - 1 and maps the
 *          result 0 to 0xFFFFFFFF, mirroring the wrap rule in
 *          @ref app_fault_boot_start, so the test survives the counter wrap.
 *          The health task uses it while a trial is pending to fail the trial
 *          when the trial boot faulted.
 * @return true when a valid record carries the previous boot identifier.
 * @retval true  The record is valid and @c boot_id equals the mapped previous
 *               value.
 * @retval false No valid record, or the record is from this or an older boot.
 * @pre  @ref app_fault_boot_start has run for the current boot.
 * @note Reads only, no blocking, no allocation; safe from tasks and ISRs.
 * @see app_fault_is_current_boot
 */
bool app_fault_is_previous_boot(void)
{
    uint32_t previous = app_boot_id - 1U;
    if(previous == 0U)
    {
        previous = 0xFFFFFFFFUL;
    }
    return app_fault_is_valid() && g_app_fault.boot_id == previous;
}

/**
 * @brief Map a fault reason to its short, stable ASCII name.
 * @details Range-checks @p reason against the size of @c fault_reason_names
 *          before indexing, so an out-of-range value, including a negative
 *          value cast to the enum, returns "unknown" instead of reading past
 *          the table or before it.
 * @param[in] reason Reason code to translate.
 * @return Pointer to a static NUL-terminated string; never NULL.
 * @retval "none".."startup" @p reason is a valid @ref app_fault_reason_t.
 * @retval "unknown" @p reason is outside the table, that is greater than
 *         APP_FAULT_STARTUP.
 * @note The returned string is read-only storage owned by the module: do not
 *       free or modify it.  Callable from any context, including the fault
 *       path.
 * @see app_fault_reason_t
 */
const char *app_fault_reason_text(app_fault_reason_t reason)
{
    if((uint32_t)reason >= (sizeof(fault_reason_names) / sizeof(fault_reason_names[0])))
    {
        return "unknown";
    }
    return fault_reason_names[reason];
}

/**
 * @brief Return the task name stored in the fault record.
 * @details Returns the address of @ref app_fault_record_t.task, which is empty
 *          when the record was saved without a task name, as the exception
 *          wrappers do.
 * @return Pointer to the in-record NUL-terminated task name; never NULL.
 * @note The pointer aliases @ref g_app_fault, so it is invalidated by the next
 *       @ref app_fault_save or @ref app_fault_clear and must never be freed.
 * @warning The bytes are only known to be NUL-terminated when
 *          @ref app_fault_is_valid is true; before that they are whatever the
 *          no-init RAM held.
 * @see app_fault_file
 */
const char *app_fault_task(void) { return g_app_fault.task; }

/**
 * @brief Return the source-file basename stored in the fault record.
 * @details Returns the address of @ref app_fault_record_t.file.  The save path
 *          strips the directory and limits the result to 31 characters, so the
 *          field identifies the file rather than the build tree (DGEN-004).
 * @return Pointer to the in-record NUL-terminated basename; never NULL.
 * @note The pointer aliases @ref g_app_fault, so it is invalidated by the next
 *       @ref app_fault_save or @ref app_fault_clear and must never be freed.
 * @warning The bytes are only known to be NUL-terminated when
 *          @ref app_fault_is_valid is true; before that they are whatever the
 *          no-init RAM held.
 * @see app_fault_task
 */
const char *app_fault_file(void) { return g_app_fault.file; }

/**
 * @brief Write a complete fault record for the next boot.
 * @details The stores follow a fixed order:
 *          -# @c magic is set to 0, so any previous record stops being valid
 *             before its payload is touched.
 *          -# @c __DMB orders that invalidation ahead of the payload stores.
 *          -# The reason, the CFSR/HFSR/MMFAR/BFAR snapshots, the stacked
 *             PC/LR/xPSR, the reason-specific @p line and the current boot
 *             identifier are stored.
 *          -# fault_copy() writes the task name and the file basename.
 *          -# A second @c __DMB, then APP_FAULT_MAGIC, then @c __DSB publish the
 *             record and order it ahead of the reset the caller performs.
 *
 *          A reset between steps 1 and 5 leaves an invalid record, which is
 *          the intended outcome.  The function writes the record only; it does
 *          not reset the MCU.
 *
 * @param[in] reason Fault reason; a value above APP_FAULT_STARTUP still clears
 *                   the record but makes it fail @ref app_fault_is_valid.
 * @param[in] cfsr   @c SCB->CFSR snapshot, or 0 for software faults.
 * @param[in] hfsr   @c SCB->HFSR snapshot, or 0 for software faults.
 * @param[in] mmfar  @c SCB->MMFAR snapshot; valid only when @c MMARVALID is set
 *                   in @p cfsr.
 * @param[in] bfar   @c SCB->BFAR snapshot; valid only when @c BFARVALID is set
 *                   in @p cfsr.
 * @param[in] pc     Stacked PC at the fault, or 0 when unavailable.
 * @param[in] lr     Stacked LR at the fault, or 0 when unavailable.
 * @param[in] psr    Stacked xPSR at the fault, or 0 when unavailable.
 * @param[in] task   NUL-terminated task name, or NULL.  Truncated to
 *                   @c APP_FAULT_TASK_LEN - 1 = 15 characters; the pointer is
 *                   not retained.
 * @param[in] file   NUL-terminated file path or label, or NULL.  Only the
 *                   basename is kept, truncated to
 *                   @c APP_FAULT_FILE_LEN - 1 = 31 characters; the pointer is
 *                   not retained.
 * @param[in] line   Source line, or a reason-specific value such as the free
 *                   heap size in bytes for APP_FAULT_MALLOC_FAILED.
 *
 * @pre  @ref app_fault_boot_start has run, so @c app_boot_id identifies this
 *       boot.
 * @post @ref g_app_fault is valid and stamped with the current boot
 *       identifier, unless the MCU reset before the final store.
 *
 * @note Does not mask interrupts, block, allocate or call into the HAL; it
 *       touches DTCM only, so it is equally safe with the caches on and off,
 *       and DTCM is not cached, so no cache maintenance is needed.
 * @warning Not reentrant.  The module assumes a single writer: the exception
 *          wrappers enter with the faulting exception active, and a
 *          task-level caller must be sure that no other context can save at
 *          the same time, otherwise a mixed record can be published.
 * @see app_fault_fatal
 * @see app_fault_is_valid
 */
void app_fault_save(app_fault_reason_t reason, uint32_t cfsr, uint32_t hfsr, uint32_t mmfar, uint32_t bfar, uint32_t pc, uint32_t lr, uint32_t psr, const char *task, const char *file, uint32_t line)
{
    /* Invalidate first so a reset in the middle of the copy is not mistaken
     * for a complete record. */
    g_app_fault.magic = 0U;
    __DMB();

    g_app_fault.reason  = (uint32_t)reason;
    g_app_fault.cfsr    = cfsr;
    g_app_fault.hfsr    = hfsr;
    g_app_fault.mmfar   = mmfar;
    g_app_fault.bfar    = bfar;
    g_app_fault.pc      = pc;
    g_app_fault.lr      = lr;
    g_app_fault.psr     = psr;
    g_app_fault.line    = line;
    g_app_fault.boot_id = app_boot_id;
    fault_copy(g_app_fault.task, sizeof(g_app_fault.task), task);
    fault_copy(g_app_fault.file, sizeof(g_app_fault.file), file);

    __DMB();
    g_app_fault.magic = APP_FAULT_MAGIC;
    __DSB();
}

/**
 * @brief Invalidate the stored fault record.
 * @details Clears only the magic word and issues a @c __DMB, so
 *          @ref app_fault_is_valid reports false immediately.  The payload
 *          fields and @c app_boot_id are left unchanged; they are simply no
 *          longer reported.  The Comms task calls it from the HWLINK ClearFault
 *          handler, together with @ref app_error_clear_fault and
 *          @ref led_post_clear_error, so Status and LD3 agree with the record.
 * @post No fault is reported in Status until the next @ref app_fault_save.
 * @note The separate @ref APP_ERROR_FAULT latch and the LD3 indication are not
 *       touched here; ClearFault clears them separately.
 * @warning Single-writer assumption: do not clear while another context may
 *          be saving, or the published record can be left inconsistent with
 *          the latch.
 * @see app_fault_save
 * @see app_fault_is_valid
 */
void app_fault_clear(void)
{
    g_app_fault.magic = 0U;
    __DMB();
}

/**
 * @brief Blocking delay built on the DWT cycle counter.
 * @details Enables the trace unit through @c CoreDebug->DEMCR @c TRCENA and the
 *          cycle counter through @c DWT->CTRL @c CYCCNTENA, then waits until
 *          @c SystemCoreClock / 1000 * @p ms cycles have elapsed, measured as a
 *          wrapping difference so a CYCCNT overflow during the wait is
 *          harmless.  A cycle counter needs no calibration, so the delay is
 *          the same at 16 MHz and at 216 MHz, which matters because the fatal
 *          path can run before or after the PLL is configured.
 *
 *          A debugger can lock the trace unit, in which case CYCCNT never
 *          advances and the cycle wait would spin for the full count.  A short
 *          probe loop detects that once, and the function then falls back to a
 *          bounded NOP loop of @p ms * 20000 iterations, so the watchdog is
 *          not given time to fire and truncate the LED pattern.
 *
 * @param[in] ms Delay length in milliseconds.
 *
 * @note Best effort, not a calibrated timer: the fallback loop depends on the
 *       core clock, the flash wait states and the generated code, so it only
 *       approximates @p ms.  The caller uses it for the LED pattern, where an
 *       exact interval is irrelevant.
 * @note Busy-waits and leaves the DWT enabled; the only caller has already
 *       disabled interrupts and never returns.
 * @warning No parameter validation and no overflow check on @p ms * 20000.
 *          Blocks for the whole interval, so it must not be used in a task.
 * @see app_fault_show_and_reset
 */
static void fault_delay_ms(uint32_t ms)
{
    /* Use the DWT cycle counter when it is available; this works before the
     * HAL and gives the same delay at 16 MHz and 216 MHz.  Fall back to a
     * calibrated loop if the trace unit is locked. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* If the trace unit is locked, CYCCNT never advances; detect that once
     * instead of spinning for cycles*4 iterations (the IWDG would fire
     * first). */
    uint32_t probe = DWT->CYCCNT;
    for(volatile uint32_t i = 0U; i < 100U; i++)
    {
        __NOP();
    }
    if(DWT->CYCCNT == probe)
    {
        for(volatile uint32_t i = 0U; i < (ms * 20000U); i++)
        {
            __NOP();
        }
        return;
    }

    uint32_t cycles = (SystemCoreClock / 1000U) * ms;
    uint32_t start  = DWT->CYCCNT;
    while((DWT->CYCCNT - start) < cycles)
    {
        __NOP();
    }
}

/**
 * @brief Drive LD3 on or off with a single register write.
 * @details Writes @c GPIOB->BSRR: the low half sets the pin, the high half
 *          resets it, so the pin changes without a read-modify-write of
 *          @c ODR and without disturbing the other port B pins.
 * @param[in] on true lights LD3, false extinguishes it.
 * @pre  The GPIOB clock is enabled and PB14 is configured as an output;
 *       @ref app_fault_show_and_reset establishes both before the first call.
 * @note The port and the pin are hard-coded as GPIOB and APP_LED3_PIN, like
 *       the rest of the fatal path, so moving LD3 to another port needs a
 *       matching edit here; see the warning on @ref APP_LED3_PORT.
 * @see app_fault_show_and_reset
 */
static void fault_led3(bool on) { GPIOB->BSRR = on ? APP_LED3_PIN : ((uint32_t)APP_LED3_PIN << 16U); }

/**
 * @brief Flash the fault pattern on LD3 and reset the MCU.
 * @details The sequence, in order:
 *          -# @c __disable_irq sets PRIMASK so nothing can preempt or re-enter
 *             the path.  PRIMASK is not restored because the function does not
 *             return.
 *          -# The reload key is written to `IWDG->KR` immediately after the
 *             interrupts are masked, so the roughly 2.2 s LED pattern starts
 *             with a full 4 s watchdog window instead of the remainder of an
 *             inherited period.  The write is harmless when the watchdog is
 *             stopped, and it avoids a dependency on @ref iwdg.c.
 *          -# @c RCC->AHB1ENR @c GPIOBEN is set, followed by @c __DSB, so the
 *             port is clocked before the first GPIO register write.
 *          -# @c GPIOB->MODER is read, the two bits of pin 14 are replaced with
 *             the general-purpose output code 01b, the register is written
 *             back and a @c __DSB follows.
 *          -# Two rounds of three flashes are emitted, each flash 100 ms on and
 *             100 ms off, with a 500 ms pause after each round; about 2.2 s in
 *             total.
 *          -# @c NVIC_SystemReset requests the reset; the trailing endless
 *             @c __NOP loop exists only for the case where the request is
 *             ignored, and keeps the function @c noreturn for the compiler.
 *
 *          No record is written here: a caller that has a reason to keep calls
 *          @ref app_fault_save first.
 * @pre  Callable at any point after reset, with or without the HAL, the clock
 *       tree, the scheduler or the GPIO driver initialised, from task, ISR or
 *       fault context.
 * @post The MCU is reset; the function never returns.
 * @warning Blocks for about 2.2 s with interrupts disabled.  The reload at
 *          entry gives the pattern a full 4 s IWDG window; a watchdog
 *          configured tighter still truncates it, but the reset happens.  The
 *          GPIOB clock stays enabled and PB14 keeps its new mode until the
 *          reset.
 * @note The pattern is diagnostic only, the record is the authoritative report
 *       (DGEN-004, LED-005).  Nothing is logged or flushed here, because the
 *       console may not exist yet and flushing could block.
 * @see app_fault_fatal
 */
void app_fault_show_and_reset(void)
{
    __disable_irq();

    /* Defer an inherited IWDG reset so the whole pattern fits its window.
     * The key write is harmless when the watchdog is stopped. */
    IWDG->KR = IWDG_KEY_RELOAD;

    /* Register-level LED set-up: DGEN-004 must work from any context. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    __DSB();
    uint32_t moder = GPIOB->MODER;
    moder &= ~(3UL << (14U * 2U));
    moder |= (1UL << (14U * 2U));
    GPIOB->MODER = moder;
    __DSB();

    /* Three short flashes, a pause, repeat; roughly 2 s in total. */
    for(uint32_t cycle = 0U; cycle < 2U; cycle++)
    {
        for(uint32_t flash = 0U; flash < 3U; flash++)
        {
            fault_led3(true);
            fault_delay_ms(100U);
            fault_led3(false);
            fault_delay_ms(100U);
        }
        fault_delay_ms(500U);
    }

    NVIC_SystemReset();
    for(;;)
    {
        __NOP();
    }
}

/**
 * @brief Record a fatal software error and reset.
 * @details Saves @p reason with zeroed CFSR, HFSR, MMFAR, BFAR, PC, LR and xPSR,
 *          because only the reason, the task, the file and the line are
 *          meaningful for a software fault, then calls
 *          @ref app_fault_show_and_reset.  It is the sink of the
 *          @c configASSERT path, the FreeRTOS stack-overflow and malloc hooks
 *          (DRT-002) and the start-up failures in main() and the clock set-up.
 * @param[in] reason Reason code, normally APP_FAULT_ASSERT,
 *                   APP_FAULT_STACK_OVERFLOW, APP_FAULT_MALLOC_FAILED or
 *                   APP_FAULT_STARTUP.
 * @param[in] task   NUL-terminated task or stage name, or NULL; truncated to
 *                   15 characters.
 * @param[in] file   NUL-terminated file path or label, or NULL; only the
 *                   basename is kept.
 * @param[in] line   Source line, or a reason-specific value.
 * @post The record is saved and the MCU resets.
 * @warning Does not return, does not flush a log and does not clear
 *          @ref APP_ERROR_FAULT; the next boot reports the record through
 *          @ref app_fault_is_valid.
 * @see app_fault_save
 * @see app_fault_show_and_reset
 */
void app_fault_fatal(app_fault_reason_t reason, const char *task, const char *file, uint32_t line)
{
    app_fault_save(reason, 0U, 0U, 0U, 0U, 0U, 0U, 0U, task, file, line);
    app_fault_show_and_reset();
}

/* Called from the naked exception wrappers with r0 = stacked frame and
 * r1 = app_fault_reason_t. */
/**
 * @brief Common C body of the four configurable fault handlers.
 * @details The naked wrappers below branch here with @c r0 pointing at the
 *          stacked exception frame and @c r1 holding the reason.  The function
 *          snapshots @c SCB->CFSR, @c SCB->HFSR, @c SCB->MMFAR and
 *          @c SCB->BFAR, extracts the stacked PC, LR and xPSR from words 6, 5
 *          and 7 of the basic frame (r0-r3, r12, lr, pc, xpsr), saves
 *          everything with a NULL task and file, and resets through
 *          @ref app_fault_show_and_reset.
 *
 *          The frame is probed before it is read: a stack overflow or a
 *          corrupted MSP/PSP can leave @p frame outside internal RAM, and
 *          dereferencing such a pointer would double-fault into lockup and
 *          lose the reason.  The frame is therefore used only when the pointer
 *          is word aligned and the whole eight-word basic frame lies inside
 *          0x20000000..0x2007FFFF, which covers DTCM, SRAM1 and SRAM2.  If the
 *          probe fails, PC, LR and xPSR are recorded as 0 while the reason and
 *          the fault status registers are still saved.
 * @param[in] frame  Pointer to the stacked exception frame taken from MSP or
 *                   PSP, as passed by the wrapper.
 * @param[in] reason One of @ref app_fault_reason_t, moved into r1 by the
 *                   wrapper.
 * @pre  Called in handler mode with the faulting exception still active.
 * @post The record is saved and the MCU resets; the function does not return.
 * @note The frame is read but never written, so a debugger that breaks before
 *       the reset still sees the faulting stack untouched.
 * @warning Not reentrant, and a fault inside this function escalates to
 *          lockup; keep it free of any call that can fault on a bad stack.
 * @see app_fault_save
 * @see app_fault_show_and_reset
 */
__attribute__((used, noreturn)) void app_fault_handler_c(uint32_t *frame, uint32_t reason)
{
    /* A stack-overflow fault can leave MSP/PSP outside RAM; dereferencing
     * such a frame double-faults into lockup.  Only read the stacked PC/LR
     * when the whole exception frame lies in internal RAM. */
    uint32_t  pc = 0U, lr = 0U, psr = 0U;
    uintptr_t base = (uintptr_t)frame;
    if(base >= 0x20000000UL && base <= 0x20080000UL && (base & 3U) == 0U && (base + (8U * 4U)) <= 0x20080000UL)
    {
        pc  = frame[6];
        lr  = frame[5];
        psr = frame[7];
    }
    app_fault_save((app_fault_reason_t)reason, SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR, pc, lr, psr, NULL, NULL, 0UL);
    app_fault_show_and_reset();
}

/**
 * @brief Define a naked exception entry that forwards to
 *        @c app_fault_handler_c.
 * @details The generated function contains a single inline assembler block.  It
 *          tests bit 2 of @c LR, the EXC_RETURN bit that selects the process
 *          stack, uses an @c ITE block to load either MSP or PSP into @c r0,
 *          moves @p reason into @c r1 and branches to
 *          @ref app_fault_handler_c.  The function is
 *          @c __attribute__((naked)) so the compiler emits no prologue and no
 *          register spill that could touch the corrupted stack, and
 *          @c noreturn because the branch target never returns.  @c r0, @c r1
 *          and memory are declared clobbered.
 * @param[in] name   C identifier of the handler; it must match the vector
 *                   table entry in the start-up file.
 * @param[in] reason Compile-time @ref app_fault_reason_t constant, inserted
 *                   with the @c "I" constraint, so it must fit the immediate
 *                   form of @c movs.
 * @note The macro declares no prototype; the start-up file already declares
 *       the weak aliases that these strong definitions replace.
 * @warning Keep the body instruction-only at file scope.  GCC does not support
 *          C statements in a naked function, and a prologue would read the
 *          stack that just faulted.
 * @see app_fault_handler_c
 */
#define APP_DEFINE_FAULT_HANDLER(name, reason)                                                                                                                                                                                                                                                             \
    __attribute__((naked, noreturn)) void name(void)                                                                                                                                                                                                                                                       \
    {                                                                                                                                                                                                                                                                                                      \
        __asm volatile("tst lr, #4\n\t"                                                                                                                                                                                                                                                                    \
                       "ite eq\n\t"                                                                                                                                                                                                                                                                        \
                       "mrseq r0, msp\n\t"                                                                                                                                                                                                                                                                 \
                       "mrsne r0, psp\n\t"                                                                                                                                                                                                                                                                 \
                       "movs r1, %0\n\t"                                                                                                                                                                                                                                                                   \
                       "b app_fault_handler_c\n\t"                                                                                                                                                                                                                                                         \
                       :                                                                                                                                                                                                                                                                                   \
                       : "I"(reason)                                                                                                                                                                                                                                                                       \
                       : "r0", "r1", "memory");                                                                                                                                                                                                                                                            \
    }

/**
 * @brief Strong definitions of the four configurable fault vectors.
 * @details Each expansion overrides the weak default in the start-up file, so
 *          the reason recorded in @ref g_app_fault names the specific vector
 *          instead of a generic HardFault.  main() enables MemManage, BusFault
 *          and UsageFault through @c SCB->SHCSR before the application starts;
 *          a fault taken earlier, or an escalated fault, still lands in
 *          HardFault_Handler and keeps the generic reason.
 * @note The handlers are handler-mode only and never return; they leave the
 *       faulting exception active while the record is saved, which is why the
 *       save path must not depend on the scheduler or on interrupts.
 * @see APP_DEFINE_FAULT_HANDLER
 * @see app_fault_handler_c
 */
APP_DEFINE_FAULT_HANDLER(HardFault_Handler, APP_FAULT_HARDFAULT)
APP_DEFINE_FAULT_HANDLER(MemManage_Handler, APP_FAULT_MEMMANAGE)
APP_DEFINE_FAULT_HANDLER(BusFault_Handler, APP_FAULT_BUSFAULT)
APP_DEFINE_FAULT_HANDLER(UsageFault_Handler, APP_FAULT_USAGEFAULT)
