/**
 * @file    main.c
 * @brief   DEMO entry point: BOOT hand-over, driver bring-up, task start-up.
 * @details DEMO runs only under BOOT.  It keeps the four documented tasks
 *          (Health, Comms, Button, LED), speaks HWLINK on the ST-LINK VCP and
 *          confirms a pending slot after its health check.  Everything is
 *          created before the scheduler starts, so no task allocates memory
 *          afterwards (`DRT-004`).
 *
 *          The file has exactly two functions: @ref main, which walks the
 *          start-up sequence once from reset to @c vTaskStartScheduler, and
 *          the @ref startup_fail helper that turns a refused bring-up step
 *          into a saved fault record, the LD3 fault pattern and a reset.
 *          There are no macros and no file-scope variables here; the file
 *          keeps no state and owns no buffer.
 *
 *          The order of the steps is not arbitrary: the vector table and the
 *          watchdog come first (`DBT-002`), the reset cause is captured
 *          before anything can clear it (`DGEN-005`), the fault handlers are
 *          enabled before the hand-over record is read (`DGEN-004`), and the
 *          HWLINK transport is up before the tasks that supervise it
 *          (`COM-001`, `HLT-001`).
 *
 *          Nothing here reads a sensor, drives an LED or parses a frame: each
 *          behaviour lives in the module named by the call, and this file
 *          only decides in which order those modules are started.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6), started by BOOT from slot A or B
 * @srs     DBT-001..DBT-006, DGEN-001..DGEN-006, DRT-004, section 5
 * @ingroup main
 * @see     boot_contract.h
 * @see     handover.h
 * @see     health_task.h
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_error.h"
#include "app_fault.h"
#include "boot_contract.h"
#include "button_task.h"
#include "cache_mpu.h"
#include "clock.h"
#include "comms_task.h"
#include "app_log.h"
#include "handover.h"
#include "health_task.h"
#include "iwdg.h"
#include "led_task.h"
#include "reset_cause.h"
#include "uart3.h"

/**
 * @defgroup main Application entry point
 * @brief Ordered start-up of the DEMO image and its fatal failure path.
 * @details The group covers the whole of @ref main.c.  @ref main runs once on
 *          the reset thread and never returns; @ref startup_fail is the only
 *          other function and is reachable only from @ref main.  The step
 *          order documented on @ref main is a contract with BOOT and with the
 *          SRS, so this group is the place to see what happens in which
 *          order; the modules it calls document the individual steps.
 * @{
 */

/** @brief Fatal start-up path; the contract is documented at the definition. */
static void startup_fail(const char *where) __attribute__((noreturn));

/**
 * @brief   Report a fatal start-up failure, save the reason and reset.
 * @details Formats one @c LOG_E line naming @p where, waits at most 50 ms for
 *          the USART6 ring to drain (@ref app_log_flush, `DLOG-004`) and then
 *          calls @ref app_fault_fatal, which stores @ref APP_FAULT_STARTUP in
 *          the no-init record with the task name `"main"`, @p where as the
 *          file/stage label and line 0, shows the LD3 fault pattern for about
 *          2.2 s and resets the MCU (`DGEN-004`, `LED-005`).
 *
 *          The stage label is what the next boot prints and reports through
 *          @c Status, so the values are short and stable: `"uart3 init"`,
 *          `"led task"`, `"button task"`, `"comms task"`, `"health task"`
 *          and `"scheduler"`.
 *
 * @param[in] where NUL-terminated stage label naming the step that failed;
 *                  must not be NULL.  At most the first 31 characters survive
 *                  in the record, because the label is stored in
 *                  @ref app_fault_record_t.file
 *                  (@ref APP_FAULT_FILE_LEN includes the NUL terminator).
 *
 * @pre   Called on the reset thread before the scheduler starts, after
 *        @ref app_log_init has returned so that the line is really queued.
 * @post  A valid @ref APP_FAULT_STARTUP record exists in @c .noinit unless the
 *        reset interrupts the save, and the MCU is reset; the function never
 *        returns to its caller.
 *
 * @note  Execution context: reset thread mode, neither a task nor an ISR.
 *        Neither FreeRTOS nor the LED task is involved, so the path also works
 *        when the scheduler is not running or has just refused to start.
 * @note  The 50 ms flush is a bounded busy wait with interrupts masked.  It is
 *        a `DLOG-004` allowance for a deliberate reset, not a pattern to copy
 *        into normal operation.
 * @warning Never returns.  Nothing is unwound and no task is stopped: the
 *          reset is the only way forward and BOOT decides what runs next
 *          (`DBT-006`).
 * @see   app_fault_fatal
 * @see   app_log_flush
 */
static void startup_fail(const char *where)
{
    LOG_E("start-up failed: %s", where);
    app_log_flush(50U);
    app_fault_fatal(APP_FAULT_STARTUP, "main", where, 0U);
}

/**
 * @brief   Bring the image up in the contractual order and start FreeRTOS.
 * @details Each step depends on the ones before it, and the order is part of
 *          the BOOT contract and of the SRS:
 *          -# **Vector table.**  @c SCB->VTOR is set to @ref APP_SLOT_BASE as
 *             the first statement, before any interrupt or exception can use
 *             the table: the image is linked for slot A and may be running
 *             from slot B behind BOOT's bank swap (`DBT-001`, `DBT-002`).
 *          -# **Early watchdog refresh.**  @ref app_iwdg_early_kick reloads
 *             the 4 s IWDG that BOOT started, so the whole bring-up stays
 *             inside the inherited timeout (`DBT-002`, `HND-004`).
 *          -# **Boot identifier.**  @ref app_fault_boot_start increments the
 *             retained counter in @c .noinit before anything can save a
 *             fault, so a later record can be attributed to this boot or to
 *             the previous one (`DGEN-004`, `DRT-002`).
 *          -# **Reset cause.**  @ref app_reset_cause_capture snapshots
 *             @c RCC_CSR, decodes it into @ref app_reset_cause_text and
 *             clears the hardware flags.  It runs before the HAL so nothing
 *             else can disturb the evidence (`DGEN-005`, `LED-005`).
 *          -# **HAL, clocks, caches.**  @c HAL_Init starts the TIM6 1 ms tick
 *             and leaves SysTick to FreeRTOS (`DGEN-006`, `DRT-001`),
 *             @ref app_clock_init programs the 216 MHz tree from the reset
 *             state BOOT leaves (`DGEN-003`, `DBT-003`) and
 *             @ref app_cache_mpu_init enables the I/D caches plus the
 *             non-cacheable SRAM2 DMA window (`DGEN-003`, `COM-002`).  A
 *             @c HAL_Init failure is deliberately ignored with a void cast:
 *             the clock tree is written from scratch afterwards anyway.
 *          -# **Fault handlers.**  MemManage, BusFault and UsageFault are
 *             enabled in @c SCB->SHCSR, followed by a @c __DSB and a @c __ISB,
 *             so each fault class reaches its own handler and the saved
 *             reason is specific instead of a generic HardFault escalation
 *             (`DGEN-004`).
 *          -# **Hand-over record.**  @ref app_handover_read decodes
 *             @c RTC_BKP2R into @ref g_handover; it needs the clock tree that
 *             was just programmed and relies on the PWR/RTC clocks BOOT leaves
 *             running instead of enabling them itself (`DBT-004`, `HND-003`).
 *          -# **Debug log.**  @ref app_log_init starts USART6 and the first
 *             @c LOG_I line reports version, commit, slot, trial state and
 *             reset cause (`DGEN-002`, `DLOG-001`).
 *          -# **LEDs and watchdog.**  @ref app_leds_init drives LD1, LD2 and
 *             LD3 dark before anything can indicate on them (`DSAFE-003`),
 *             @ref app_iwdg_init preserves the BOOT timeout, or starts a 4 s
 *             watchdog when DEMO runs stand-alone (`HND-004`).
 *          -# **Fault and watchdog latches.**  A valid no-init record latches
 *             @ref APP_ERROR_FAULT and is printed (`DGEN-004`, `DLOG-001`); a
 *             watchdog reset latches @ref APP_ERROR_WATCHDOG.  Both keep LD3
 *             solid until ClearFault (`LED-005`, `DGEN-005`).
 *          -# **HWLINK transport.**  @ref app_uart3_init arms the USART3 DMA
 *             ring.  The link must be running before the health check looks
 *             at it, and a failure is fatal (`COM-001`, `COM-002`,
 *             `HLT-001`).
 *          -# **Tasks.**  LED, Button, Comms and Health are created in that
 *             order, each failure fatal, with Health last so that every
 *             check-in target already exists (`DRT-004`, SRS section 5.2).
 *          -# **Scheduler.**  @c vTaskStartScheduler does not return on
 *             success.  The code after it is the unreachable failure path,
 *             taken only when the idle or timer task could not be created
 *             and handled like any other start-up failure (`DRT-004`).
 *
 * @return Never returns.  On success the scheduler takes over and the four
 *         tasks run forever; every failure path ends in @ref startup_fail,
 *         which saves a fault record and resets the MCU.
 *
 * @pre    The MCU was started by BOOT from slot A or slot B.  A stand-alone
 *         debug start is tolerated: the hand-over record then reads as
 *         missing and the watchdog is started here instead.
 * @post   The four application tasks are running, USART3 carries HWLINK, the
 *         debug log is on USART6, and @ref g_handover, @ref app_error_flags
 *         and @ref app_reset_cause_text describe the state found at start-up.
 *
 * @note   Execution context: reset thread mode, privileged, with no scheduler
 *         and no RTOS object in use.  Every call is a single-threaded
 *         start-up call; the first FreeRTOS API call is made indirectly by
 *         @ref led_task_create.
 * @note   Nothing is allocated after this function returns, and every buffer
 *         the tasks use is static, so the FreeRTOS heap only has to cover the
 *         tasks and queues created here (`DRT-004`).
 * @warning Do not reorder the steps.  VTOR before the watchdog, the reset
 *          cause before the HAL, the fault enables before the hand-over read
 *          and USART3 before task creation are all contractual (`DBT-002`,
 *          `DGEN-005`, `DGEN-004`, `COM-002`).
 * @see    startup_fail
 * @see    iwdg.h
 * @see    reset_cause.h
 */
int main(void)
{
    /* DBT-002: first instructions, before any interrupt can use the table. */
    SCB->VTOR = APP_SLOT_BASE;
    app_iwdg_early_kick();
    app_fault_boot_start();

    /* DGEN-005: keep the flags for Status, then clear them. */
    app_reset_cause_capture();

    (void)HAL_Init();
    /* DGEN-003 / DBT-003: reset clock state -> 216 MHz, then the caches. */
    app_clock_init();
    app_cache_mpu_init();

    /* DGEN-004: take MemManage/BusFault/UsageFault to their own handlers so
     * the saved reason is specific; HardFault catches escalations. */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
    __DSB();
    __ISB();

    /* DBT-004 / DBT-005: hand-over record and backup-domain access. */
    app_handover_read();

    /* DLOG-001: debug lines on USART6 from here on. */
    app_log_init();
    LOG_I("start v" APP_VERSION_STR " " APP_GIT_COMMIT ", slot %c %s, "
          "reset %s",
          g_handover.slot == APP_SLOT_A   ? 'A'
          : g_handover.slot == APP_SLOT_B ? 'B'
                                          : '?',
          g_handover.trial == 2U   ? "on trial"
          : g_handover.trial == 1U ? "confirmed"
                                   : "no hand-over",
          app_reset_cause_text);

    /* DSAFE-003: three LED pins dark; HND-004: keep BOOT's IWDG timeout. */
    app_leds_init();
    app_iwdg_init();

    /* LED-005: latch LD3 for the last fault or watchdog reset. */
    if(app_fault_is_valid())
    {
        app_error_set(APP_ERROR_FAULT);
        LOG_W("last fault: reason %lu in %.16s at %.32s:%lu, pc 0x%08lx", (unsigned long)g_app_fault.reason, g_app_fault.task, g_app_fault.file, (unsigned long)g_app_fault.line, (unsigned long)g_app_fault.pc);
    }
    if(app_reset_was_error())
    {
        app_error_set(APP_ERROR_WATCHDOG);
    }

    /* COM-001/002: USART3 must be running before the health check. */
    if(!app_uart3_init())
    {
        startup_fail("uart3 init");
    }

    /* DRT-004: every task and queue exists before vTaskStartScheduler(). */
    if(!led_task_create())
    {
        startup_fail("led task");
    }
    if(!button_task_create())
    {
        startup_fail("button task");
    }
    if(!comms_task_create())
    {
        startup_fail("comms task");
    }
    if(!health_task_create())
    {
        startup_fail("health task");
    }

    vTaskStartScheduler();

    /* Only reached when the idle or timer task could not be created. */
    startup_fail("scheduler");
}

/** @} */
