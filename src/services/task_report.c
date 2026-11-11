/**
 * @file    task_report.c
 * @brief   Once-a-second task status line; see task_report.h.
 */
#include "services/task_report.h"

#include "FreeRTOS.h"
#include "task.h"
#include "services/app_log.h"

#define TASK_REPORT_PERIOD_MS 1000U

void task_report(task_report_t *state, unsigned number, const char *name, const char *purpose)
{
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if((state->primed != 0U) && ((now - state->last_ms) < TASK_REPORT_PERIOD_MS))
    {
        return;
    }
    state->primed  = 1U;
    state->last_ms = now;
    app_log('I', "task %u %s: %s", number, name, purpose);
}
