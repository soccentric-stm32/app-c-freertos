/**
 * @file    iwdg.h
 * @brief   Independent watchdog service and its single-feeder contract.
 * @details The IWDG is clocked by the LSI, independent of SYSCLK, and cannot
 *          be stopped once started.  It is therefore the last-resort recovery
 *          path for a hung task, a dead scheduler or a corrupted clock tree.
 *          BOOT starts it with a 4 s timeout before hand-over (`HND-004`);
 *          the application only refreshes that instance, and starts one
 *          itself solely when it is run stand-alone for debugging.  On that
 *          path it first starts the LSI and both the LSI-ready and IWDG
 *          update-flag waits are bounded, so a dead oscillator cannot hang
 *          start-up.
 *
 *          **Timeout.**  The IWDG configuration used by both stages is LSI
 *          (~32 kHz) with a /64 prescaler and a reload value of 1999, that is
 *          2000 * 64 / 32 kHz = 4.0 s nominal.  The private macro
 *          `IWDG_RELOAD_4S` in @ref iwdg.c holds the value; this header
 *          documents the contract only.  The LSI tolerance is wide, so 4 s is
 *          a nominal figure and must be treated as such by timing budgets.
 *          The window feature is unused: `IWDG->WIN` keeps its reset value,
 *          so only the lower bound is enforced and a refresh may happen at
 *          any time within the period.
 *
 *          **Single-feeder policy.**  The Health task is the only steady-state
 *          feeder.  It calls @ref app_iwdg_kick every 100 ms while every
 *          supervised task has checked in within the last 2 s and the
 *          pending-slot health check has not failed.  When supervision is
 *          lost, or when the health check has failed for 10 s, it stops
 *          refreshing and the 4 s timeout resets the board (`HLT-003`).
 *          Refreshing from any other task would mask a stalled Health task,
 *          so the reset would no longer prove that supervision was lost.
 *
 *          @ref app_iwdg_early_kick and @ref app_iwdg_init are pre-scheduler
 *          bootstrap helpers only; they are not part of the feed path.
 *
 * @warning No `DBG_IWDG_STOP` freeze bit is programmed in `DBGMCU`, so
 *          halting the core under a debugger for longer than the timeout
 *          resets the board.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DBT-002, DRT-012, HLT-003, HND-004
 *
 * @ingroup iwdg
 */

#ifndef IWDG_H
#define IWDG_H

/**
 * @defgroup iwdg Independent watchdog service
 * @brief 4 s LSI watchdog refreshed by the Health task only.
 * @details Exposes the three watchdog entry points: one early refresh from
 *          the first instructions of `main`, one configuration step after the
 *          clock tree is up, and the steady-state refresh owned by Health.
 *          The module keeps no state of its own; the counter lives in the
 *          IWDG peripheral, which is why BOOT and the application can share
 *          one timeout without any shared variable.
 * @see health_task.c
 * @{
 */

/**
 * @brief   Refresh the IWDG as early as possible in application start-up.
 * @details Performs a single write of the reload key (0xAAAA) to `IWDG->KR`.
 *          It touches neither the prescaler nor the reload register and does
 *          not wait for the update flags, so the timeout BOOT programmed is
 *          kept unchanged.  `main()` calls this as the first statement after
 *          `SCB->VTOR` is set to @ref APP_SLOT_BASE (`DBT-002`), which buys
 *          roughly the whole clock and peripheral initialisation time before
 *          the 4 s window that BOOT started can expire.
 *
 * @pre  BOOT has started the IWDG with the 4 s timeout (`HND-004`), so the
 *       counter is running and can be reloaded.
 * @post The 4 s period restarts from the moment of the call.
 *
 * @note Safe before the HAL, the system clocks and the interrupt controller
 *       are up: the function touches only the IWDG key register, whose kernel
 *       clock is the always-on LSI, and it neither blocks nor waits for a
 *       flag.
 * @warning Do not use this as a periodic feed.  When the IWDG is not running
 *          the reload write is accepted by the key register but has no
 *          effect, so this call is not a substitute for a running watchdog.
 * @see app_iwdg_init
 */
void app_iwdg_early_kick(void);

/**
 * @brief   Start the IWDG at 4 s if BOOT did not, then refresh it once.
 * @details Detects the reset state of the peripheral by reading `IWDG->PR`
 *          and `IWDG->RLR`:
 *          -# `PR == 0` and `RLR == 0xFFF` means no stage has configured the
 *             watchdog, so the function first starts the LSI and waits, with
 *             a bound, for it to become ready, enables the counter with the
 *             enable key (0xCCCC), unlocks the registers with the access key
 *             (0x5555), programs prescaler /64 and reload 1999, waits with a
 *             bound for the PVU/RVU/WVU update flags to clear, and writes
 *             the reload key (0xAAAA);
 *          -# any other value means BOOT, or a previous application image,
 *             already configured and started it, so the configuration is
 *             left untouched to preserve the inherited 4 s timeout.
 *
 *          In both cases the call ends with @ref app_iwdg_kick, so the period
 *          restarts from a known point when `main()` continues.
 *
 * @pre  Called once from `main()` after the clock tree and the backup-domain
 *       hand-over record have been read, and always before
 *       `vTaskStartScheduler()`; no other context touches the IWDG at that
 *       point.
 * @post The IWDG is running with a 4 s nominal LSI timeout and is freshly
 *       reloaded.
 *
 * @note Leaving the registers alone when the watchdog is already running is
 *       what keeps the hand-over contract intact: `HND-004` gives BOOT the
 *       authority to choose the configuration, and DEMO must not silently
 *       change it.
 * @note The LSI keeps counting through `app_clock_init()`, so a slow or
 *       retried clock tree configuration is covered by the BOOT-started
 *       timeout rather than by a watchdog that starts afterwards.
 * @note The stand-alone start sequence follows the bootloader: LSI on, wait
 *       ready, enable, unlock, program `PR`/`RLR`, wait for the update flags,
 *       reload.  Every wait is bounded by a fixed loop count, so a dead LSI
 *       leaves a possibly inactive watchdog instead of blocking `main()`.
 * @warning The detection can only recognise a *configured* watchdog.  A
 *          stage that starts the counter without writing `PR` and `RLR`
 *          (reset values /4 and 4095, about 512 ms) looks stopped here and
 *          would be re-programmed.
 * @warning Must not be used as an extra feed source: calling it from a task
 *          after the scheduler is running violates the single-feeder policy.
 * @see app_iwdg_early_kick
 * @see app_iwdg_kick
 */
void app_iwdg_init(void);

/**
 * @brief   Refresh the IWDG, deferring a reset by another 4 s.
 * @details Writes the reload key (0xAAAA) to `IWDG->KR`.  The function is
 *          unconditional: it does not inspect task state, the check-in table
 *          or the health state, because that decision belongs to the caller.
 *          It performs no blocking, no allocation and no HAL call.
 *
 * @pre  The IWDG is running: BOOT starts it before hand-over, and
 *       @ref app_iwdg_init guarantees it before the scheduler starts.
 * @post The watchdog period restarts; a reset is deferred by one nominal 4 s
 *       timeout.
 *
 * @note Health calls this only while every supervised task has checked in
 *       within the last 2 s, and stops when supervision is lost or the
 *       pending-slot health check has failed (`HLT-003`).  The refresh
 *       cadence there is 100 ms, so the 4 s timeout absorbs about 40 missed
 *       refreshes before the board resets.
 * @warning Task context only, and only from the Health task.  Feeding the
 *          watchdog from another task or from an ISR hides exactly the stall
 *          the watchdog exists to catch, and turns a lost supervision event
 *          into an undetected hang (`DRT-012`, `HLT-003`).
 * @warning Does not verify that the IWDG was started; a reload write while
 *          the counter is stopped has no effect and reports no error.
 * @see health_task.c
 * @see app_iwdg_init
 */
void app_iwdg_kick(void);

/** @} */

#endif /* IWDG_H */
