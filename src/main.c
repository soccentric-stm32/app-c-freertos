/*
 * app - FreeRTOS demo application entry point
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (STM32F767ZIT6), started by BOOT from slot A or B
 * SRS:     DBT-001..DBT-006, DGEN-001..DGEN-006, DRT-004, section 5
 *
 * DEMO runs only under BOOT.  It keeps the four documented tasks (Health,
 * Comms, Button, LED), speaks HWLINK on the ST-LINK VCP, and confirms a
 * pending slot after its health check.  Everything is created before the
 * scheduler starts; no task allocates memory afterwards.
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

static void startup_fail(const char *where) __attribute__((noreturn));
static void startup_fail(const char *where)
{
    LOG_E("start-up failed: %s", where);
    app_log_flush(50U);
    app_fault_fatal(APP_FAULT_STARTUP, "main", where, 0U);
}

int main(void)
{
    /* DBT-002: first instructions, before any interrupt can use the table. */
    SCB->VTOR = APP_SLOT_BASE;
    app_iwdg_early_kick();
    app_fault_boot_start();

    /* DGEN-005: keep the flags for Status, then clear them. */
    app_reset_cause_capture();

    (void)HAL_Init();
    app_clock_init();
    app_cache_mpu_init();

    /* DGEN-004: take MemManage/BusFault/UsageFault to their own handlers so
     * the saved reason is specific; HardFault catches escalations. */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
    __DSB();
    __ISB();

    /* DBT-004 / DBT-005: hand-over record and backup-domain access. */
    app_handover_read();

    /* DLOG-001: debug lines on USART6 from here on. */
    app_log_init();
    LOG_I("start v" APP_VERSION_STR " " APP_GIT_COMMIT ", slot %c %s, "
          "reset %s",
          g_handover.slot == APP_SLOT_A ? 'A' :
          g_handover.slot == APP_SLOT_B ? 'B' : '?',
          g_handover.trial == 2U ? "on trial" :
          g_handover.trial == 1U ? "confirmed" : "no hand-over",
          app_reset_cause_text);

    app_leds_init();
    app_iwdg_init();

    /* LED-005: latch LD3 for the last fault or watchdog reset. */
    if (app_fault_is_valid()) {
        app_error_set(APP_ERROR_FAULT);
        LOG_W("last fault: reason %lu in %.16s at %.32s:%lu, pc 0x%08lx",
              (unsigned long)g_app_fault.reason, g_app_fault.task,
              g_app_fault.file, (unsigned long)g_app_fault.line,
              (unsigned long)g_app_fault.pc);
    }
    if (app_reset_was_error()) {
        app_error_set(APP_ERROR_WATCHDOG);
    }

    /* COM-001/002: USART3 must be running before the health check. */
    if (!app_uart3_init()) {
        startup_fail("uart3 init");
    }

    /* DRT-004: every task and queue exists before vTaskStartScheduler(). */
    if (!led_task_create()) {
        startup_fail("led task");
    }
    if (!button_task_create()) {
        startup_fail("button task");
    }
    if (!comms_task_create()) {
        startup_fail("comms task");
    }
    if (!health_task_create()) {
        startup_fail("health task");
    }

    vTaskStartScheduler();

    /* Only reached when the idle or timer task could not be created. */
    startup_fail("scheduler");
}
