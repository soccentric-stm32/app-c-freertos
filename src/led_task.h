/*
 * led_task.h - LED patterns and LD3 error display (section 6)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI
 * SRS:     LED-001..LED-007, BTN-002, BTN-003
 */

#ifndef LED_TASK_H
#define LED_TASK_H

#include <stdbool.h>
#include <stdint.h>

#define APP_LED_ID_1       1U
#define APP_LED_ID_2       2U
#define APP_LED_ID_3       3U

#define APP_PATTERN_OFF        0U
#define APP_PATTERN_ON         1U
#define APP_PATTERN_BLINK      2U
#define APP_PATTERN_HEARTBEAT  3U
#define APP_PATTERN_FAST       4U
#define APP_PATTERN_CHASE      5U
#define APP_PATTERN_MORSE_SOS  6U
#define APP_PATTERN_COUNT      7U

#define APP_LED_MIN_PERIOD     50U
#define APP_LED_MAX_PERIOD     5000U
#define APP_LED_DEFAULT_PERIOD 1000U

void app_leds_init(void);
void app_led3_set(bool on);

/* Create the LED task and its command queue before the scheduler starts. */
bool led_task_create(void);

/* Queue operations from other tasks (DRT-011: no direct state writes). */
bool led_post_set(uint8_t led, uint8_t pattern, uint16_t period, bool force);
bool led_post_set_all(uint8_t pattern, uint16_t period);
bool led_post_button(uint8_t press);       /* 1 = short, 2 = long */
bool led_post_clear_error(void);
bool led_post_health_failed(void);

#endif /* LED_TASK_H */
