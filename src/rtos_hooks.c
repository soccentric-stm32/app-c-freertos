/*
 * rtos_hooks.c - FreeRTOS hooks and fault handlers (DRT-002, DGEN-004)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DRT-002 (assert, stack overflow, malloc failed), DGEN-004
 *
 * Each error turns LD3 on for about 2 s and resets. Saving the reason and
 * the fault registers to a no-init area for the Status message (DGEN-004)
 * comes with the Comms task.
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "led_task.h"

void app_fault_assert(const char *file, int line);
void vApplicationStackOverflowHook(TaskHandle_t task, char *name);
void vApplicationMallocFailedHook(void);
void HardFault_Handler(void);

/* Works with interrupts off and without the scheduler. */
static void app_fatal(void) __attribute__((noreturn));
static void app_fatal(void)
{
    __disable_irq();
    app_led3_set(true);
    for (volatile uint32_t n = (SystemCoreClock / 4U) * 2U; n > 0U; n--) {
        __NOP();
    }
    NVIC_SystemReset();
}

void app_fault_assert(const char *file, int line)
{
    (void)file;
    (void)line;
    app_fatal();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    app_fatal();
}

void vApplicationMallocFailedHook(void)
{
    app_fatal();
}

/* MemManage, BusFault and UsageFault are not enabled separately, so they
 * escalate here. */
void HardFault_Handler(void)
{
    app_fatal();
}
