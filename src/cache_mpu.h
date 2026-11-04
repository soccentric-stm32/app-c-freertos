/*
 * cache_mpu.h - caches and the non-cacheable DMA window
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DGEN-003 (caches on), COM-002 (DMA buffers)
 */

#ifndef CACHE_MPU_H
#define CACHE_MPU_H

/* Configure the MPU region for .dma_buffer in SRAM2 as normal, non-cacheable
 * memory, then enable the I-cache and D-cache.  Call once before DMA starts. */
void app_cache_mpu_init(void);

#endif /* CACHE_MPU_H */
