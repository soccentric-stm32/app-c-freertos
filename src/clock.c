/*
 * clock.c - 216 MHz core clock (DGEN-003)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6 rev Z @ 3.3 V (voltage scale 1)
 * SRS:     DGEN-003 (same configuration as HWVAL GEN-004), DBT-003
 *
 * Taken from the validation firmware's src/clock.c (DGEN-007 asks for a
 * shared copy later, OPEN-D02).
 */

#include "stm32f7xx_hal.h"
#include "app_board.h"
#include "app_fault.h"
#include "clock.h"

static void clock_fail(void) __attribute__((noreturn));
static void clock_fail(void)
{
    app_fault_fatal(APP_FAULT_STARTUP, "clock", "HSE/PLL/over-drive", 0U);
}

void app_clock_init(void)
{
    /* PWR register access needs the PWR interface clock (APB1) first. */
    __HAL_RCC_PWR_CLK_ENABLE();

    /* Voltage scale 1 before raising the frequency. As in HWVAL (CL-009),
     * VOSRDY is not polled: it does not set before the PLL is up. */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_BYPASS; /* ST-LINK MCO 8 MHz (DCON-005) */
    osc.HSIState            = RCC_HSI_OFF;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM            = 8;              /* VCO in: 1 MHz               */
    osc.PLL.PLLN            = 432;            /* VCO: 432 MHz                */
    osc.PLL.PLLP            = RCC_PLLP_DIV2;  /* SYSCLK: 216 MHz             */
    osc.PLL.PLLQ            = 9;              /* PLLQ: 48 MHz                */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        clock_fail();
    }

    /* 216 MHz needs over-drive in voltage scale 1 ([DS11532]). */
    if (HAL_PWREx_EnableOverDrive() != HAL_OK) {
        clock_fail();
    }

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                         RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;      /* HCLK 216 MHz           */
    clk.APB1CLKDivider = APP_APB1_PRESCALER;   /* 54 MHz, timers 108 MHz */
    clk.APB2CLKDivider = APP_APB2_PRESCALER;   /* 108 MHz, timers 216 MHz */

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_7) != HAL_OK) {
        clock_fail();
    }

    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
    __HAL_FLASH_ART_ENABLE();
    /* Caches and the non-cacheable DMA MPU region are enabled together in
     * app_cache_mpu_init() after this function. */
}
