/*
 * iwdg.c - independent watchdog service
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DBT-002, DRT-012, HLT-003, HND-004
 *
 * BOOT starts the IWDG with a 4 s timeout (TRY-004).  If DEMO is started
 * without BOOT for debugging, it starts the same 4 s timeout itself.
 * LSI is nominally 32 kHz; prescaler /64 and reload 1999 give 4.0 s.
 */

#include <stdbool.h>
#include "stm32f7xx_hal.h"
#include "iwdg.h"

#define IWDG_KEY_ACCESS       0x00005555UL
#define IWDG_RELOAD_4S        1999U

static void iwdg_wait_ready(void)
{
    while ((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) != 0U) {
        __NOP();
    }
}

void app_iwdg_early_kick(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

void app_iwdg_init(void)
{
    /* PR == 0 and RLR == 0xFFF is the reset state; if they differ, BOOT (or
     * a previous stage) has already configured and started the watchdog. */
    bool running = (IWDG->PR != 0U) || (IWDG->RLR != 0x0FFFU);

    if (!running) {
        IWDG->KR = IWDG_KEY_ACCESS;
        IWDG->PR = IWDG_PRESCALER_64;
        IWDG->RLR = IWDG_RELOAD_4S;
        iwdg_wait_ready();
        IWDG->KR = IWDG_KEY_RELOAD;
        IWDG->KR = IWDG_KEY_ENABLE;
    }

    app_iwdg_kick();
}

void app_iwdg_kick(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}
