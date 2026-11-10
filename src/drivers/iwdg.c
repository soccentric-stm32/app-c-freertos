/**
 * @file    iwdg.c
 * @brief   Independent watchdog implementation: start-time detection, early
 *          refresh and steady-state refresh.
 * @details Implements the three entry points of @ref iwdg that are declared in
 *          @ref iwdg.h, writing the IWDG registers directly instead of going
 *          through the HAL IWDG driver.  The module keeps no state of its own:
 *          the counter, prescaler and reload value live in the peripheral,
 *          which sits in the always-on domain, so BOOT, the bootloader
 *          hand-over record and this image all observe one timeout without any
 *          shared RAM.
 *
 *          **Timeout arithmetic.**  The IWDG is clocked by the LSI, nominally
 *          32 kHz and independent of SYSCLK, the PLL and the APB prescalers.
 *          The code programs prescaler /64 (`IWDG_PRESCALER_64`, that is
 *          `PR = 4`) and reload 1999 (@ref IWDG_RELOAD_4S), so the counter
 *          ticks at 32 kHz / 64 = 500 Hz, one count every 2 ms, and expires
 *          after (1999 + 1) * 2 ms = 4.0 s.  The reload value is 1999 and not
 *          2000 because the counter runs from RLR down to 0 inclusive.  The
 *          real LSI has a wide tolerance, so 4 s is a nominal figure and every
 *          timing budget that depends on it must treat it as such.  The window
 *          register `IWDG->WIN` is left at its reset value, so only the lower
 *          bound is enforced and a refresh is legal at any point in the
 *          period.
 *
 *          **Key sequence.**  All control goes through the key register
 *          `IWDG->KR`:
 *          -# 0x5555 (@ref IWDG_KEY_ACCESS) opens write access to `PR`,
 *             `RLR` and `WIN`; the access closes again automatically once
 *             `RLR` has been written;
 *          -# 0xAAAA (`IWDG_KEY_RELOAD`) reloads the counter from `RLR`;
 *          -# 0xCCCC (`IWDG_KEY_ENABLE`) starts the counter, after which it
 *             cannot be stopped by software.
 *          The two value keys come from the HAL header `stm32f7xx_hal_iwdg.h`;
 *          only the access key is defined locally, in @ref IWDG_KEY_ACCESS.
 *
 *          **LSI start and bounded waits.**  On a stand-alone start the LSI
 *          may still be off, in which case the IWDG update flags never clear.
 *          @ref app_iwdg_init therefore starts the LSI first, through
 *          @ref iwdg_start_lsi, and the readiness check and the update-flag
 *          poll are both bounded by @ref IWDG_WAIT_LOOPS so a dead LSI cannot
 *          hang `main()`.  The sequence matches the bootloader so both images
 *          bring the watchdog up the same way.
 *
 *          **Already-running detection.**  @ref app_iwdg_init programs the
 *          peripheral only while `PR` and `RLR` still hold their reset values
 *          (0 and 0xFFF).  Any other combination is taken as proof that BOOT,
 *          or an earlier image in the same power cycle, already owns the
 *          configuration, so the inherited 4 s timeout is preserved
 *          (`HND-004`) instead of being silently reprogrammed.
 *
 *          **Early kick.**  @ref app_iwdg_early_kick refreshes the counter in
 *          the first instructions of `main`, before the clock tree, the
 *          peripherals and the FreeRTOS objects exist, so the BOOT-started 4 s
 *          window also covers the whole start-up sequence (`DBT-002`).
 *
 *          **Single-feeder policy.**  In steady state only the Health task
 *          calls @ref app_iwdg_kick, every 100 ms and only while supervision
 *          holds (`DRT-012`, `HLT-003`).  The two bootstrap helpers above are
 *          pre-scheduler calls and are not part of that feed path: an extra
 *          feeder would mask exactly the stall the watchdog exists to prove.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DBT-002, DRT-012, HLT-003, HND-004
 * @ingroup iwdg
 * @see iwdg.h
 * @see health_task.c
 */

#include <stdbool.h>
#include "stm32f7xx_hal.h"
#include "drivers/iwdg.h"

/**
 * @brief IWDG register access key, written to `IWDG->KR` as 0x5555.
 * @details Opens write access to the prescaler, reload and window registers.
 *          The access is re-closed by hardware after the write to `RLR`, so
 *          the sequence unlock, write `PR`, write `RLR` needs no explicit
 *          re-lock step.  The value is repeated here, under a local name,
 *          because the module owns the configuration sequence and must stay
 *          readable without the HAL key macros; the reload (0xAAAA) and
 *          enable (0xCCCC) keys are used straight from `stm32f7xx_hal_iwdg.h`.
 * @note The key register is 16 bits wide inside a 32-bit register, so the
 *       upper half of the constant is zero by definition.
 * @see IWDG_RELOAD_4S
 */
#define IWDG_KEY_ACCESS 0x00005555UL

/**
 * @brief Reload value that produces the nominal 4.0 s timeout.
 * @details With the LSI at 32 kHz and prescaler /64 the counter advances one
 *          step every 2 ms, and the period is (RLR + 1) * 2 ms because the
 *          counter decrements through 0.  1999 therefore gives
 *          (1999 + 1) * 2 ms = 4.0 s, the same timeout BOOT programs
 *          (`TRY-004`, `HND-004`).  The value is a 12-bit field, so the range
 *          that can be represented in `RLR` is 0 to 4095, that is 2 ms to
 *          8.192 s with this prescaler.
 * @note The constant is nominal only: the LSI tolerance dominates the real
 *       period, and the application must not tighten a timing budget on the
 *       assumption of exactly 4.0 s.
 * @warning Changing the value changes the hand-over contract and the reset
 *          latency that `HLT-003` and `DRT-012` rely on.
 * @see IWDG_KEY_ACCESS
 * @see app_iwdg_init
 */
#define IWDG_RELOAD_4S 1999U

/**
 * @brief Bound on the LSI-ready and IWDG update-flag polls.
 * @details About 40 ms at 216 MHz, generous for the roughly 160 us the LSI
 *          and an update transfer actually need, but short enough that a dead
 *          LSI or a stuck update flag cannot hang the start-up.  The same
 *          limit is used by @ref iwdg_start_lsi and @ref iwdg_wait_ready and
 *          matches the bootloader's bound, so both images fail the same way.
 * @see iwdg_start_lsi
 * @see iwdg_wait_ready
 */
#define IWDG_WAIT_LOOPS 1000000UL

/**
 * @brief Start the LSI and wait, with a bound, for it to become ready.
 * @details Sets `RCC->CSR` @c LSION and polls @c LSIRDY for at most
 *          @ref IWDG_WAIT_LOOPS iterations.  The IWDG update flags PVU, RVU
 *          and WVU are clocked from the LSI, so they can never clear while
 *          the oscillator is off; on a stand-alone start this call must
 *          therefore run before `PR` and `RLR` are written.  When BOOT is
 *          present the LSI is already running and both the set and the ready
 *          bit are no-ops.
 *
 * @pre  Called from @ref app_iwdg_init before the watchdog registers are
 *       programmed.
 * @post The LSI is enabled; @c LSIRDY is set when it started within the
 *       bound.  On timeout the function returns anyway and the caller
 *       proceeds, so a dead LSI leaves a watchdog that may not run instead of
 *       hanging `main()` forever.
 *
 * @note Bounded busy-wait of at most @ref IWDG_WAIT_LOOPS iterations; no
 *       interrupt, HAL call or allocation is involved.
 * @warning Blocking.  Start-up only, before the scheduler exists.
 * @see iwdg_wait_ready
 * @see app_iwdg_init
 */
static void iwdg_start_lsi(void)
{
    RCC->CSR |= RCC_CSR_LSION;
    for(uint32_t i = 0U; i < IWDG_WAIT_LOOPS; i++)
    {
        if((RCC->CSR & RCC_CSR_LSIRDY) != 0U)
        {
            return;
        }
    }
}

/**
 * @brief Spin, with a bound, until the IWDG shadow registers have updated.
 * @details Polls `IWDG->SR` and returns once the prescaler update flag (PVU),
 *          the reload update flag (RVU) and the watchdog enable / voltage
 *          domain update flag (WVU) are all clear.  Each flag is raised by
 *          hardware when the corresponding write is transferred from the APB
 *          shadow register into the LSI clock domain; while a flag is set the
 *          matching register ignores further writes, so the wait is mandatory
 *          between programming `PR`/`RLR` and issuing the reload and enable
 *          keys.  The loop runs at most @ref IWDG_WAIT_LOOPS iterations, so
 *          a dead LSI cannot block the start-up indefinitely.
 *
 * @pre  Write access has been opened with @ref IWDG_KEY_ACCESS and new values
 *       have been stored to `IWDG->PR` and `IWDG->RLR`.
 * @post PVU, RVU and WVU are clear and the programmed configuration is
 *       visible to the LSI domain, so the reload and enable keys take effect.
 *       On timeout the flags may still be set; the caller continues with the
 *       configuration that did land rather than waiting forever.
 *
 * @note The transfer costs a few LSI periods, tens of microseconds at most,
 *       and the function is only called from @ref app_iwdg_init during
 *       start-up, before the scheduler exists.
 * @note Bounded by @ref IWDG_WAIT_LOOPS, matching the bootloader: a dead LSI
 *       or a stuck flag must not hang `main()`.
 * @warning Blocking.  Must not be called from an ISR or from a task once the
 *          scheduler is running, and requires write access to `PR`/`RLR` to
 *          still be open, otherwise the flags are already clear and the wait
 *          returns immediately on a configuration that was never applied.
 * @see iwdg_start_lsi
 * @see app_iwdg_init
 */
static void iwdg_wait_ready(void)
{
    for(uint32_t i = 0U; i < IWDG_WAIT_LOOPS; i++)
    {
        if((IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU)) == 0U)
        {
            return;
        }
    }
}

/**
 * @brief Refresh the IWDG as early as possible in application start-up.
 * @details Writes the reload key to `IWDG->KR` once.  Neither the prescaler
 *          nor the reload register is touched and no update flag is polled, so
 *          the timeout BOOT programmed survives unchanged.  `main()` calls
 *          this immediately after setting `SCB->VTOR` to @ref APP_SLOT_BASE
 *          (`DBT-002`), which buys the whole clock and peripheral
 *          initialisation time before the 4 s window that BOOT started can
 *          expire.  The call performs no blocking, no allocation and no HAL
 *          call, and touches only a register clocked by the always-on LSI, so
 *          it is safe before the clocks, the HAL and the NVIC are configured;
 *          it is equally safe before `app_iwdg_init` has run.
 *
 * @pre  BOOT has started the IWDG with the 4 s timeout (`HND-004`), so the
 *       counter is running and the reload key has an effect.  No other context
 *       touches the watchdog at this point in start-up.
 * @post The nominal 4 s period restarts from the moment of the call; the
 *       prescaler, reload value and window configuration are unchanged.
 *
 * @note When DEMO runs stand-alone (no BOOT) the counter is still stopped, so
 *       this write is accepted by the key register but discarded; the window
 *       from reset until @ref app_iwdg_init is then uncovered, which is
 *       acceptable only because that mode exists for debugging.
 * @note This is a start-up helper, not a feed source: it is called once, from
 *       `main`, and never from a task.
 * @warning Do not use it as a periodic refresh.  Feeding from anywhere but the
 *          Health task defeats the single-feeder policy (`DRT-012`,
 *          `HLT-003`).
 * @see app_iwdg_init
 * @see app_iwdg_kick
 */
void app_iwdg_early_kick(void) { IWDG->KR = IWDG_KEY_RELOAD; }

/**
 * @brief Start the IWDG at 4 s if BOOT did not, then refresh it once.
 * @details Detects whether the peripheral is already configured by reading
 *          `IWDG->PR` and `IWDG->RLR`, whose reset values are 0 (/4) and 0xFFF
 *          (4095):
 *          -# `PR == 0` and `RLR == 0xFFF` means no stage has programmed the
 *             watchdog yet, so the function first starts the LSI with
 *             @ref iwdg_start_lsi and waits for it, then enables the counter
 *             with @ref IWDG_KEY_ENABLE, opens register access with
 *             @ref IWDG_KEY_ACCESS, writes `IWDG_PRESCALER_64` to `PR` and
 *             @ref IWDG_RELOAD_4S to `RLR`, waits for the update flags with
 *             the bounded @ref iwdg_wait_ready, and reloads with
 *             @ref IWDG_KEY_RELOAD;
 *          -# any other value means BOOT, or a previous application image,
 *             already configured the watchdog, so the registers are left
 *             untouched to preserve the inherited 4 s timeout.
 *
 *          In both cases the call ends with @ref app_iwdg_kick, so the period
 *          restarts from a known point when `main()` continues and the time
 *          spent in this function is not charged to the caller.  The function
 *          is idempotent with respect to the timeout: repeated calls after the
 *          first programming take the already-running branch.
 *
 * @pre  Called once from `main()` after `app_clock_init()` and
 *       `app_handover_read()`, and always before `vTaskStartScheduler()`; no
 *       other context touches the IWDG at that point, so the read, the
 *       reprogramming and the two keys cannot interleave with another writer.
 * @post The IWDG is running with a 4 s nominal LSI timeout and has just been
 *       reloaded; `PR` and `RLR` no longer hold their reset values, so a later
 *       call detects the configured peripheral.
 *
 * @note Leaving the registers alone when the watchdog is already configured is
 *       what keeps the hand-over contract intact: `HND-004` gives BOOT the
 *       authority to choose the configuration, and DEMO must not silently
 *       change it.
 * @note The LSI counts through `app_clock_init()`, so a slow or retried clock
 *       tree configuration is covered by the BOOT-started timeout rather than
 *       by a watchdog that only starts afterwards.
 * @note On a stand-alone start the LSI and the update-flag waits are bounded
 *       by @ref IWDG_WAIT_LOOPS, so a failing oscillator cannot hang `main()`.
 *       The same sequence is used by the bootloader.
 * @warning The detection can only recognise a *programmed* watchdog.  A stage
 *          that enables the counter without writing `PR` and `RLR` keeps the
 *          reset values (/4, 4095, about 512 ms), looks unconfigured here and
 *          is reprogrammed - the same blind spot documented in @ref iwdg.h.
 * @warning Must not be used as an extra feed source after the scheduler is
 *          running; calling it from a task would reset the timeout to the
 *          inherited value and break the single-feeder policy.
 * @see app_iwdg_early_kick
 * @see app_iwdg_kick
 * @see iwdg_wait_ready
 */
void app_iwdg_init(void)
{
    /* PR == 0 and RLR == 0xFFF is the reset state; if they differ, BOOT (or
     * a previous stage) has already configured and started the watchdog. */
    bool running = (IWDG->PR != 0U) || (IWDG->RLR != 0x0FFFU);

    if(!running)
    {
        iwdg_start_lsi(); /* update flags need a running LSI */
        IWDG->KR  = IWDG_KEY_ENABLE;
        IWDG->KR  = IWDG_KEY_ACCESS; /* open write access to PR and RLR */
        IWDG->PR  = IWDG_PRESCALER_64;
        IWDG->RLR = IWDG_RELOAD_4S;
        iwdg_wait_ready(); /* bounded: PR/RLR visible for reload */
        IWDG->KR = IWDG_KEY_RELOAD;
    }

    app_iwdg_kick();
}

/**
 * @brief Refresh the IWDG, deferring a reset by another 4 s.
 * @details Writes the reload key 0xAAAA to `IWDG->KR`, which copies `RLR` into
 *          the down-counter.  The function is unconditional and stateless: it
 *          inspects neither the task check-in table nor the health state,
 *          because that decision belongs to the caller.  It performs no
 *          blocking, no allocation and no HAL call, and completes in a single
 *          register store, so a reload can never be lost to a fault during a
 *          longer sequence.
 *
 * @pre  The IWDG is running: BOOT starts it before hand-over, and
 *       @ref app_iwdg_init guarantees it before the scheduler starts.
 * @post The watchdog period restarts; a reset is deferred by one nominal 4 s
 *       timeout.
 *
 * @note Health is the only steady-state caller (`HLT-003`).  It refreshes
 *       every 100 ms while all supervised tasks have checked in within the
 *       last 2 s and the pending-slot health check has not failed, so the
 *       4 s timeout absorbs about 40 missed refreshes before the board resets.
 * @note No return value and no error path: the reload key is always accepted,
 *       and a write while the counter is stopped is silently ignored, so the
 *       function cannot report whether the watchdog was actually running.
 * @warning Task context only, and only from the Health task.  Feeding from
 *          another task or from an ISR hides exactly the stall the watchdog
 *          exists to catch and turns a lost supervision event into an
 *          undetected hang (`DRT-012`, `HLT-003`).
 * @see health_task.c
 * @see app_iwdg_init
 */
void app_iwdg_kick(void) { IWDG->KR = IWDG_KEY_RELOAD; }
