/*
 * reset_cause.h - RCC reset flags captured at start-up
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DGEN-005, LED-005
 */

#ifndef RESET_CAUSE_H
#define RESET_CAUSE_H

#include <stdbool.h>
#include <stdint.h>

extern uint32_t app_reset_cause_flags;
extern const char *app_reset_cause_text;

/* Read RCC_CSR, keep a copy for Status and clear the flags. */
void app_reset_cause_capture(void);

/* True when the last reset was caused by a fault, watchdog or software
 * failure; LD3 is latched on for these (LED-005). */
bool app_reset_was_error(void);

#endif /* RESET_CAUSE_H */
