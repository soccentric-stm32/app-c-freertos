/*
 * clock.h - 216 MHz core clock (DGEN-003)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * SRS:     DGEN-003, DBT-003
 */

#ifndef CLOCK_H
#define CLOCK_H

/* Configure the clock tree from the reset state BOOT leaves (HND-001). */
void app_clock_init(void);

#endif /* CLOCK_H */
