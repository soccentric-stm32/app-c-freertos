/*
 * health_task.h - task supervision, health check and CONFIRMED record
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DRT-012, DRT-013, HLT-001..HLT-005
 */

#ifndef HEALTH_TASK_H
#define HEALTH_TASK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HEALTH_TASK_LED = 0,
    HEALTH_TASK_BUTTON,
    HEALTH_TASK_COMMS,
    HEALTH_TASK_COUNT
} health_task_id_t;

/* Create the highest-priority Health task before the scheduler starts. */
bool health_task_create(void);

/* Called by every task except Health at least once per second. */
void health_checkin(health_task_id_t task);

/* HealthState values used by Status. */
uint8_t health_state_get(void);
bool health_is_failed(void);

#endif /* HEALTH_TASK_H */
