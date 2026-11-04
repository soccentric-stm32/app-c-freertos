/*
 * button_task.h - user button B1 (PC13)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI
 * SRS:     BTN-001..BTN-004
 */

#ifndef BUTTON_TASK_H
#define BUTTON_TASK_H

#include <stdbool.h>

/* Create the Button task and configure EXTI13 before the scheduler starts. */
bool button_task_create(void);

#endif /* BUTTON_TASK_H */
