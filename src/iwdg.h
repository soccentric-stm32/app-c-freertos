/*
 * iwdg.h - independent watchdog service
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DBT-002, DRT-012, HLT-003, HND-004
 */

#ifndef IWDG_H
#define IWDG_H

/* Refresh the IWDG as early as possible; BOOT starts it with a 4 s timeout
 * before hand-over (HND-004).  Safe before the HAL and clocks are up. */
void app_iwdg_early_kick(void);

/* Start the IWDG at 4 s if BOOT did not already start it, then refresh. */
void app_iwdg_init(void);

/* Refresh.  Health calls this only while every task checked in. */
void app_iwdg_kick(void);

#endif /* IWDG_H */
