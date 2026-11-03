/*
 * led_task.h - LED task (section 6)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * SRS:     LED-001, LED-002, LED-006
 */

#ifndef LED_TASK_H
#define LED_TASK_H

#include <stdbool.h>

/* Configure LD1..LD3 as outputs, all off. Call before the scheduler. */
void app_leds_init(void);

/* LD3 is reserved for errors (LED-005); usable from fault paths. */
void app_led3_set(bool on);

/* Create the LED task; returns false when the task cannot be created. */
bool led_task_create(void);

#endif /* LED_TASK_H */
