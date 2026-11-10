/**
 * @file    clock.c
 * @brief   Bring-up of the 216 MHz system clock tree (DGEN-003).
 * @details Takes the STM32F767ZIT6 from the reset clock state left by BOOT
 *          (DBT-003) to the tree required by `DGEN-003`: the 8 MHz ST-LINK HSE
 *          in bypass mode feeds the main PLL, SYSCLK runs from PLLCLK at
 *          216 MHz with over-drive active in voltage scale 1, seven flash wait
 *          states, and the prefetch buffer and ART accelerator enabled.  With
 *          the bus prescalers from @ref app_board.h the resulting tree is:
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
 *          The file keeps no state and defines no macros: the only file-scope
 *          symbol is the @ref clock_fail helper and both initialization
 *          structures are function-local.  The instruction and data caches and
 *          the non-cacheable DMA MPU window are not touched here even though
 *          `DGEN-003` lists them with this configuration;
 *          @ref app_cache_mpu_init enables them afterwards.
 *
 *          The code is a copy of the validation firmware's `src/clock.c`;
 *          `DGEN-007` asks for a shared copy, which is still OPEN-D02.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6 rev Z @ 3.3 V, 8 MHz HSE bypass, voltage scale 1
 * @srs     DGEN-003 (same configuration as HWVAL GEN-004), DBT-003
 * @ingroup clock
 * @see     clock.h, app_board.h
 */

#include "stm32f7xx_hal.h"
#include "app_board.h"
#include "services/app_fault.h"
#include "drivers/clock.h"

/**
 * @brief   Latch a clock set-up failure as a start-up fault and reset.
 * @details The only file-scope symbol of this module.  Records
 *          @ref APP_FAULT_STARTUP with the task name `"clock"`, the fixed stage
 *          label `"HSE/PLL/over-drive"` and line 0 through @ref app_fault_fatal,
 *          which saves the record in .noinit, blinks LD3 and resets the MCU.
 *          Nothing is unwound and no clock register is restored: the caller is
 *          a start-up routine with no scheduler and no tasks to return to.
 *
 * @pre     Called only from @ref app_clock_init, at a point where the core may
 *          still run from the 16 MHz HSI with the PLL, the over-drive or the
 *          dividers only partially programmed.
 * @post    The MCU resets; this function never returns to its caller.
 *
 * @note    The stage label is a compile-time constant, so a failing
 *          @c HAL_RCC_ClockConfig() or @c HAL_PWREx_EnableOverDrive() is
 *          recorded with the same text as a failing HSE or PLL start.  Read the
 *          other fields of the fault record, not this label, to tell them
 *          apart.
 * @warning The reset path is deliberately independent of this clock tree, so
 *          it also works when the PLL never locked.
 * @see     app_fault_fatal, app_fault_show_and_reset
 */
static void clock_fail(void) __attribute__((noreturn));
static void clock_fail(void) { app_fault_fatal(APP_FAULT_STARTUP, "clock", "HSE/PLL/over-drive", 0U); }

/**
 * @brief   Program PWR, HSE, the main PLL, over-drive, the bus prescalers and
 *          the flash interface, in the order the hardware requires.
 * @details The device enters with SYSCLK = HSI at 16 MHz and the flash
 *          interface at its reset latency.  That state belongs to BOOT, so
 *          nothing is inherited: both @c RCC_*InitTypeDef objects are zero
 *          initialized and then written field by field (DBT-003).
 *
 *          Steps, and why they have to come in this order:
 *          -# @c __HAL_RCC_PWR_CLK_ENABLE() gates the PWR interface clock on
 *             APB1.  Without it the PWR_CR1 writes of the next two steps are
 *             dropped, so voltage scaling and over-drive would silently do
 *             nothing.
 *          -# Voltage scale 1 is selected before any frequency is raised, so
 *             the regulator can supply the core at 216 MHz before HCLK gets
 *             there.  VOSRDY is deliberately not polled, see @note.
 *          -# HSE is started in bypass mode from the ST-LINK MCO (`DCON-005`)
 *             and the main PLL is enabled and locked.  HSI is requested off,
 *             but it is still the SYSCLK source here and therefore keeps
 *             running until the switch in step 5.
 *          -# Over-drive is enabled while SYSCLK is still 16 MHz, because
 *             voltage scale 1 alone covers only 180 MHz.  The HAL waits for
 *             ODRDY and then for ODSWRDY and reports a timeout through its
 *             return value; that is a hard failure here.
 *          -# SYSCLK is switched to PLLCLK and the AHB/APB dividers are
 *             programmed in the same call as the new flash latency.  The HAL
 *             writes the latency before it throws the switch, so the flash
 *             interface is never addressed faster than it can answer.
 *          -# Prefetch and the ART accelerator are enabled last, once the
 *             final latency is in place.
 *
 *          PLL math from the 8 MHz HSE: `f(PLLin) = 8 MHz / PLLM(8) = 1 MHz`
 *          is the reference the wide-band VCO needs, `f(VCO) = 1 MHz *
 *          PLLN(432) = 432 MHz` sits at the top of the 100..432 MHz VCO range,
 *          `f(SYSCLK) = 432 MHz / PLLP(2) = 216 MHz` is the F767 maximum at
 *          voltage scale 1 with over-drive, and `f(PLLQ) = 432 MHz / PLLQ(9) =
 *          48 MHz` is left running for USB, SDMMC and RNG although this
 *          application uses none of them.
 *
 *          Flash latency: `FLASH_LATENCY_7` is seven wait states, the value
 *          [DS11532] specifies for 210 MHz < HCLK <= 216 MHz at 3.3 V in
 *          voltage scale 1.  Changing PLLN, PLLP or the AHB divider needs a
 *          matching latency, otherwise the core reads stale or wrong
 *          instructions from flash.
 *
 *          The prescalers are not local constants: APB1 and APB2 come from
 *          @ref app_board.h so the board clock contract lives in one place.
 *          AHB /1 keeps HCLK at 216 MHz, APB1 /4 gives 54 MHz with 108 MHz
 *          timer clocks, APB2 /2 gives 108 MHz with 216 MHz timer clocks; both
 *          APB buses end up at their datasheet maximum.
 *
 *          Side effects: @c HAL_RCC_ClockConfig() updates the HAL core clock
 *          variables and re-invokes @c HAL_InitTick().  In this project the
 *          override in @ref hal_timebase.c reprograms TIM6 for the new PCLK1
 *          (DGEN-006), so the 1 ms HAL tick survives the frequency change and
 *          TIM6_DAC_IRQn is re-armed at the HAL tick priority.  Apart from
 *          PWR, RCC, FLASH and that tick interrupt no peripheral is touched and
 *          no interrupt is enabled or disabled.  The only waits are the HAL
 *          timeouts of the oscillator, PLL lock and over-drive start-up; the
 *          function blocks on nothing else and keeps no state.
 *
 * @pre   HAL_Init() has completed and the HAL tick already runs from TIM6, so
 *        SYSCLK is HSI at 16 MHz and the flash latency is at its reset value.
 * @pre   No peripheral clocked from PCLK1 or PCLK2 has been initialized yet,
 *        because their dividers change here.
 * @pre   Called exactly once, from main(), before the scheduler starts and
 *        before any timing-dependent set-up (UART baud rate, IWDG, SysTick).
 * @post  SYSCLK = PLLCLK at 216 MHz from the 8 MHz HSE bypass, HCLK = 216 MHz,
 *        PCLK1 = 54 MHz, PCLK2 = 108 MHz, PLLQ = 48 MHz, flash latency seven
 *        wait states, prefetch and ART enabled, over-drive active in voltage
 *        scale 1.
 * @post  I-cache, D-cache and the MPU are still off; @ref app_cache_mpu_init
 *        enables them next.
 *
 *
 * @warning Not re-entrant and not idempotent by design: call it once, from
 *          single-threaded start-up.
 * @warning On any HAL error the function calls @ref clock_fail and never
 *          returns, so a caller cannot continue with a reduced clock tree.
 * @warning @c configCPU_CLOCK_HZ in @ref FreeRTOSConfig.h is 216000000 and must
 *          keep matching this tree; the port derives the SysTick reload from it
 *          (DRT-001).
 * @note  VOSRDY is not polled, matching the validation firmware (`CL-009`): it
 *        does not assert before the PLL is up in this configuration.  The
 *        over-drive enable and the HAL timeouts of the clock switch cover the
 *        practical risk.
 * @note  `HND-001`/`DBT-003`: the reset clock state is defined by BOOT, hence
 *        the explicit field-by-field programming.
 * @note  The configuration matches HWVAL `GEN-004`; the shared copy requested
 *        by `DGEN-007` does not exist yet (OPEN-D02), so the code is duplicated
 *        in this application.
 * @see   app_cache_mpu_init, @ref clock, clock.h
 */
void app_clock_init(void)
{
    /* PWR register access needs the PWR interface clock (APB1) first. */
    __HAL_RCC_PWR_CLK_ENABLE();

    /* Voltage scale 1 before raising the frequency, so the regulator can feed
     * the core at 216 MHz. As in HWVAL (CL-009), VOSRDY is not polled: it does
     * not set before the PLL is up. */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* HSE bypass and main PLL.  Every field is written explicitly because BOOT
     * owns the reset state (DBT-003). */
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType     = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState           = RCC_HSE_BYPASS; /* ST-LINK MCO 8 MHz (DCON-005) */
    osc.HSIState           = RCC_HSI_OFF;
    osc.PLL.PLLState       = RCC_PLL_ON;
    osc.PLL.PLLSource      = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM           = 8;             /* VCO in: 1 MHz               */
    osc.PLL.PLLN           = 432;           /* VCO: 432 MHz                */
    osc.PLL.PLLP           = RCC_PLLP_DIV2; /* SYSCLK: 216 MHz             */
    osc.PLL.PLLQ           = 9;             /* PLLQ: 48 MHz                */

    /* HSE missing (no ST-LINK MCO) or the PLL did not lock; the tree cannot
     * be reached, so latch the start-up fault and reset. */
    if(HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        clock_fail();
    }

    /* 216 MHz needs over-drive in voltage scale 1 ([DS11532]).  It has to be
     * active before SYSCLK passes 180 MHz, so it is enabled now, while the core
     * still runs from the 16 MHz HSI; the HAL polls ODRDY and ODSWRDY and
     * reports HAL_TIMEOUT when the regulator does not switch. */
    if(HAL_PWREx_EnableOverDrive() != HAL_OK)
    {
        clock_fail();
    }

    /* SYSCLK source, AHB divider and both APB dividers are programmed by one
     * call, so no intermediate tree is ever clocked. */
    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType          = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource       = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider      = RCC_SYSCLK_DIV1;    /* HCLK 216 MHz           */
    clk.APB1CLKDivider     = APP_APB1_PRESCALER; /* 54 MHz, timers 108 MHz */
    clk.APB2CLKDivider     = APP_APB2_PRESCALER; /* 108 MHz, timers 216 MHz */

    /* HAL_RCC_ClockConfig() raises the latency to 7 wait states first, then
     * switches SYSCLK to PLLCLK and applies the dividers; it also re-runs
     * HAL_InitTick() so TIM6 is rescaled to the new PCLK1 (DGEN-006). */
    if(HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_7) != HAL_OK)
    {
        clock_fail();
    }

    /* DGEN-003: prefetch and the ART accelerator, enabled once the final
     * latency is programmed. */
    __HAL_FLASH_PREFETCH_BUFFER_ENABLE();
    __HAL_FLASH_ART_ENABLE();
    /* Caches and the non-cacheable DMA MPU region are enabled together in
     * app_cache_mpu_init() after this function. */
}
