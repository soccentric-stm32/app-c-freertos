/*
 * hal_timebase.c - HAL tick on TIM6 (DGEN-006)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DGEN-006 - the HAL time base uses a hardware timer other than
 *          SysTick (SysTick belongs to FreeRTOS, DRT-001). Same scheme as
 *          the validation firmware (HWVAL GEN-012).
 */

#include "stm32f7xx_hal.h"

void TIM6_DAC_IRQHandler(void);

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    /* APB timers run at PCLK x2 unless the APB prescaler is 1 (RM0410). */
    uint32_t timclk = ((RCC->CFGR & RCC_CFGR_PPRE1) == 0U) ? pclk1 : pclk1 * 2U;
    uint32_t prescaler = (timclk / 1000000U) - 1U;

    __HAL_RCC_TIM6_CLK_ENABLE();

    TIM6->CR1 = 0U;
    TIM6->PSC = prescaler;       /* 1 MHz count                          */
    TIM6->ARR = 1000U - 1U;      /* 1000 counts = 1 ms (uwTickFreq 1kHz) */
    TIM6->EGR = TIM_EGR_UG;      /* latch PSC/ARR, reset counter         */
    TIM6->SR  = 0U;
    TIM6->DIER = TIM_DIER_UIE;
    TIM6->CR1 = TIM_CR1_CEN | TIM_CR1_URS;

    uwTickPrio = TickPriority;
    NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority);
    NVIC_EnableIRQ(TIM6_DAC_IRQn);

    return HAL_OK;
}

void HAL_SuspendTick(void)
{
    TIM6->DIER &= ~TIM_DIER_UIE;
}

void HAL_ResumeTick(void)
{
    TIM6->SR = 0U;
    TIM6->DIER |= TIM_DIER_UIE;
}

void TIM6_DAC_IRQHandler(void)
{
    if ((TIM6->SR & TIM_SR_UIF) != 0U) {
        TIM6->SR = ~TIM_SR_UIF;
        uwTick += uwTickFreq;
    }
}
