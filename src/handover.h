/*
 * handover.h - BOOT hand-over record (RTC_BKP2R)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DBT-004, DBT-005
 */

#ifndef HANDOVER_H
#define HANDOVER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool present;
    uint8_t slot;         /* APP_SLOT_* */
    uint8_t trial;        /* 0 unknown, 1 confirmed, 2 pending */
    uint8_t trial_count;
    uint32_t raw;
} app_handover_t;

extern app_handover_t g_handover;

/* Enable backup-domain access and decode RTC_BKP2R.  Missing record means
 * slot UNKNOWN and no confirmation (DBT-004). */
void app_handover_read(void);

/* Write the "stay in BOOT" magic to RTC_BKP1R (DBT-005). */
void app_handover_request_bootloader(void);

#endif /* HANDOVER_H */
