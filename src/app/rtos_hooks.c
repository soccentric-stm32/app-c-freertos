/**
 * @file    rtos_hooks.c
 * @brief   FreeRTOS error hooks for assert, stack overflow and malloc failure.
 * @details Implements the kernel callbacks enabled by @c configASSERT,
 *          @c configCHECK_FOR_STACK_OVERFLOW = 2 and
 *          @c configUSE_MALLOC_FAILED_HOOK = 1 in @ref FreeRTOSConfig.h
 *          (DRT-002).  Each hook funnels into app_fault_fatal(), which writes
 *          the reason, the task name and a short label into the no-init fault
 *          record (DGEN-004), flashes the fault pattern on LD3 and resets the
 *          MCU.  The record therefore survives the reset and is reported in
 *          Status by the next boot through @ref app_fault_is_valid and
 *          @ref APP_ERROR_FAULT.
 *
 *          The hooks are deliberately minimal: they copy no buffers, take no
 *          locks, allocate nothing and never return, so they are safe in the
 *          context that detects the error.  Because an assertion or a failed
 *          allocation can happen before the scheduler runs, the task name is
 *          resolved through current_task_name(), which falls back to the
 *          literal "startup".  No register snapshot is taken here: the
 *          software fault path stores zeros in the @c cfsr, @c hfsr,
 *          @c mmfar, @c bfar, @c pc, @c lr and @c psr fields of
 *          @ref app_fault_record_t and puts a reason-specific value in
 *          @c line.
 *
 *          The file defines no macros and no writable state; the only object
 *          is the read-only task-name fallback used by current_task_name().
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DRT-002 (assert, stack overflow, malloc failed), DGEN-004
 * @ingroup app_fault
 */

#include "FreeRTOS.h"
#include "task.h"
#include "services/app_fault.h"

/**
 * @brief Resolve the task or stage name to record for the failing context.
 * @details Returns the FreeRTOS name of the running task when the scheduler
 *          is running, and the string literal "startup" otherwise.  The
 *          scheduler-state test keeps the hooks usable when the failure
 *          happens before @c vTaskStartScheduler(), for example inside a task
 *          creation that fails in main(), where no current TCB exists yet.
 *
 * @return Pointer to a NUL-terminated name; never NULL.
 * @retval "startup" The scheduler is not in the running state.
 * @retval other     Name of the running task, owned by its TCB.
 *
 * @note The name is not copied here; app_fault_fatal() truncates it to
 *       @c APP_FAULT_TASK_LEN - 1 = 15 characters, so a longer task name is
 *       shortened in the record.
 * @note Reads only, does not block and does not allocate, so it is safe from
 *       task, ISR and start-up context.
 * @warning Only taskSCHEDULER_RUNNING is accepted, so a failure raised while
 *          the scheduler is suspended also reports "startup" even though a
 *          task is current.
 * @see vApplicationStackOverflowHook
 * @see vApplicationMallocFailedHook
 * @see app_fault_assert
 */
static const char *current_task_name(void)
{
    if(xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        return pcTaskGetName(NULL);
    }
    return "startup";
}

/**
 * @brief Record a failed @c configASSERT and reset (DRT-002).
 * @details Assertion sink for the kernel macro in @ref FreeRTOSConfig.h, which
 *          expands to app_fault_assert(__FILE__, __LINE__).  Saves
 *          APP_FAULT_ASSERT together with the running task name, the source
 *          file and the line, then resets through app_fault_fatal(), so the
 *          next boot reports the failure in Status (DGEN-004).
 *
 * @param[in] file Source file name from @c __FILE__, normally an absolute
 *                 build path; only the basename is kept, truncated to 31
 *                 characters.
 * @param[in] line Source line number from @c __LINE__, stored in the @c line
 *                 field of @ref app_fault_record_t.
 *
 * @post The no-init record holds APP_FAULT_ASSERT and the MCU resets.
 * @note The context is whatever tripped the assertion: task, ISR or
 *       start-up.  The call does not return, does not block and allocates
 *       nothing; the fault status and stacked-register fields are left zero,
 *       because only the task, file and line are meaningful here.
 * @warning The prototype in @ref FreeRTOSConfig.h cannot carry the
 *          @c noreturn attribute, because the kernel translation units do not
 *          include @ref app_fault.h, so the compiler cannot see that control
 *          never returns to the caller.
 * @see app_fault_fatal
 * @see configASSERT
 */
void app_fault_assert(const char *file, int line) { app_fault_fatal(APP_FAULT_ASSERT, current_task_name(), file, (uint32_t)line); }

/**
 * @brief Record a detected task stack overflow and reset (DRT-002).
 * @details Called by the kernel from the context-switch path when
 *          @c configCHECK_FOR_STACK_OVERFLOW detects that a task overran its
 *          stack: method 2 compares the stack pointer with the stack limit and
 *          also scans the 20 bytes at the bottom of the stack for the
 *          0xa5a5a5a5 fill pattern.  Saves APP_FAULT_STACK_OVERFLOW with the
 *          offending task name and the label "FreeRTOS", then resets through
 *          app_fault_fatal(), so the next boot reports which task overflowed
 *          (DGEN-004).
 *
 * @param[in] task Handle of the overflowing task.  Unused: the hook needs only
 *                 the name, which the kernel has already resolved, so the
 *                 handle is explicitly discarded.
 * @param[in] name Name of the overflowing task.  May be NULL, in which case
 *                 the recorded task name is empty; truncated to 15 characters.
 *
 * @post The no-init record holds APP_FAULT_STACK_OVERFLOW and the MCU resets.
 * @note Runs in the context of the context switch, normally the SysTick or
 *       PendSV exception, so it is ISR context and must not block; it does
 *       not.  The @c line field is 0 because no source line applies.
 * @note The name is read from the TCB, not from the task stack, so it is
 *       still readable after the overflow.
 * @warning The offending stack is already corrupted, so the hook performs no
 *          logging or string formatting that would need stack space.
 * @see app_fault_fatal
 * @see current_task_name
 */
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    app_fault_fatal(APP_FAULT_STACK_OVERFLOW, name, "FreeRTOS", 0U);
}

/**
 * @brief Record a failed heap allocation and reset (DRT-002).
 * @details Called by @c pvPortMalloc() in heap_4 when no free block can
 *          satisfy the request and @c configUSE_MALLOC_FAILED_HOOK is 1.
 *          Saves APP_FAULT_MALLOC_FAILED with the running task name, the label
 *          "malloc" and, in the @c line field, the number of free heap bytes
 *          that remained.  The record makes a heap shortfall visible across
 *          the reset instead of leaving a NULL handle behind (DGEN-004).
 *
 * @post The no-init record holds APP_FAULT_MALLOC_FAILED, with the free heap
 *       size in the @c line field, and the MCU resets.
 * @note The free size is sampled after the request was rejected, so it is
 *       smaller than the requested block.  It is a diagnostic hint for sizing
 *       the heap, not an exact requirement.  @c xPortGetFreeHeapSize() only
 *       reads the heap_4 free-byte counter, so it is cheap and does not walk
 *       the block list.
 * @note Runs in the context that called @c pvPortMalloc(), normally a task
 *       but also start-up before the scheduler runs; it does not block and
 *       allocates nothing.
 * @warning The @c line field is overloaded by reason: a byte count here, a
 *          source line for APP_FAULT_ASSERT.  A reader must check @c reason
 *          before interpreting it.
 * @see app_fault_fatal
 * @see vApplicationStackOverflowHook
 */
void vApplicationMallocFailedHook(void)
{
    /* Store the free heap in the line field so Status can report how much
     * was left when the allocation failed. */
    app_fault_fatal(APP_FAULT_MALLOC_FAILED, current_task_name(), "malloc", (uint32_t)xPortGetFreeHeapSize());
}
