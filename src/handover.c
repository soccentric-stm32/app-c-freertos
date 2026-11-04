/*
 * handover.c - BOOT hand-over record (RTC_BKP2R)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DBT-004, DBT-005, HND-003, ENT-002
 *
 * VBAT is tied to VDD on the NUCLEO-F767ZI, so the backup registers survive
 * a system reset (BOOT hand-over) but not a power cycle.
 */

#include "stm32f7xx_hal.h"
#include "boot_contract.h"
#include "handover.h"

app_handover_t g_handover;

void app_handover_read(void)
{
    /* Writing the stay magic needs DBP; do it once before the scheduler. */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_RTC_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    uint32_t raw = RTC->BKP2R;

    g_handover.raw = raw;
    g_handover.present = (raw & APP_HANDOVER_MAGIC_MASK) ==
                         APP_HANDOVER_MAGIC;
    if (!g_handover.present) {
        g_handover.slot = APP_SLOT_UNKNOWN;
        g_handover.trial = 0U;
        g_handover.trial_count = 0U;
        return;
    }

    g_handover.slot = (uint8_t)((raw >> APP_HANDOVER_SLOT_SHIFT) &
                                APP_HANDOVER_SLOT_MASK);
    if (g_handover.slot != APP_SLOT_A && g_handover.slot != APP_SLOT_B) {
        g_handover.slot = APP_SLOT_UNKNOWN;
        g_handover.present = false;
        g_handover.trial = 0U;
        g_handover.trial_count = 0U;
        return;
    }

    g_handover.trial = ((raw >> APP_HANDOVER_PENDING_BIT) & 1U) != 0U
                       ? 2U : 1U;
    g_handover.trial_count = (uint8_t)((raw >> APP_HANDOVER_TRIAL_SHIFT) &
                                       APP_HANDOVER_TRIAL_MASK);
}

void app_handover_request_bootloader(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_RTC_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    RTC->BKP1R = APP_STAY_IN_BOOT_MAGIC;
    __DSB();
}
