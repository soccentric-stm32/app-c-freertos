/*
 * app_error.h - latched error state shown on LD3
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI
 * SRS:     LED-005, DGEN-004, DGEN-005, HLT-003
 */

#ifndef APP_ERROR_H
#define APP_ERROR_H

#include <stdint.h>

#define APP_ERROR_FAULT      (1UL << 0U)   /* saved fault or assert       */
#define APP_ERROR_WATCHDOG   (1UL << 1U)   /* last reset was IWDG/WWDG    */
#define APP_ERROR_HEALTH     (1UL << 2U)   /* pending-slot health failed  */

/* Read-only latch shared with the LED task and Status.  Only the module that
 * owns a bit sets it; ClearFault clears the whole latch. */
extern volatile uint32_t app_error_flags;

void app_error_set(uint32_t flags);
void app_error_clear(void);

#endif /* APP_ERROR_H */
