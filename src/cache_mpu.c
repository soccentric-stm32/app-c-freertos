/*
 * cache_mpu.c - caches and the non-cacheable DMA window
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DGEN-003 (caches on), COM-002 (DMA buffers)
 *
 * All DMA buffers are placed in the .dma_buffer section in SRAM2.  One MPU
 * region makes that 16 KB window normal, non-cacheable memory, so the CPU
 * and DMA see the same bytes without cache maintenance in the interrupt
 * path.  The rest of the map uses the default memory attributes.
 */

#include "stm32f7xx_hal.h"
#include "cache_mpu.h"

#define DMA_MPU_REGION       6U
#define DMA_MPU_BASE         0x2007C000UL  /* SRAM2, 16 KB                  */
#define DMA_MPU_SIZE_LOG2    13U          /* 2^(13+1) = 16 KB             */

void app_cache_mpu_init(void)
{
    MPU->CTRL = 0U;
    __DSB();
    __ISB();

    MPU->RNR = DMA_MPU_REGION;
    MPU->RBAR = DMA_MPU_BASE | (DMA_MPU_REGION << MPU_RBAR_REGION_Pos) |
                MPU_RBAR_VALID_Msk;
    /* XN=1 (not executable), AP=full access, TEX=1, C=0, B=0, S=0. */
    MPU->RASR = MPU_RASR_XN_Msk |
                (3UL << MPU_RASR_AP_Pos) |
                (1UL << MPU_RASR_TEX_Pos) |
                (DMA_MPU_SIZE_LOG2 << MPU_RASR_SIZE_Pos) |
                MPU_RASR_ENABLE_Msk;

    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    __DSB();
    __ISB();

    SCB_EnableICache();
    SCB_EnableDCache();
}
