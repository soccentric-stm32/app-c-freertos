/**
 * @file    health_task.h
 * @brief   Task supervision, trial health check and CONFIRMED writer.
 * @details The Health task is the only task that refreshes the independent
 *          watchdog in steady state, so the IWDG timeout stays a proof that
 *          supervision is alive.  Every supervised task calls
 *          @ref health_checkin, which sets one bit in the Health task
 *          notification value.  Health drains those bits on each 20 ms loop
 *          iteration and timestamps the tasks that raised them; it also
 *          stamps itself, because that accounting code only runs while the
 *          task runs.  If any supervised task has not been seen for more
 *          than 2 s, supervision is declared lost and Health stops
 *          refreshing the watchdog.  A stalled Health task needs no such
 *          check, because no kick is issued at all in that case.  Either way
 *          the 4 s IWDG that BOOT started resets the board (`DRT-012`,
 *          `HLT-003`).
 *
 *          **Trial health window.**  When the hand-over record marks the
 *          running slot as pending, Health starts in `HEALTH_STATE_RUNNING`
 *          and evaluates the health check 5 s after start-up: all four
 *          accounting slots, that is the three supervised tasks plus Health
 *          itself, must show at least five check-ins, no fault may be
 *          recorded for the current boot, a fault from the previous boot
 *          only invalidates the check once the trial counter shows more than
 *          one trial, the free heap must be at least 4 KiB and the HWLINK
 *          receiver must be running (`HLT-001`).  The check is re-evaluated
 *          on every loop iteration, so it may still pass between 5 s and
 *          10 s; at 10 s the window closes and the check fails (`HLT-002`).
 *          A `DEMO_FAIL_HEALTH` build skips the conditions and always fails,
 *          which is how rollback is tested (`HLT-004`).  When no slot is
 *          pending the state stays `HEALTH_STATE_NOT_REQUIRED` and the check
 *          never runs.
 *
 *          **CONFIRMED write.**  On success Health programs the single
 *          CONFIRMED metadata record of the running slot with
 *          @ref app_flash_confirm_running_slot, which is a no-op when the
 *          record is already present; that is the only flash write DEMO
 *          performs (`HLT-002`, `HLT-005`).  The cached hand-over trial state
 *          is then updated to confirmed so `Status` reports the new state,
 *          and the health state becomes `HEALTH_STATE_PASSED`.
 *
 *          **Failure and reset path.**  On failure Health latches
 *          @ref APP_ERROR_HEALTH, asks the LED task for the fast LD3 blink
 *          with @ref led_post_health_failed, reports `FAILED` in `Status`
 *          and keeps refreshing the watchdog until 10 s after start-up.
 *          From then on no kick is issued, so the 4 s IWDG timeout resets the
 *          board and BOOT can count a failed trial and roll back
 *          (`HLT-003`).  Only the Health task owns that decision; feeding the
 *          watchdog from anywhere else would hide the stall this module
 *          exists to detect.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     DRT-012, DRT-013, HLT-001..HLT-005
 * @see     health_task.c
 * @see     iwdg.h
 * @ingroup health_task
 */

#ifndef HEALTH_TASK_H
#define HEALTH_TASK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup health_task Task supervision and trial health check
 * @brief Check-in supervision, watchdog feeding policy and CONFIRMED record.
 * @details The group exposes the supervised task identifiers, the check-in
 *          report used by every other task and the two read-only accessors
 *          used to fill the `Status` reply.  All supervision state is private
 *          to @ref health_task.c; callers never see the check-in counters,
 *          the last-seen timestamps or the watchdog decision.
 *
 *          The module is event driven by task notifications rather than by
 *          shared flags: @ref health_checkin only raises a bit, so it is
 *          non-blocking, needs no lock and cannot fail.  Timing is measured
 *          in FreeRTOS ticks taken from `xTaskGetTickCount`, and the
 *          thresholds are compile-time constants in @ref health_task.c, not
 *          configuration knobs.
 * @note  The health state codes used by `Status` match the `HealthState`
 *        enumeration of `protocol/hwlink.fbs` by value, so the Comms task can
 *        cast the result of @ref health_state_get directly.  The codes
 *        themselves are private to @ref health_task.c and are not part of
 *        this interface.
 * @{
 */

/**
 * @brief Identifier of a supervised task, used for check-in accounting.
 * @details The values are dense and start at zero on purpose: a value is used
 *          as a bit index into the Health task notification value and as an
 *          index into the per-task check-in counter and last-seen table in
 *          @ref health_task.c.  The order therefore encodes the bit layout,
 *          and the enumerators must not be renumbered or reordered.  The
 *          enumeration is not a bit mask; callers pass one identifier per
 *          call.
 * @note  The Health task itself is not an enumerator.  Its liveness is
 *        tracked in the extra table slot indexed by @ref HEALTH_TASK_COUNT,
 *        which is why that value doubles as the table length.
 */
typedef enum {
    HEALTH_TASK_LED = 0,   /**< LED task; checks in on its LD3 refresh cycle. */
    HEALTH_TASK_BUTTON,    /**< Button task; checks in from its polling loop. */
    HEALTH_TASK_COMMS,     /**< Comms task; checks in from its UART3 loop. */
    HEALTH_TASK_COUNT      /**< Number of supervised tasks; not a valid id. */
} health_task_id_t;

/**
 * @brief   Create the Health task before the scheduler starts.
 * @details Creates the highest-priority application task: priority
 *          `tskIDLE_PRIORITY + 5`, 384 stack words and a 20 ms loop period.
 *          The task control block handle produced here is also the
 *          notification target of @ref health_checkin, so check-ins that
 *          arrive before this call succeeds are dropped rather than queued.
 *          The task does not start until `vTaskStartScheduler()` is called.
 *
 * @return  true when the task was created, false when it was not.
 * @retval  false FreeRTOS could not allocate the task control block or its
 *          stack; the handle stays `NULL` and every later check-in is a
 *          no-op.
 *
 * @pre     Called from `main()` after the other tasks and queues exist and
 *          before `vTaskStartScheduler()` (`DRT-004`).
 * @post    On success the Health task is ready to run and the check-in
 *          target is valid.
 * @note    A failure is fatal for the caller: `main()` reports it through
 *          `startup_fail()` and never reaches the scheduler, because without
 *          this task nothing would feed the watchdog.
 * @see     health_checkin
 */
bool health_task_create(void);

/**
 * @brief   Report that a supervised task is alive.
 * @details Sets the caller's bit in the Health task notification value with
 *          `xTaskNotify` and `eSetBits`.  Bits coalesce, so Health learns
 *          that a task checked in, not how often; it clears the pending bits
 *          when it reads them, then stamps the check-in time and increments
 *          the per-task counter used by the trial health check.  The call
 *          never blocks, performs no allocation and reports no error.
 *
 * @param[in] task Identifier of the calling task.  One of
 *                 @ref HEALTH_TASK_LED, @ref HEALTH_TASK_BUTTON or
 *                 @ref HEALTH_TASK_COMMS; values greater than or equal to
 *                 @ref HEALTH_TASK_COUNT are ignored, as are all calls made
 *                 before @ref health_task_create succeeded.
 *
 * @pre     The Health task exists, otherwise the call is a silent no-op.
 * @post    The caller's bit is pending for the next Health loop iteration and
 *          its last-seen tick is refreshed when Health consumes the bit.
 * @warning Task context only.  `xTaskNotify` is used rather than
 *          `xTaskNotifyFromISR`, so this must not be called from an interrupt
 *          handler.
 * @note    The range check casts @p task to `uint32_t` before comparing with
 *          @ref HEALTH_TASK_COUNT, so a cast negative value is rejected
 *          instead of shifting the notification mask by an out-of-range
 *          amount.  Pass one of the enumerators only.
 * @note    The nominal cadence is once per second per task (`DRT-012`).  One
 *          missed period is tolerated because supervision only gives up after
 *          2 s, but a task that stops checking in for longer stops the
 *          watchdog feed and eventually resets the board (`HLT-003`).
 * @see     health_task_create
 * @see     iwdg.h
 */
void health_checkin(health_task_id_t task);

/**
 * @brief   Read the current health-state code for the `Status` reply.
 * @details Returns the Health task's `volatile uint8_t` state, which is one of
 *          the four codes defined privately in @ref health_task.c, namely
 *          `NOT_REQUIRED` (0) when no pending slot asked for a check,
 *          `RUNNING` (1) while the 5 s to 10 s trial window is open,
 *          `PASSED` (2) after the CONFIRMED record was written, and
 *          `FAILED` (3) after a failed check.  The values equal the
 *          `HealthState` enumeration of `protocol/hwlink.fbs`, so the caller
 *          casts instead of mapping.
 *
 * @return  The current health-state code, 0 to 3.
 *
 * @note    A single-byte read is atomic on Cortex-M7, so no lock is required;
 *          the state can still change immediately after the call returns.
 *          The function may be called before the Health task has run, in
 *          which case it reports `NOT_REQUIRED`.
 * @see     health_is_failed
 * @see     comms_task.c
 */
uint8_t health_state_get(void);

/**
 * @brief   Test whether the trial health check has failed.
 * @details Predicate equivalent to comparing @ref health_state_get with the
 *          private `FAILED` code (3).  It becomes true the moment a check
 *          fails, for example when the CONFIRMED write is rejected or the
 *          10 s window closes, and stays true until the next reset.
 *
 * @return  true when the health check failed, false otherwise.
 * @retval  false No pending slot required a check, the check is still
 *          running, or it passed.
 *
 * @note    No caller in the current application; `Status` reports the numeric
 *          state through @ref health_state_get instead.  The predicate is kept
 *          as the readable form of the comparison for any future consumer.
 * @see     health_state_get
 */
bool health_is_failed(void);

/** @} */

#endif /* HEALTH_TASK_H */
