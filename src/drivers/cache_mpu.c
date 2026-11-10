/**
 * @file    cache_mpu.c
 * @brief   Cortex-M7 cache enable and the non-cacheable DMA window.
 * @details Implements @ref app_cache_mpu_init, the one-shot start-up sequence
 *          declared in @ref cache_mpu.h.  Every DMA buffer in the application
 *          is linked into the @c .dma_buffer section, which @c ld/app.ld
 *          places in SRAM2 at 0x2007C000 (16 KB).  One MPU region makes that
 *          whole window normal, non-cacheable and non-shareable memory, so the
 *          CPU and the DMA controller observe the same bytes without any cache
 *          maintenance in the interrupt path (COM-002, DGEN-003).
 *
 *          The order of the steps is deliberate:
 *          -# The MPU is disabled first, so the region registers can be
 *             programmed while no access is checked against a half-written
 *             attribute set.
 *          -# Region 6 is programmed and enabled while both caches are still
 *             off, so no access to the window can be cached between the region
 *             becoming active and the caches coming up.
 *          -# The MPU is then enabled with @c MPU_CTRL_PRIVDEFENA, and only
 *             after that are the I-cache and the D-cache enabled.  Everything
 *             outside the window keeps the architectural default attributes
 *             through the MPU background region, which is what lets code, the
 *             stacks, the FreeRTOS heap and the DTCM .noinit record work
 *             unchanged.
 *
 *          Each register group is separated by a data synchronisation barrier
 *          (@c __DSB) so the writes have completed, followed by an instruction
 *          synchronisation barrier (@c __ISB) so following instructions are
 *          fetched and executed in the new configuration.  The function is
 *          called once from @c main(), after @ref app_clock_init and
 *          before @ref app_uart3_init arms the first DMA channel, and keeps no
 *          state of its own.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DGEN-003 (caches on), COM-002 (DMA buffers)
 * @ingroup cache_mpu
 *
 * @note The ordering and the attribute choice are inherited from the
 *       bootloader contract: BOOT hands over with the caches off, and this
 *       translation unit is the only place that turns them on.
 * @see cache_mpu.h
 */

#include "stm32f7xx_hal.h"
#include "drivers/cache_mpu.h"

/**
 * @brief MPU region number that describes the non-cacheable DMA window.
 * @details The Cortex-M7 in the STM32F767 implements eight MPU regions
 *          (0-7).  Region 6 is the only region this application programs;
 *          regions 0-5 and 7 stay disabled, so privileged accesses to them
 *          fall back to the default memory map through
 *          @c MPU_CTRL_PRIVDEFENA.  The number also appears in the
 *          @c REGION field of @c MPU->RBAR, where a @c VALID write selects the
 *          region by number rather than by the value last written to
 *          @c MPU->RNR, so the two writes agree by construction.
 *
 * @note Changing this value changes @c MPU->RNR, the @c RBAR @c REGION field
 *       and nothing else; the region number is not stored anywhere for later
 *       reference.
 */
#define DMA_MPU_REGION 6U
/**
 * @brief Base address of the non-cacheable DMA window (SRAM2 origin).
 * @details 0x2007C000 is @c ORIGIN(SRAM2) in @c ld/app.ld, whose @c LENGTH is
 *          16 KB, so the window spans 0x2007C000..0x2007FFFF.  The linker
 *          places the @c .dma_buffer output section (32-byte aligned,
 *          @c NOLOAD) in that memory region, which is where
 *          @c uart_rx_ring, @c tx_frame and every other DMA buffer live.
 *
 *          ARMv7-M requires the region base to be aligned to the region size;
 *          0x2007C000 is naturally 16 KB aligned, so the address occupies
 *          only bits [31:5] of @c MPU->RBAR and never collides with the
 *          4-bit @c REGION field in bits [3:0] or the @c VALID flag in bit 4.
 *
 * @warning The address is a literal and is not derived from the linker
 *          symbols.  @c ld/app.ld asserts only that @c .dma_buffer fits in
 *          SRAM2, not that @c ORIGIN(SRAM2) still equals this value, so moving
 *          SRAM2 in the linker script without editing this macro silently
 *          leaves the DMA buffers cacheable again.
 */
#define DMA_MPU_BASE 0x2007C000UL /* SRAM2, 16 KB                  */
/**
 * @brief @c MPU_RASR @c SIZE field encoding for the 16 KB DMA window.
 * @details The ARMv7-M @c SIZE field holds log2(size_in_bytes) - 1, not
 *          log2(size_in_bytes).  For 16 KB that is 14 - 1 = 13, which is why
 *          the macro can be shifted straight into @c MPU_RASR_SIZE_Pos: the
 *          hardware decodes the field as 2^(SIZE + 1) = 2^(13 + 1) = 16384
 *          bytes.  This matches @c LENGTH(SRAM2) in @c ld/app.ld, so the whole
 *          of SRAM2 is covered by one region.
 *
 * @warning Despite the name the value is not log2(bytes).  Reusing it for a
 *          different window needs log2(bytes) - 1, otherwise the region covers
 *          twice or half the intended range and part of @c .dma_buffer becomes
 *          cacheable, or unrelated SRAM is made non-cacheable.
 */
#define DMA_MPU_SIZE_LOG2 13U /* 2^(13+1) = 16 KB             */

/**
 * @brief Configure the non-cacheable DMA MPU region and enable the caches.
 * @details The function performs these steps in order:
 *          -# @c MPU->CTRL = 0 disables the MPU, then @c __DSB() and
 *             @c __ISB() make the disable visible before any region register
 *             is touched.  While the MPU is off the default memory map
 *             applies, so accesses in flight cannot hit a partially
 *             programmed region.
 *          -# @c MPU->RNR selects region @ref DMA_MPU_REGION.
 *          -# @c MPU->RBAR receives @ref DMA_MPU_BASE, the region number in
 *             its @c REGION field and @c MPU_RBAR_VALID.
 *          -# @c MPU->RASR receives the attributes: @c XN=1 (not
 *             executable), @c AP=3 (read/write for privileged and
 *             unprivileged), @c TEX=1, @c C=0, @c B=0 (Normal memory, inner
 *             and outer non-cacheable), @c S=0 (non-shareable), the @c SIZE
 *             field from @ref DMA_MPU_SIZE_LOG2 and @c ENABLE.  A single
 *             @c TEX/C/B combination is used, so the region is
 *             non-cacheable for both the inner and the outer cache policy
 *             and the core never allocates a line for it.
 *          -# @c MPU->CTRL = @c MPU_CTRL_PRIVDEFENA_Msk |
 *             @c MPU_CTRL_ENABLE_Msk enables the MPU together with the
 *             background region, so privileged accesses outside region 6 use
 *             the default memory map instead of faulting.
 *          -# @c __DSB() and @c __ISB() ensure the attributes are active
 *             before the caches are turned on, so the first cacheable access
 *             after this point already sees region 6 as non-cacheable.
 *          -# @c SCB_EnableICache() then @c SCB_EnableDCache() invalidate
 *             their cache by set/way and enable it.  The D-cache comes up
 *             last, after region 6 is already active, so no access to the
 *             window can be cached even transiently.
 *
 *          @par Cache and DMA ordering
 *          Because the whole SRAM2 window is non-cacheable, DMA traffic in
 *          either direction needs no @c SCB_CleanDCache or
 *          @c SCB_InvalidateDCache call: the core never holds a stale or dirty
 *          line for an object in @c .dma_buffer, and the Comms task may read
 *          the USART3 receive ring while the DMA is still writing into it
 *          (COM-002).  Buffers outside the section remain cacheable and their
 *          coherence stays the caller's responsibility.
 *
 * @pre Called from privileged thread mode, before @c vTaskStartScheduler and
 *      before any DMA channel is enabled.  @ref app_clock_init has already
 *      run, so the core is at its final 216 MHz and the flash latency and ART
 *      settings match (DGEN-003).  The caches are off on entry, as handed over
 *      by BOOT.
 * @post The I-cache and the D-cache are enabled, the MPU is enabled with
 *       @c PRIVDEFENA and region 6 active, and every access to
 *       0x2007C000..0x2007FFFF bypasses the D-cache and is non-executable.
 *
 *
 * @note Not ISR-safe and not thread-safe: it is a start-up-only call that
 *       writes shared system registers without any locking.
 * @note Call this exactly once.  A second call re-runs
 *       @c SCB_EnableDCache(), which invalidates the D-cache by set/way and
 *       would discard dirty lines of cacheable memory that are not yet in
 *       SRAM.
 * @note The barriers are the only ordering mechanism: no polling of
 *       @c MPU->TYPE or of the cache status registers is performed, and the
 *       CMSIS helpers carry their own @c __DSB / @c __ISB sequences.
 * @note The @c RBAR and @c RASR writes are not followed by their own barrier;
 *       the single @c __DSB after the @c CTRL enable write orders them ahead
 *       of the region becoming active, which is sufficient because the MPU
 *       was disabled in the first step.
 * @warning Only objects placed in the @c .dma_buffer section are covered by
 *          the non-cacheable region.  Passing a stack, @c .bss or heap buffer
 *          to a DMA driver reintroduces cache incoherence that this module
 *          does not detect.
 * @warning The region is also @c XN, so branching into a DMA buffer raises a
 *          MemManage fault (escalated to HardFault until
 *          @c SCB_SHCSR_MEMFAULTENA is set in @c main()).
 *
 * @see app_clock_init
 * @see app_uart3_init
 * @see cache_mpu.h
 */
void app_cache_mpu_init(void)
{
    MPU->CTRL = 0U;
    __DSB();
    __ISB();

    MPU->RNR  = DMA_MPU_REGION;
    MPU->RBAR = DMA_MPU_BASE | (DMA_MPU_REGION << MPU_RBAR_REGION_Pos) | MPU_RBAR_VALID_Msk;
    /* XN=1 (not executable), AP=full access, TEX=1, C=0, B=0, S=0. */
    MPU->RASR = MPU_RASR_XN_Msk | (3UL << MPU_RASR_AP_Pos) | (1UL << MPU_RASR_TEX_Pos) | (DMA_MPU_SIZE_LOG2 << MPU_RASR_SIZE_Pos) | MPU_RASR_ENABLE_Msk;

    MPU->CTRL = MPU_CTRL_PRIVDEFENA_Msk | MPU_CTRL_ENABLE_Msk;
    __DSB();
    __ISB();

    SCB_EnableICache();
    SCB_EnableDCache();
}
