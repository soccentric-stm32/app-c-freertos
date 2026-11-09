/**
 * @file    clock.h
 * @brief   Public interface to the 216 MHz system clock tree (DGEN-003).
 * @details Declares the single entry point that takes the STM32F767ZIT6 from
 *          the reset clock state left by BOOT to the application clock tree
 *          required by `DGEN-003`.  The resulting tree is:
 *
 *          | Domain | Derivation          | Frequency | Notes             |
 *          |--------|---------------------|-----------|-------------------|
 *          | HSE    | ST-LINK MCO, bypass | 8 MHz     | `DCON-005`        |
 *          | PLL in | HSE / PLLM (8)      | 1 MHz     | PLL reference     |
 *          | VCO    | 1 MHz * PLLN (432)  | 432 MHz   |                   |
 *          | SYSCLK | VCO / PLLP (2)      | 216 MHz   | PLLCLK            |
 *          | HCLK   | SYSCLK / 1          | 216 MHz   | AHB, core, flash  |
 *          | PCLK1  | HCLK / 4            | 54 MHz    | timers at 108 MHz |
 *          | PCLK2  | HCLK / 2            | 108 MHz   | timers at 216 MHz |
 *          | PLLQ   | VCO / 9             | 48 MHz    | USB/SDMMC/RNG     |
 *
 *          The same routine enables over-drive in voltage scale 1 and
 *          programs seven flash wait states, the combination ST documents
 *          for 216 MHz at 3.3 V ([DS11532]).
 *
 *          The module keeps no state and is not re-entrant; it is expected to
 *          run exactly once during start-up.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     DGEN-003, DBT-003
 *
 * @ingroup clock
 */

#ifndef CLOCK_H
#define CLOCK_H

/**
 * @defgroup clock System clock tree
 * @brief 216 MHz core clock derived from the 8 MHz ST-LINK HSE.
 * @details Groups the start-up clock configuration described in @ref clock.h.
 *          Consumers only need to call @ref app_clock_init; no other symbol of
 *          this module is exported.
 * @{
 */

/**
 * @brief   Configure the clock tree from the reset state BOOT leaves.
 * @details Programs PWR, HSE, the main PLL, the bus prescalers and the flash
 *          interface in the order the reference manual requires:
 *          -# enable the PWR interface clock on APB1 so PWR registers respond;
 *          -# select voltage scale 1 before raising the frequency;
 *          -# start HSE in bypass mode and the main PLL with PLLM = 8,
 *             PLLN = 432, PLLP = 2 and PLLQ = 9, HSI off;
 *          -# enable over-drive, required above 180 MHz in voltage scale 1;
 *          -# switch SYSCLK to PLLCLK with AHB /1, APB1 /4 and APB2 /2 and
 *             seven flash wait states;
 *          -# enable the flash prefetch buffer and the ART accelerator.
 *
 *          On success the core and HCLK run at 216 MHz, PCLK1 at 54 MHz
 *          (APB1 timers 108 MHz) and PCLK2 at 108 MHz (APB2 timers 216 MHz).
 *          The 48 MHz PLLQ output is left running for USB, SDMMC and RNG even
 *          though the demo does not use those peripherals.
 *
 * @pre   HAL_Init() has completed, so the device still runs from the 16 MHz
 *        HSI and the flash interface is at its reset latency.
 * @pre   No peripheral clocked from PCLK1/PCLK2 (UART, IWDG, timers) has been
 *        initialised yet, because their dividers change here.
 * @post  SYSCLK is PLLCLK at 216 MHz, HSE is the PLL source and the flash
 *        latency is 7 wait states with prefetch and ART enabled.
 * @post  The data/instruction caches and the non-cacheable DMA MPU region are
 *        still disabled; app_cache_mpu_init() enables them afterwards.
 *
 * @warning Call this before any timing-sensitive initialisation (UART baud
 *          rate, IWDG timeout, SysTick).  A HAL error is not recoverable: the
 *          fault is latched as APP_FAULT_STARTUP via app_fault_fatal() and the
 *          CPU never returns to the caller.
 *
 * @note `HND-001`: the reset clock state belongs to BOOT, so every field this
 *       function depends on is written explicitly instead of assumed.
 * @note VOSRDY is deliberately not polled: it does not assert before the PLL
 *       is up, matching the validation firmware (`CL-009`).
 * @note The configuration matches HWVAL `GEN-004`; the shared copy requested
 *       by `DGEN-007` (OPEN-D02) does not exist yet, so the code is duplicated
 *       in this application.
 *
 * @see    app_cache_mpu_init
 */
void app_clock_init(void);

/** @} */

#endif /* CLOCK_H */
