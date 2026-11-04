/*
 * rtos_hooks.c - FreeRTOS error hooks (DRT-002)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DRT-002 (assert, stack overflow, malloc failed)
 *
 * Each hook records the reason in the no-init fault area (DGEN-004) and
 * resets through the common fault path, so the next Status reports it.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "app_fault.h"

static const char *current_task_name(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        return pcTaskGetName(NULL);
    }
    return "startup";
}

void app_fault_assert(const char *file, int line)
{
    app_fault_fatal(APP_FAULT_ASSERT, current_task_name(), file,
                     (uint32_t)line);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    app_fault_fatal(APP_FAULT_STACK_OVERFLOW, name, "FreeRTOS", 0U);
}

void vApplicationMallocFailedHook(void)
{
    /* Store the free heap in the line field so Status can report how much
     * was left when the allocation failed. */
    app_fault_fatal(APP_FAULT_MALLOC_FAILED, current_task_name(),
                    "malloc", (uint32_t)xPortGetFreeHeapSize());
}
