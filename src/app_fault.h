/**
 * @file    app_fault.h
 * @brief   No-init fault record and fatal reset paths.
 * @details The module keeps one @ref app_fault_record_t in the reserved
 *          @c .noinit window at the top of DTCM, so a fault recorded just
 *          before a reset is still readable after the reset and can be
 *          reported in Status and shown on LD3 (DGEN-004, LED-005).
 *
 *          Two rules govern the lifetime of the record:
 *          - The C start-up code clears only @c .bss and BOOT never writes
 *            the 1 KB no-init window, so the bytes survive every system
 *            reset, including a watchdog or fault reset (see
 *            APP_NOINIT_ADDR and APP_NOINIT_SIZE in @ref boot_contract.h).
 *          - The bytes are @em not initialised at power-on.  A record is
 *            meaningful only when @ref app_fault_is_valid reports true.  The
 *            magic word is written last, after the payload, so a reset in the
 *            middle of @ref app_fault_save leaves an invalid record.
 *
 *          A retained boot identifier is incremented once per boot, which
 *          lets a reader tell a fault from the running boot apart from one
 *          inherited from the previous boot.  Fatal paths save the record,
 *          flash the pattern on LD3 and reset; they work before the HAL and
 *          with interrupts disabled, so they touch hardware registers only.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     DGEN-004, DRT-002, LED-005
 * @ingroup app_fault
 */

#ifndef APP_FAULT_H
#define APP_FAULT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup app_fault No-init fault record and fatal paths
 * @brief Retained fault record, boot identification and fatal reset helpers.
 * @details The group exposes the record object, the validity and boot
 *          predicates, the reason-to-text mapping and the fatal save/reset
 *          entry points used by the exception wrappers, the FreeRTOS hooks
 *          and start-up code.
 * @{
 */

/**
 * @brief Validity marker stored in @ref app_fault_record_t.magic.
 * @details Spells "FAUL" when read as a big-endian word, while the field is
 *          stored little-endian in RAM.  It is written last by
 *          app_fault_save() and cleared first by app_fault_clear(), so it
 *          never marks a partially written record.
 */
#define APP_FAULT_MAGIC 0x4641554CUL

/**
 * @brief Capacity of @ref app_fault_record_t.task in bytes, including the
 *        NUL terminator.
 * @details fault_copy() keeps at most @c APP_FAULT_TASK_LEN - 1 = 15
 *          characters, so a longer FreeRTOS task name is truncated.
 */
#define APP_FAULT_TASK_LEN 16U

/**
 * @brief Capacity of @ref app_fault_record_t.file in bytes, including the
 *        NUL terminator.
 * @details Only the basename of the @c __FILE__ path is kept, at most
 *          @c APP_FAULT_FILE_LEN - 1 = 31 characters.
 */
#define APP_FAULT_FILE_LEN 32U

/**
 * @brief Reason recorded in the no-init fault record.
 * @details The enumerator values are stored verbatim in
 *          @ref app_fault_record_t.reason and are used as indexes into the
 *          private @c fault_reason_names table in @c app_fault.c.
 * @note Do not reorder, insert or renumber enumerators without updating that
 *       table; app_fault_reason_text() would otherwise return the wrong text.
 *       app_fault_is_valid() accepts only values from @ref APP_FAULT_NONE to
 *       @ref APP_FAULT_STARTUP.
 */
typedef enum
{
    APP_FAULT_NONE = 0,       /**< No fault recorded; never written by a fatal path. */
    APP_FAULT_HARDFAULT,      /**< HardFault_Handler, including escalated faults. */
    APP_FAULT_MEMMANAGE,      /**< MemManage_Handler, normally an MPU violation. */
    APP_FAULT_BUSFAULT,       /**< BusFault_Handler, normally a bad peripheral access. */
    APP_FAULT_USAGEFAULT,     /**< UsageFault_Handler, e.g. undefined instruction. */
    APP_FAULT_ASSERT,         /**< configASSERT failure (DRT-002). */
    APP_FAULT_STACK_OVERFLOW, /**< FreeRTOS stack overflow hook (DRT-002). */
    APP_FAULT_MALLOC_FAILED,  /**< FreeRTOS malloc hook; line holds free bytes. */
    APP_FAULT_STARTUP         /**< Start-up check failed; task/file name stage. */
} app_fault_reason_t;

/**
 * @brief Persistent fault record stored in the @c .noinit section.
 * @details One instance, @ref g_app_fault, is written by app_fault_save()
 *          immediately before a reset and read after the next boot by main(),
 *          the health task, the Comms task and the debugger.  All register
 *          fields are snapshots taken at save time, not live hardware state.
 * @note The layout is part of the debugger and Status contract: keep the
 *       field order and the fixed-size character arrays.  The total size,
 *       together with the retained boot counter, is checked against
 *       APP_NOINIT_SIZE by a compile-time assertion in @c app_fault.c (A-9).
 * @warning The object is not initialised at power-on, so every field must be
 *          ignored until app_fault_is_valid() returns true.
 */
typedef struct
{
    uint32_t magic;                    /**< APP_FAULT_MAGIC when valid. */
    uint32_t reason;                   /**< One of @ref app_fault_reason_t. */
    uint32_t cfsr;                     /**< SCB->CFSR snapshot; 0 for software faults. */
    uint32_t hfsr;                     /**< SCB->HFSR snapshot; 0 for software faults. */
    uint32_t mmfar;                    /**< SCB->MMFAR snapshot; check MMARVALID. */
    uint32_t bfar;                     /**< SCB->BFAR snapshot; check BFARVALID. */
    uint32_t pc;                       /**< Stacked PC at fault; 0 if unavailable. */
    uint32_t lr;                       /**< Stacked LR at fault; 0 if unavailable. */
    uint32_t psr;                      /**< Stacked xPSR at fault; 0 if unavailable. */
    uint32_t line;                     /**< Source line; free heap bytes on malloc fail. */
    uint32_t boot_id;                  /**< Retained boot counter value at save time. */
    char     task[APP_FAULT_TASK_LEN]; /**< Task name, NUL-terminated, may be empty. */
    char     file[APP_FAULT_FILE_LEN]; /**< Source basename, NUL-terminated, may be empty. */
} app_fault_record_t;

/**
 * @brief Persistent fault record shared by the whole application.
 * @details Placed in @c .noinit (NOLOAD) inside the 1 KB NOINIT region at
 *          APP_NOINIT_ADDR, which start-up code does not clear and BOOT never
 *          writes, so the content survives a system reset (DGEN-004).
 *          Written by app_fault_save() and app_fault_clear() in
 *          @c app_fault.c; read by main(), the health task, the Comms task
 *          for Status and the debugger.
 * @note Do not initialise this object, move it out of @c .noinit or enlarge it
 *       past APP_NOINIT_SIZE; the reset-survival contract depends on the
 *       linker placement and on the A-9 static assertion.
 * @warning Treat as read-only outside @c app_fault.c.  A direct write can
 *          leave a valid magic word with a partial payload, which is then
 *          reported as a fault.  Access is not locked: the module assumes a
 *          single writer (the fault path or ClearFault).
 */
extern app_fault_record_t g_app_fault;

/**
 * @brief Increment the retained boot identifier for the current boot.
 * @details Must run early in main(), before anything can record a fault.
 *          The identifier lives in @c .noinit next to the record, so it is
 *          undefined after power-on and preserved across resets.  It is
 *          incremented once per boot; the wrap value 0 is replaced by 1 so
 *          the current/previous comparisons stay unambiguous.
 * @pre The @c .noinit window has not been cleared by start-up code.
 * @post The retained counter differs from its value in the previous boot.
 * @note Call exactly once per boot, before the scheduler starts.  It is not
 *       atomic and not reentrant, but main() is the only caller.  Because the
 *       section is not initialised, the first power-on value is arbitrary.
 * @see app_fault_is_current_boot
 * @see app_fault_is_previous_boot
 */
void app_fault_boot_start(void);

/**
 * @brief Report whether @ref g_app_fault holds a complete record.
 * @details Checks only the magic word and that the stored reason is in the
 *          accepted range 0..APP_FAULT_STARTUP.  It deliberately does not
 *          compare @ref app_fault_record_t.boot_id, so a valid record from any
 *          earlier boot qualifies.
 * @return true when @ref g_app_fault contains a complete, plausible record.
 * @retval true  The magic word is APP_FAULT_MAGIC and @c reason is a known
 *               @ref app_fault_reason_t value.
 * @retval false The record was never written, was cleared, or holds an
 *               out-of-range reason.
 * @note Reads only, so it is safe from tasks, ISRs and the fault path.  The
 *       record has no checksum, so arbitrary power-on DTCM content could in
 *       principle pass the test; the odds are negligible and the boot-id
 *       predicates narrow the answer further.
 * @see app_fault_is_current_boot
 * @see app_fault_is_previous_boot
 */
bool app_fault_is_valid(void);

/**
 * @brief Report whether the valid record was written during this boot.
 * @details True when the record is valid and
 *          @ref app_fault_record_t.boot_id equals the retained counter for
 *          the running boot.  main() does not use it; the health check does,
 *          because a fault in the current trial boot must fail the check.
 * @return true when a valid record belongs to the running boot.
 * @retval true  @ref app_fault_is_valid is true and @c boot_id matches the
 *               current counter.
 * @retval false No valid record, or the record predates this boot.
 * @pre app_fault_boot_start() has run for the current boot.
 * @note Reads only; safe from tasks and ISRs.
 * @see app_fault_is_previous_boot
 */
bool app_fault_is_current_boot(void);

/**
 * @brief Report whether the valid record was written during the previous boot.
 * @details Computes the previous counter value as @c boot_id - 1, mapping 0 to
 *          0xFFFFFFFF so the test still works across the wrap that
 *          app_fault_boot_start() performs.  The health check uses it, while
 *          a pending trial is active (@c trial_count > 1), to fail the trial
 *          when the trial boot faulted.
 * @return true when a valid record belongs to the boot before this one.
 * @retval true  @ref app_fault_is_valid is true and @c boot_id equals the
 *               previous counter value.
 * @retval false No valid record, the record is from the current or an older
 *               boot, or no boot start has run.
 * @pre app_fault_boot_start() has run for the current boot.
 * @note Reads only; safe from tasks and ISRs.
 * @see app_fault_is_current_boot
 */
bool app_fault_is_previous_boot(void);

/**
 * @brief Map a fault reason to a short, stable ASCII name.
 * @details Returns an entry of the private @c fault_reason_names table in
 *          @c app_fault.c.  The names are the wire vocabulary used by Status,
 *          so they must not change without a host-side update.
 * @param[in] reason Reason code to translate.
 * @return Pointer to a static NUL-terminated string; never NULL.
 * @retval "unknown" @p reason is outside the table (greater than
 *         APP_FAULT_STARTUP).
 * @note Performs no allocation or copy and returns storage owned by the
 *       module, so no caller may free or modify it.  Safe from any context.
 * @see app_fault_reason_t
 */
const char *app_fault_reason_text(app_fault_reason_t reason);

/**
 * @brief Access the task name stored in the fault record.
 * @details Returns @ref app_fault_record_t.task, which is empty when the
 *          record was saved without a task name, for example from an
 *          exception wrapper.
 * @return Pointer to the NUL-terminated task name inside @ref g_app_fault.
 * @note The pointer aliases the no-init record: it stays valid until the next
 *       app_fault_save() or app_fault_clear(), and must not be freed.  It may
 *       be empty but is never NULL.  Reads only; safe from any context.
 * @see app_fault_file
 */
const char *app_fault_task(void);

/**
 * @brief Access the source-file basename stored in the fault record.
 * @details Returns @ref app_fault_record_t.file.  The save path strips the
 *          directory so the 32-byte field identifies the file (DGEN-004).
 * @return Pointer to the NUL-terminated basename inside @ref g_app_fault.
 * @note The pointer aliases the no-init record: it stays valid until the next
 *       app_fault_save() or app_fault_clear(), and must not be freed.  It may
 *       be empty but is never NULL.  Reads only; safe from any context.
 * @see app_fault_task
 */
const char *app_fault_file(void);

/**
 * @brief Save a fault record for the next boot.
 * @details Invalidates the record first (magic cleared, then a DMB), stores
 *          the reason, the register snapshots and the current boot identifier,
 *          copies the task and file strings, then publishes the record by
 *          writing APP_FAULT_MAGIC followed by a DSB.  A reset at any point
 *          before that final store therefore leaves an invalid record instead
 *          of a half-written one.  The function does not reset the MCU.
 *
 * @param[in] reason Fault reason; values above APP_FAULT_STARTUP make the
 *                   record fail app_fault_is_valid().
 * @param[in] cfsr   SCB->CFSR snapshot, or 0 when not applicable.
 * @param[in] hfsr   SCB->HFSR snapshot, or 0 when not applicable.
 * @param[in] mmfar  SCB->MMFAR snapshot, or 0 when not applicable.
 * @param[in] bfar   SCB->BFAR snapshot, or 0 when not applicable.
 * @param[in] pc     Stacked PC at the fault, or 0 when unknown.
 * @param[in] lr     Stacked LR at the fault, or 0 when unknown.
 * @param[in] psr    Stacked xPSR at the fault, or 0 when unknown.
 * @param[in] task   NUL-terminated task name, or NULL.  Truncated to 15
 *                   characters; the pointer is not retained.
 * @param[in] file   NUL-terminated file path or label, or NULL.  Only the
 *                   basename is kept, truncated to 31 characters; the pointer
 *                   is not retained.
 * @param[in] line   Source line, or a reason-specific value such as the free
 *                   heap size for APP_FAULT_MALLOC_FAILED.
 *
 * @post @ref g_app_fault holds a valid record stamped with the current boot
 *       identifier, unless a reset intervened before the final store.
 * @note Does not mask interrupts, block or allocate.  It relies on having a
 *       single writer: the exception wrappers enter with faults masked, while
 *       a task-level caller (configASSERT or start-up) is safe only when no
 *       other context can save at the same time.
 * @warning Not reentrant.  Do not call it from a context that can preempt an
 *          in-progress save; that could publish a mixed record.
 * @see app_fault_fatal
 * @see app_fault_is_valid
 */
void app_fault_save(app_fault_reason_t reason, uint32_t cfsr, uint32_t hfsr, uint32_t mmfar, uint32_t bfar, uint32_t pc, uint32_t lr, uint32_t psr, const char *task, const char *file, uint32_t line);

/**
 * @brief Show the fault pattern on LD3 for about two seconds and reset.
 * @details Disables interrupts, enables the GPIOB clock, configures LD3
 *          (PB14, APP_LED3_PIN) as a push-pull output through direct register
 *          accesses and emits three short flashes, a pause, and repeats.
 *          This works before the HAL and before the GPIO driver, so it can run
 *          from any fault or assert context (DGEN-004).  The delay uses the
 *          DWT cycle counter when it is available and falls back to a busy
 *          loop when the trace unit is locked.
 * @pre Callable at any point after reset, with or without the HAL, clock tree
 *      or scheduler initialised.
 * @post The MCU is reset through NVIC_SystemReset(); the function never
 *       returns and no record is written by itself.
 * @warning No return.  Call app_fault_save() first if the reason must survive
 *          the reset.  The pattern is diagnostic only: roughly 2.2 s of
 *          blocking, interrupt-disabled execution.  This fits inside the 4 s
 *          IWDG timeout, but a watchdog that expires sooner truncates the
 *          pattern (the reset still happens).
 * @see app_fault_fatal
 */
void app_fault_show_and_reset(void) __attribute__((noreturn));

/**
 * @brief Record a fatal software error and reset.
 * @details Saves @p reason with zeroed fault status and stacked registers,
 *          because only the task, file and line are meaningful for a software
 *          error, then calls app_fault_show_and_reset().  Used by the
 *          configASSERT path, the FreeRTOS stack-overflow and malloc hooks
 *          (DRT-002) and by start-up failures in main() and the clock set-up.
 *
 * @param[in] reason Fault reason; normally APP_FAULT_ASSERT,
 *                   APP_FAULT_STACK_OVERFLOW, APP_FAULT_MALLOC_FAILED or
 *                   APP_FAULT_STARTUP.
 * @param[in] task   NUL-terminated task or stage name, or NULL.  Truncated to
 *                   15 characters.
 * @param[in] file   NUL-terminated file path or label, or NULL.  Only the
 *                   basename is stored.
 * @param[in] line   Source line, or a reason-specific value.
 *
 * @post The record is saved and the MCU resets.
 * @warning Does not return and does not clear @ref APP_ERROR_FAULT; the next
 *          boot reports the record through app_fault_is_valid().
 * @see app_fault_save
 * @see app_fault_show_and_reset
 */
void app_fault_fatal(app_fault_reason_t reason, const char *task, const char *file, uint32_t line) __attribute__((noreturn));

/**
 * @brief Invalidate the stored fault record.
 * @details Clears only the magic word, followed by a DMB, so
 *          app_fault_is_valid() reports false immediately.  The payload
 *          fields and the retained boot identifier are left unchanged; they
 *          are simply no longer reported.  Called by the HWLINK ClearFault
 *          handler.
 * @post No fault is reported in Status until the next app_fault_save().
 * @note The separate @ref APP_ERROR_FAULT latch and the LD3 indication are
 *       not touched here; ClearFault clears them through app_error_clear_fault
 *       and led_post_clear_error.  Reads and writes only, no blocking.
 * @warning Do not call while a save may be in progress; the module assumes a
 *          single writer.
 * @see app_fault_is_valid
 * @see app_error_clear_fault
 */
void app_fault_clear(void);

/** @} */

#endif /* APP_FAULT_H */
