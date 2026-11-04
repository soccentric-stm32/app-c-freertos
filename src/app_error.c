/*
 * app_error.c - latched error state shown on LD3
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI
 * SRS:     LED-005, DGEN-004, DGEN-005, HLT-003
 */

#include "stm32f7xx_hal.h"
#include "app_error.h"

volatile uint32_t app_error_flags;

void app_error_set(uint32_t flags)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    app_error_flags |= flags;
    __set_PRIMASK(primask);
}

void app_error_clear(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    app_error_flags = 0U;
    __set_PRIMASK(primask);
}
