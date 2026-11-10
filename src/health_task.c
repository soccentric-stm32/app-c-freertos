/**
 * @file    health_task.c
 * @brief   Task supervision, watchdog feeding and trial CONFIRMED write.
 * @details Implementation of @ref health_task.h and the only steady-state
 *          caller of @ref app_iwdg_kick.  The module has three jobs:
 *
 *          -# **Check-in bookkeeping.**  Every supervised task calls
 *             @ref health_checkin, which sets one bit in this task's
 *             notification value.  The 20 ms loop drains those bits, bumps a
 *             per-task counter and stores the tick at which the task was last
 *             seen.  Health stamps its own slot in the same call, so the
 *             accounting also proves that this loop is running (`DRT-012`).
 *          -# **Supervision.**  If any slot, Health included, has not been
 *             seen for more than 2 s, @ref supervise latches
 *             @c supervision_lost and the watchdog is no longer refreshed, so
 *             the 4 s IWDG that BOOT started resets the board (`HLT-003`).
 *          -# **Trial health check.**  When the hand-over record marks the
 *             running slot as pending, the loop evaluates
 *             @ref health_conditions_met from 5 s after start-up and, once
 *             they pass, writes the CONFIRMED record with
 *             @ref app_flash_confirm_running_slot (`HLT-002`, `HLT-005`).  The
 *             window closes at 10 s, after which a failed check stops the
 *             watchdog feed so BOOT can roll the trial back (`HLT-004`).
 *
 *          **Timing.**  The loop blocks in @c xTaskNotifyWait for at most
 *          @ref HEALTH_LOOP_MS, so a check-in wakes it early and the 100 ms
 *          watchdog cadence is the only period that matters.  Both trial
 *          deadlines are measured against @ref start_tick, the tick captured
 *          when the task first ran, and not against the 5 s decision itself;
 *          that is why the conditions are re-evaluated on every iteration
 *          between 5 s and 10 s.
 *
 *          **Failure path.**  @ref health_check_fail latches
 *          @ref APP_ERROR_HEALTH and asks the LED task for the fast blink;
 *          the state becomes `FAILED` and feeding continues until 10 s so that
 *          Comms can still report the state.  After that deadline no kick is
 *          issued at all and the IWDG expires.
 *
 *          A @c DEMO_FAIL_HEALTH build makes @ref health_conditions_met return
 *          false unconditionally, which is how the rollback path is exercised
 *          (`HLT-004`).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DRT-012, DRT-013, HLT-001..HLT-005
 * @ingroup health_task
 * @see     health_task.h
 * @see     iwdg.c
 * @see     flash_confirm.c
 * @see     handover.h
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_error.h"
#include "app_fault.h"
#include "boot_contract.h"
#include "app_log.h"
#include "flash_confirm.h"
#include "handover.h"
#include "health_task.h"
#include "iwdg.h"
#include "led_task.h"
#include "uart3.h"

/**
 * @brief Stack size of the Health task in 32-bit words: 384 (1536 bytes).
 * @details Taken from the heap_4 arena by @ref health_task_create.  The task
 *          only calls short helpers plus @ref app_flash_confirm_running_slot,
 *          which builds an eight-word record image on the stack, but it also
 *          calls @ref app_log, whose formatter uses a 128-byte stack buffer,
 *          so the stack covers the deepest call chain with margin.
 * @note The watermark is not checked at run time; it can only be inspected
 *       with @c uxTaskGetStackHighWaterMark while debugging.
 */
#define HEALTH_TASK_STACK_WORDS  384U
/**
 * @brief Health task priority: @c tskIDLE_PRIORITY + 5, highest in the set.
 * @details Above Comms (+4), Button (+3) and LED (+2), so the supervision loop
 *          preempts ordinary application work and the check-in accounting
 *          cannot be starved (`DRT-010`, `DRT-012`).  Valid because
 *          @c configMAX_PRIORITIES is 8 and @c configUSE_PREEMPTION is 1.
 */
#define HEALTH_TASK_PRIORITY     (tskIDLE_PRIORITY + 5U)
/**
 * @brief Blocking period of one supervision loop iteration, in ms: 20.
 * @details Passed through @c pdMS_TO_TICKS to @c xTaskNotifyWait, so the task
 *          sleeps until a check-in bit arrives or 20 ms elapse, whichever is
 *          first.  The value bounds the latency of the 2 s supervision
 *          deadline and of the 5 s to 10 s trial window; it does not set the
 *          watchdog cadence, which is @ref HEALTH_REFRESH_MS.
 */
#define HEALTH_LOOP_MS           20U
/**
 * @brief Silence after which a supervised task is dead, in ms: 2000.
 * @details Compared against a tick difference of @ref last_seen, which is only
 *          correct because one tick is one millisecond at
 *          @c configTICK_RATE_HZ = 1000.  The value tolerates one missed
 *          one-second check-in of `DRT-012` and still stops the feed well
 *          inside the 4 s IWDG period (`HLT-003`).
 * @note The constant is used directly, without @c pdMS_TO_TICKS, so the unit
 *       in its name holds only while the tick rate stays 1 kHz.
 */
#define HEALTH_CHECKIN_TIMEOUT_MS 2000U
/**
 * @brief Delay from task start to the first health evaluation, in ms: 5000.
 * @details Gives every supervised task five one-second periods to check in and
 *          leaves the UART3 link, the heap and the fault record time to settle
 *          before they are tested (`HLT-001`).  The evaluation is repeated on
 *          every loop iteration from this point, so a condition that becomes
 *          true later still passes.
 */
#define HEALTH_CHECK_AT_MS       5000U
/**
 * @brief Deadline that closes the trial and stops the watchdog, in ms: 10000.
 * @details Measured from @ref start_tick and used twice: the health check fails
 *          when it is reached without a pass, and a `FAILED` state stops the
 *          watchdog refresh here.  Because feeding runs until this deadline, a
 *          failed trial resets the board through the 4 s IWDG timeout roughly
 *          14 s after start-up rather than by a direct reset request
 *          (`HLT-003`, `HLT-004`).
 */
#define HEALTH_STOP_KICK_MS      10000U
/**
 * @brief Watchdog refresh period while supervision holds, in ms: 100.
 * @details The loop reloads the IWDG when at least this long has passed since
 *          @ref last_kick, so a nominal 4 s IWDG period absorbs about 40
 *          missed refreshes.  Much shorter than
 *          @ref HEALTH_CHECKIN_TIMEOUT_MS on purpose: the feed must keep the
 *          board alive while a single check-in is merely late.
 * @note Like the other periods this is compared against raw ticks, so it is
 *       100 ms only at the configured 1 kHz tick rate.
 */
#define HEALTH_REFRESH_MS        100U
/**
 * @brief Minimum check-ins per accounting slot for a passing trial: 5.
 * @details One per second of the five-second window (`DRT-012`).  Notification
 *          bits coalesce, so the counters measure loop iterations that saw a
 *          check-in, not calls to @ref health_checkin a task reporting twice
 *          between two iterations scores once.  Health's own slot is stamped
 *          on every iteration and therefore exceeds the threshold long before
 *          5 s, as long as the task is running at all.
 */
#define HEALTH_MIN_CHECKINS      5U
/**
 * @brief Free heap required at the 5 s evaluation, in bytes: 4096.
 * @details Read with @c xPortGetFreeHeapSize, which heap_4 answers from its
 *          free-block list.  The application allocates nothing after start-up
 *          (`DRT-004`), so the figure is a fragmentation canary rather than a
 *          reservation: with the default 24 KiB arena the check fails once
 *          more than about 20 KiB is in use (`DRT-012`, `HLT-001`).
 */
#define HEALTH_MIN_FREE_HEAP     4096U

/**
 * @brief No pending slot asked for a check, so no check will run: 0.
 * @details The state after start-up when @ref g_handover does not report a
 *          pending trial, and the value reported before the task has run.
 *          The code matches the `HealthState` enumerator of
 *          `protocol/hwlink.fbs`, so Comms may cast it (`DRT-013`).
 */
#define HEALTH_STATE_NOT_REQUIRED 0U
/**
 * @brief The 5 s to 10 s trial window is open and the check may still pass: 1.
 * @details Set at task start when @ref g_handover reports `present` and
 *          @c trial == 2, that is a slot BOOT started on trial.  It stays until
 *          @ref health_check_pass or @ref health_check_fail reaches a verdict.
 */
#define HEALTH_STATE_RUNNING      1U
/**
 * @brief The trial check passed and the CONFIRMED record is in flash: 2.
 * @details Set by @ref health_check_pass after
 *          @ref app_flash_confirm_running_slot returned true.  The state is
 *          final for the boot; the watchdog feed continues normally.
 */
#define HEALTH_STATE_PASSED       2U
/**
 * @brief The trial check failed: 3.  LD3 blinks fast and the trial rolls back.
 * @details Set by @ref health_check_fail.  Feeding continues until
 *          @ref HEALTH_STOP_KICK_MS and then stops, so the IWDG resets the
 *          board and BOOT can reject the slot (`HLT-003`, `HLT-004`).
 */
#define HEALTH_STATE_FAILED       3U

/**
 * @brief Handle of the Health task and the check-in notification target.
 * @details Set once by @ref health_task_create and never cleared.  It stays
 *          `NULL` when creation fails, which makes every later
 *          @ref health_checkin a silent no-op.  The handle is the only link
 *          between the reporting tasks and this module; there is no shared
 *          flag to synchronise.
 */
static TaskHandle_t health_task_handle;
/**
 * @brief Check-ins observed per accounting slot since the task started.
 * @details Indexed by @ref health_task_id_t; the extra slot at index
 *          @ref HEALTH_TASK_COUNT belongs to the Health task itself.  Cleared
 *          once at start-up and then incremented by @ref checkin_account, at
 *          most once per slot per loop iteration because the notification bits
 *          coalesce.  The counters are cumulative, never reset again, and read
 *          only by @ref health_conditions_met.
 * @note Written only from the Health task, so no lock is needed.
 */
static uint32_t checkin_count[HEALTH_TASK_COUNT + 1U];
/**
 * @brief Tick at which each accounting slot was last seen, in milliseconds.
 * @details Same indexing as @ref checkin_count.  Initialised to the start tick
 *          for every slot, refreshed by @ref checkin_account and compared in
 *          @ref supervise.  Stored as @c uint32_t rather than @c TickType_t so
 *          that the unsigned subtraction wraps correctly across a tick-counter
 *          overflow.
 * @note Written only from the Health task.
 */
static uint32_t last_seen[HEALTH_TASK_COUNT + 1U];
/**
 * @brief Tick captured on the first iteration of the Health task.
 * @details Reference point for both trial deadlines, @ref HEALTH_CHECK_AT_MS
 *          and @ref HEALTH_STOP_KICK_MS.  Measuring from here rather than from
 *          @ref health_task_create keeps the window independent of how long
 *          start-up took before the scheduler first ran this task.
 */
static uint32_t start_tick;
/**
 * @brief Tick of the last @ref app_iwdg_kick, in milliseconds.
 * @details Initialised to the start tick so the first reload happens one
 *          @ref HEALTH_REFRESH_MS after the task starts rather than
 *          immediately; @ref app_iwdg_init has already refreshed the watchdog
 *          during start-up.
 */
static uint32_t last_kick;
/**
 * @brief Current health state, one of the `HEALTH_STATE_*` codes.
 * @details Holds @ref HEALTH_STATE_NOT_REQUIRED, @ref HEALTH_STATE_RUNNING,
 *          @ref HEALTH_STATE_PASSED or @ref HEALTH_STATE_FAILED.  It is the
 *          only variable of this module read from another task, through
 *          @ref health_state_get and @ref health_is_failed, hence @c volatile:
 *          the compiler must re-read it and must not drop the store.  An
 *          aligned single-byte access is atomic on Cortex-M7, which is what
 *          lets readers go without a lock.
 * @note The state never moves back to a lower value; once `PASSED` or
 *       `FAILED` it holds until the next reset.
 */
static volatile uint8_t health_state = HEALTH_STATE_NOT_REQUIRED;
/**
 * @brief Set once the trial check has reached a verdict.
 * @details Guards the check so that the CONFIRMED write is attempted at most
 *          once and a verdict is never re-evaluated.  It becomes true in both
 *          @ref health_check_pass and @ref health_check_fail, including when
 *          only the flash write failed.  It stays false when no slot is
 *          pending, in which case it is never read.
 */
static bool check_done;
/**
 * @brief Latched flag: at least one slot missed its 2 s check-in deadline.
 * @details Set by @ref supervise and never cleared, so the watchdog feed cannot
 *          resume even if the missing task checks in again later.  Together
 *          with the 10 s deadline for a `FAILED` state it is the only reason
 *          for @ref health_task to stop calling @ref app_iwdg_kick.
 */
static bool supervision_lost;

/**
 * @brief Account the check-ins drained from one loop iteration.
 * @details Walks the supervised bits 0 to @ref HEALTH_TASK_COUNT - 1 and, for
 *          every bit set in @p bits, increments that slot's @ref checkin_count
 *          and stores @p now in @ref last_seen.  Afterwards the Health slot at
 *          index @ref HEALTH_TASK_COUNT is stamped unconditionally, because
 *          reaching this code proves that the Health task itself is alive;
 *          that is what gives Health a check-in count that grows with every
 *          iteration.
 *
 *          Bits coalesce in the task notification value, so a task reporting
 *          twice between two iterations is counted once.  The counters are
 *          cumulative and are never reset after start-up.
 *
 * @param[in] bits Bit mask of check-ins read from the notification value; bit
 *                 @c i corresponds to task identifier @c i.
 * @param[in] now  Current tick in milliseconds, stored as the last-seen
 *                 timestamp of every task that checked in.
 *
 * @pre  Called from the Health task only, after @ref checkin_count and
 *       @ref last_seen have been initialised at task start.
 * @post For each set bit the matching slot has a higher counter and a newer
 *       timestamp; the Health slot always has both.
 *
 * @note No locking: the counters and timestamps are private to the Health
 *       task, and a stale entry is exactly what @ref supervise looks for.
 * @see health_conditions_met
 * @see supervise
 */
static void checkin_account(uint32_t bits, uint32_t now)
{
    for (uint32_t i = 0U; i < HEALTH_TASK_COUNT; i++) {
        if ((bits & (1UL << i)) != 0U) {
            checkin_count[i]++;
            last_seen[i] = now;
        }
    }

    /* Health itself is alive whenever this function runs. */
    checkin_count[HEALTH_TASK_COUNT]++;
    last_seen[HEALTH_TASK_COUNT] = now;
}

/**
 * @brief Compare every slot with the 2 s deadline and latch a supervision loss.
 * @details Returns immediately when @ref supervision_lost is already set.
 *          Otherwise it walks all @ref HEALTH_TASK_COUNT + 1 slots, the Health
 *          slot included, and compares the unsigned tick difference
 *          @c now - last_seen[i] with @ref HEALTH_CHECKIN_TIMEOUT_MS.  The
 *          first overdue slot sets @ref supervision_lost and ends the walk; the
 *          caller then stops refreshing the watchdog.
 *
 * @param[in] now Current tick in milliseconds.
 *
 * @pre  @ref last_seen holds a start value for every slot, written before the
 *       first call.
 * @post Either no slot is overdue, or @ref supervision_lost is true.  The flag
 *       is never cleared here.
 *
 * @note The unsigned subtraction wraps correctly across a 32-bit tick
 *       overflow, so a tick wrap is not mistaken for a long silence.
 * @note One late check-in is tolerated: the test is strictly greater than the
 *       deadline, so a task seen exactly 2000 ms ago is still considered alive.
 * @warning The decision is one-directional.  Once supervision is declared lost
 *          only a reset restores the feed (`HLT-003`).
 * @see checkin_account
 * @see health_task
 */
static void supervise(uint32_t now)
{
    if (supervision_lost) {
        return;
    }
    for (uint32_t i = 0U; i < (HEALTH_TASK_COUNT + 1U); i++) {
        if ((now - last_seen[i]) > HEALTH_CHECKIN_TIMEOUT_MS) {
            supervision_lost = true;
            return;
        }
    }
}

/**
 * @brief Evaluate every condition a pending slot must satisfy.
 * @details The tests run in this order and stop at the first failure:
 *          -# In a @c DEMO_FAIL_HEALTH build the function returns false before
 *             looking at anything, so the trial always fails (`HLT-004`).
 *          -# Every accounting slot, Health included, must show at least
 *             @ref HEALTH_MIN_CHECKINS check-ins (`HLT-001`, `DRT-012`).
 *          -# No fault record may belong to the current boot, which catches a
 *             fault already latched during this trial run.
 *          -# A fault from the previous boot only fails the check when
 *             @ref g_handover reports more than one trial, that is when the
 *             faulted boot was itself a trial.  An older fault from before the
 *             update is reported in `Status` only.
 *          -# The free heap must still be at least @ref HEALTH_MIN_FREE_HEAP
 *             bytes.
 *          -# The UART3 receiver must be running, so a confirmed slot is one
 *             whose HWLINK link came up.
 *
 * @return true when the pending slot may be confirmed.
 * @retval false At least one condition failed, or this is a
 *               @c DEMO_FAIL_HEALTH build.
 *
 * @pre  Called from the Health task at or after @ref HEALTH_CHECK_AT_MS.
 * @post No state is changed; every input is a read-only query.
 *
 * @note The decision is not final by itself: the caller re-evaluates it on
 *       every loop iteration until @ref HEALTH_STOP_KICK_MS, because the fault,
 *       hand-over and UART state belong to other modules and may change.
 * @note @ref g_handover is a start-up snapshot; only its @c trial field is
 *       patched by this module, and only after a successful CONFIRMED write.
 * @see app_fault_is_current_boot
 * @see app_fault_is_previous_boot
 * @see app_uart3_rx_running
 * @see app_flash_confirm_running_slot
 */
static bool health_conditions_met(void)
{
#ifdef DEMO_FAIL_HEALTH
    return false;
#else
    for (uint32_t i = 0U; i < (HEALTH_TASK_COUNT + 1U); i++) {
        if (checkin_count[i] < HEALTH_MIN_CHECKINS) {
            return false;
        }
    }
    if (app_fault_is_current_boot()) {
        return false;
    }
    /* A fault in the previous trial boot is a failed trial; an older fault
     * from before the update is only reported in Status.  BOOT increments
     * the trial counter each time it starts a pending slot. */
    if (g_handover.trial_count > 1U && app_fault_is_previous_boot()) {
        return false;
    }
    if (xPortGetFreeHeapSize() < HEALTH_MIN_FREE_HEAP) {
        return false;
    }
    return app_uart3_rx_running();
#endif
}

/**
 * @brief Latch the failed verdict of the trial health check.
 * @details Sets @ref check_done and moves @ref health_state to
 *          @ref HEALTH_STATE_FAILED, then latches @ref APP_ERROR_HEALTH in the
 *          global error mask and asks the LED task for the fast LD3 blink with
 *          @ref led_post_health_failed (`LED-005`, `HLT-004`).  The return
 *          value of the LED request is ignored: when the queue is full the
 *          global latch still drives the blink on the task's next iteration.
 *
 * @pre  Called from the Health task only, and at most once per boot because
 *       @ref check_done is set here.
 * @post The verdict is final: the check is not evaluated again and the
 *       watchdog feed stops at @ref HEALTH_STOP_KICK_MS.
 *
 * @note Task context.  @ref app_error_set masks interrupts for a few
 *       instructions; @ref led_post_health_failed may block briefly on the LED
 *       queue but allocates nothing.
 * @see health_check_pass
 * @see health_state_get
 */
static void health_check_fail(void)
{
    check_done = true;
    health_state = HEALTH_STATE_FAILED;
    app_error_set(APP_ERROR_HEALTH);
    (void)led_post_health_failed();
}

/**
 * @brief Latch the passing verdict of the trial health check.
 * @details Sets @ref check_done and moves @ref health_state to
 *          @ref HEALTH_STATE_PASSED.  The CONFIRMED record has already been
 *          written by the caller and @c g_handover.trial has already been
 *          lowered to the confirmed value, so nothing else is recorded here.
 *
 * @pre  Called from the Health task, only from the success path of
 *       @ref app_flash_confirm_running_slot.
 * @post The state stays `PASSED` until the next reset and the watchdog feed
 *       continues normally.
 *
 * @see health_check_fail
 * @see app_flash_confirm_running_slot
 */
static void health_check_pass(void)
{
    check_done = true;
    health_state = HEALTH_STATE_PASSED;
}

/**
 * @brief Body of the Health task: initialise the accounting, then loop.
 * @details **Start-up.**  Captures the tick once and uses it for
 *          @ref start_tick, @ref last_kick and every @ref last_seen entry, so
 *          no slot is overdue before the first iteration has been accounted.
 *          Clears @ref checkin_count and then selects the initial
 *          @ref health_state from the hand-over record:
 *          @ref HEALTH_STATE_RUNNING when @ref g_handover is present with
 *          @c trial == 2 (pending), @ref HEALTH_STATE_NOT_REQUIRED otherwise.
 *          Only a pending slot logs the window start, because only a pending
 *          slot is ever checked.
 *
 *          **Loop.**  Every iteration performs these steps:
 *          -# Blocks in @c xTaskNotifyWait for at most @ref HEALTH_LOOP_MS with
 *             all bits cleared on exit, so @p bits holds exactly the check-ins
 *             that arrived since the previous iteration.  A timeout leaves
 *             @p bits at the zero it was initialised with.
 *          -# Re-reads the tick count, accounts the check-ins with
 *             @ref checkin_account and runs @ref supervise.
 *          -# While the slot is pending, no verdict exists and
 *             @ref HEALTH_CHECK_AT_MS has elapsed, evaluates
 *             @ref health_conditions_met.  On success it calls
 *             @ref app_flash_confirm_running_slot and, when that returns true,
 *             lowers @c g_handover.trial to 1 so Comms and `Status` report
 *             the slot as confirmed, then records the pass.  A rejected write
 *             fails the check immediately instead of being retried.  While the
 *             conditions do not hold, the evaluation is repeated every
 *             iteration until @ref HEALTH_STOP_KICK_MS, where it fails.
 *          -# Decides whether to feed the watchdog: never while
 *             @ref supervision_lost is set, and not once the state is `FAILED`
 *             and the 10 s deadline has passed.  Otherwise it calls
 *             @ref app_iwdg_kick whenever @ref HEALTH_REFRESH_MS has elapsed
 *             since @ref last_kick.
 *
 * @param[in] arg Unused.  The task needs no parameter because every input is
 *                reachable through the module's static state.
 *
 * @pre  Created by @ref health_task_create before the scheduler starts, with
 *       @ref g_handover already decoded by @c app_handover_read.
 * @post Never returns.  The task runs until reset, so there is no cleanup and
 *       no termination path.
 *
 * @note Task context at the highest application priority, and the only writer
 *       of @ref health_state, @ref check_done and @ref supervision_lost.
 * @note Blocking calls in the loop: @c xTaskNotifyWait for at most 20 ms,
 *       @ref app_flash_confirm_running_slot once for the duration of one flash
 *       word-programming sequence, and @ref app_log, which never blocks.
 * @note A failed trial ends by starving the IWDG rather than by requesting a
 *       reset, so the mechanism that proves supervision works also guarantees
 *       the reset (`HLT-003`).
 * @warning The feed decision must stay in this task.  Any other caller of
 *          @ref app_iwdg_kick would keep the board alive through a failed check
 *          and defeat the rollback path (`DRT-012`, `HLT-003`).
 * @see health_conditions_met
 * @see app_iwdg_kick
 * @see app_flash_confirm_running_slot
 */
static void health_task(void *arg)
{
    (void)arg;
    uint32_t now = xTaskGetTickCount();
    start_tick = now;
    last_kick = now;
    for (uint32_t i = 0U; i < (HEALTH_TASK_COUNT + 1U); i++) {
        last_seen[i] = now;
        checkin_count[i] = 0U;
    }

    bool pending = g_handover.present && g_handover.trial == 2U;
    health_state = pending ? HEALTH_STATE_RUNNING : HEALTH_STATE_NOT_REQUIRED;
    if (pending) {
        LOG_I("health: on trial, check in %u ms", (unsigned)HEALTH_CHECK_AT_MS);
    }

    for (;;) {
        uint32_t bits = 0U;
        (void)xTaskNotifyWait(0U, 0xFFFFFFFFUL, &bits,
                              pdMS_TO_TICKS(HEALTH_LOOP_MS));
        now = xTaskGetTickCount();
        checkin_account(bits, now);
        supervise(now);

        if (pending && !check_done &&
            (now - start_tick) >= HEALTH_CHECK_AT_MS) {
            /* The trial window closes at HEALTH_STOP_KICK_MS.  Check the
             * deadline before the conditions, so a check that only becomes
             * true after the window cannot still confirm the slot. */
            if ((now - start_tick) >= HEALTH_STOP_KICK_MS) {
                health_check_fail();
                LOG_E("health: conditions not met, stopping the watchdog "
                      "kick (trial fails)");
            } else if (health_conditions_met()) {
                if (app_flash_confirm_running_slot()) {
                    /* The CONFIRMED record is in flash, so the slot is no
                     * longer on trial.  g_handover was decoded once at
                     * start-up; update it so Status reports CONFIRMED.  A
                     * single byte store, so Comms never sees a torn value. */
                    g_handover.trial = 1U;
                    health_check_pass();
                    LOG_I("health: passed, CONFIRMED written");
                } else {
                    health_check_fail();
                    LOG_E("health: CONFIRMED record write failed");
                }
            }
        }

        bool kick = !supervision_lost;
        if (health_state == HEALTH_STATE_FAILED &&
            (now - start_tick) >= HEALTH_STOP_KICK_MS) {
            kick = false;
        }

        if (kick && (now - last_kick) >= HEALTH_REFRESH_MS) {
            app_iwdg_kick();
            last_kick = now;
        }
    }
}

/**
 * @brief Create the Health task and store its handle.
 * @details Calls @c xTaskCreate with @ref HEALTH_TASK_STACK_WORDS,
 *          @ref HEALTH_TASK_PRIORITY, the name `"Health"`, a `NULL` argument
 *          and @ref health_task_handle as the output.  The task is created
 *          before the scheduler runs, so it becomes ready but does not execute
 *          until @c vTaskStartScheduler.
 *
 * @return true when FreeRTOS created the task.
 * @retval false The task control block or the 1536-byte stack could not be
 *               allocated from the heap_4 arena; @ref health_task_handle stays
 *               `NULL` and @ref health_checkin becomes a no-op.
 *
 * @pre  Called once from @c main after the heap, the clocks and the hand-over
 *       read are done and before @c vTaskStartScheduler (`DRT-004`).
 * @post On success the task is ready and the check-in target is valid.
 *
 * @note Not idempotent: a second successful call creates a second task and
 *       overwrites the handle, leaving the first instance running with no
 *       check-in target.  Call it once, as @c main does.
 * @note The caller must treat a false return as fatal: without this task
 *       nothing refreshes the watchdog.
 * @see health_checkin
 */
bool health_task_create(void)
{
    return xTaskCreate(health_task, "Health", HEALTH_TASK_STACK_WORDS, NULL,
                       HEALTH_TASK_PRIORITY, &health_task_handle) == pdPASS;
}

/**
 * @brief Report that a supervised task is alive.
 * @details Validates the arguments and then sets the caller's bit with
 *          @c xTaskNotify and @c eSetBits.  A `NULL` handle, meaning
 *          @ref health_task_create failed or has not run yet, makes the call a
 *          no-op, as does an identifier at or above @ref HEALTH_TASK_COUNT.
 *          The call is non-blocking, allocates nothing and reports no error.
 *          The notification value is a set of bits, so several check-ins
 *          before the next loop iteration are merged into one.
 *
 * @param[in] task Identifier of the calling task: @ref HEALTH_TASK_LED,
 *                 @ref HEALTH_TASK_BUTTON or @ref HEALTH_TASK_COMMS.
 *
 * @pre  The Health task has been created and @p task is a valid enumerator.
 * @post On a valid call bit @c 1 << task is pending, so the next Health loop
 *       iteration wakes early and refreshes that task's last-seen timestamp.
 *
 * @note Task context only: @c xTaskNotify is the task-level API and must not
 *       be called from an ISR.  An ISR would need @c xTaskNotifyFromISR, which
 *       this module does not provide.
 * @note The range test casts @p task to @c uint32_t first, so a cast negative
 *       value is rejected instead of shifting the notification mask by an
 *       out-of-range amount.
 * @see health_task_create
 * @see health_task_id_t
 */
void health_checkin(health_task_id_t task)
{
    if (health_task_handle == NULL ||
        (uint32_t)task >= (uint32_t)HEALTH_TASK_COUNT) {
        return;
    }
    (void)xTaskNotify(health_task_handle, 1UL << (uint32_t)task, eSetBits);
}

/**
 * @brief Read the current health-state code for the `Status` reply.
 * @details Returns the single-byte @ref health_state.  The code matches the
 *          `HealthState` enumeration in `protocol/hwlink.fbs`, so Comms casts
 *          instead of mapping, and it is @ref HEALTH_STATE_NOT_REQUIRED before
 *          the task first runs.
 *
 * @return The current state code, 0 to 3.
 *
 * @pre  None; safe before the scheduler starts.
 * @post @ref health_state is unchanged.
 *
 * @note The aligned byte load is atomic on Cortex-M7, so no lock is needed;
 *       the value may of course change immediately after the call returns.
 * @see health_is_failed
 * @see health_state
 */
uint8_t health_state_get(void)
{
    return health_state;
}

/**
 * @brief Test whether the trial health check has failed.
 * @details Compares @ref health_state with @ref HEALTH_STATE_FAILED.  The
 *          result is true from the moment @ref health_check_fail latches the
 *          verdict and for the rest of the boot.
 *
 * @return true when the state is @ref HEALTH_STATE_FAILED.
 * @retval false The check is not required, is still running, or passed.
 *
 * @pre  None; safe from any task.
 * @post No state is changed.
 *
 * @note No caller in the current application; `Status` uses
 *       @ref health_state_get.  The predicate is the readable form of the
 *       comparison and is kept for future consumers.
 * @see health_state_get
 */
bool health_is_failed(void)
{
    return health_state == HEALTH_STATE_FAILED;
}
