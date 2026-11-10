/**
 * @file    led_task.h
 * @brief   LED pattern engine and LD3 error/fault/health indication.
 * @details The LED task owns the pattern state of LD1 (green), LD2 (blue)
 *          and LD3 (red) and is the only writer of the three LED pins.
 *          Other tasks change patterns by posting messages to a dedicated
 *          queue (DRT-011: no direct state writes); @ref led_task_create
 *          creates that queue and the task before the scheduler starts.
 *          Pattern transitions are scheduled to the exact tick boundary of
 *          the next segment instead of a fixed polling rate (LED-006), so a
 *          segment length in milliseconds becomes a delay in ticks (the tick
 *          rate is 1000 Hz, DRT-001).
 * @details LD3 is reserved for status indication and takes priority over its
 *          configured pattern: a 100 ms fast blink while a pending-slot
 *          health check has failed (HLT-003), a solid light while a saved
 *          fault or watchdog reset is latched (LED-005), and otherwise the
 *          pattern selected by @ref led_post_set with @p force set.
 *          @ref led_post_clear_error plus app_error_clear_fault() release
 *          the latched state; the HEALTH indication survives that clear.
 * @note    The task runs forever, never blocks on anything but its queue and
 *          the tick delay, and checks in with the health task every 500 ms.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI
 * @srs     LED-001..LED-007, BTN-002, BTN-003
 * @ingroup led_task
 */

#ifndef LED_TASK_H
#define LED_TASK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup led_task LED patterns and LD3 status indication
 * @brief Pattern engine, LD3 error display and the queue-based control API.
 * @details The group covers the pattern identifiers, the period limits and
 *          the message-posting API.  Every posting function is non-blocking
 *          and returns @c false when the message could not be queued, so a
 *          caller can report BUSY without stalling its own task.
 * @note The queue is created by @ref led_task_create; before that call all
 *       posting functions fail because the queue handle is @c NULL.
 * @{
 */

/**
 * @brief LED identifier for LD1, the green user LED (PB0).
 * @details Valid for @ref led_post_set.  LD1 and LD2 form a pair: setting
 *          @ref APP_PATTERN_CHASE on either one applies it to both.
 */
#define APP_LED_ID_1 1U

/**
 * @brief LED identifier for LD2, the blue user LED (PB7).
 * @details Valid for @ref led_post_set and behaves exactly like
 *          @ref APP_LED_ID_1 with respect to the CHASE pairing.
 */
#define APP_LED_ID_2 2U

/**
 * @brief LED identifier for LD3, the red error LED (PB14).
 * @details Reserved by LED-005 for fault, watchdog and health reporting.
 *          @ref led_post_set accepts this identifier only when the @p force
 *          argument is @c true; without it the message is silently ignored
 *          by the task, and the HWLINK SetLed handler rejects the request
 *          with a BAD_ARGUMENT NAK before it ever reaches the queue.
 * @warning Do not use LD3 as a generic indicator.  Health and latched error
 *          bits override the configured pattern and hide it completely.
 */
#define APP_LED_ID_3 3U

/**
 * @brief Pattern that leaves the LED dark.
 * @details The LED is driven low and the task does not schedule a transition
 *          for it, so it only wakes for the health check-in or a new message.
 */
#define APP_PATTERN_OFF 0U

/**
 * @brief Pattern that holds the LED continuously lit.
 * @details Drive only: like OFF, no periodic transition is scheduled.
 */
#define APP_PATTERN_ON 1U

/**
 * @brief Symmetric blink, one on and one off segment per period.
 * @details On time is @c period/2 and the off segment covers the remainder,
 *          so a 1000 ms period yields 500 ms on and 500 ms off.  The default
 *          period of @ref APP_LED_DEFAULT_PERIOD gives a 1 Hz blink.
 */
#define APP_PATTERN_BLINK 2U

/**
 * @brief Two short flashes per cycle, as used for the default LD1 pattern.
 * @details The visible sequence is 50 ms on, 150 ms off, 50 ms on, then off
 *          for the remainder of the cycle.  The cycle is at least 250 ms, so
 *          a shorter configured period is clamped to 250 ms; with the 1000 ms
 *          default the final gap is 750 ms.
 */
#define APP_PATTERN_HEARTBEAT 3U

/**
 * @brief Blink at five times the rate of @ref APP_PATTERN_BLINK.
 * @details The internal cycle is @c period/5 (minimum 2 ms) split into an on
 *          segment of @c cycle/2 and the remainder off.  A 1000 ms period
 *          therefore produces a 200 ms cycle, that is, a 5 Hz blink.
 */
#define APP_PATTERN_FAST 4U

/**
 * @brief Alternating pattern for the LD1/LD2 pair.
 * @details Each half period exactly one LED of the pair is lit: LD1 is on for
 *          the first half and LD2 for the second half.  Setting CHASE on
 *          either LED configures both, and setting any other pattern on one
 *          of them switches the partner off so the pair cannot stay split.
 * @note The effect is only visible when both LEDs are on the same board;
 *       LD3 is not part of the pair.
 */
#define APP_PATTERN_CHASE 5U

/**
 * @brief Morse "SOS" (... --- ...) on a fixed 100 ms time unit.
 * @details The 18 segments run 100 ms (dot or intra-character gap), 300 ms
 *          (dash or letter gap) and a final 700 ms word gap, giving a 3.4 s
 *          cycle that ends dark.  The configured period is ignored for this
 *          pattern.
 * @note Because the period is ignored, a period change alone cannot alter the
 *       rhythm of this pattern; only switching away and back restarts it.
 */
#define APP_PATTERN_MORSE_SOS 6U

/**
 * @brief Number of defined patterns, also used as the wrap limit.
 * @details @ref led_task uses @c (pattern + 1) % APP_PATTERN_COUNT for the
 *          short-press cycle and Comms rejects any pattern of this value or
 *          above, so the enumerator count must match the pattern list.
 */
#define APP_PATTERN_COUNT 7U

/**
 * @brief Smallest accepted pattern period in milliseconds (50 ms).
 * @details Below this value the blink, heartbeat and fast segments would
 *          collapse to a few ticks.  Comms rejects SetLed and SetLedAll
 *          requests outside the range and applies no clamping of its own.
 */
#define APP_LED_MIN_PERIOD 50U

/**
 * @brief Largest accepted pattern period in milliseconds (5000 ms).
 * @details Together with @ref APP_LED_MIN_PERIOD this brackets the period
 *          that Comms accepts; the LED task itself trusts the value it is
 *          given through the queue.
 * @note Comms additionally requires the period to be a multiple of 10 ms for
 *       HWLINK requests.  The LED task does not check that.
 */
#define APP_LED_MAX_PERIOD 5000U

/**
 * @brief Period applied to LD1 at start-up and by a long button press.
 * @details Also the fallback period used when a short press finds the stored
 *          LD1 period outside the @ref APP_LED_MIN_PERIOD ..
 *          @ref APP_LED_MAX_PERIOD range.  LD2 starts with the same period
 *          but with @ref APP_PATTERN_OFF.
 */
#define APP_LED_DEFAULT_PERIOD 1000U

/**
 * @brief Configure the three LED GPIOs and switch them off.
 * @details Enables the GPIOB clock, drives LD1, LD2 and LD3 low, and
 *          configures the three pins as push-pull outputs without pull
 *          resistors at low slew rate.  The function is not reentrant and
 *          must run after the HAL and clock initialisation so that the GPIOB
 *          clock tree is already set up.
 *
 * @pre   Called once from main() before vTaskStartScheduler().
 * @post  All three LEDs are dark and ready for the LED task to drive them.
 * @note  The pins and ports come from @ref app_board.h; the function does not
 *        touch the button or UART pins.
 */
void app_leds_init(void);

/**
 * @brief Force LD3 on or off, bypassing the pattern engine.
 * @details Writes LD3 through the port BSRR register, exactly as the task
 *          does, but without queueing a message.  Intended for start-up or
 *          fatal paths where the scheduler is not running yet.
 *
 * @param[in] on @c true lights LD3, @c false turns it off.
 *
 * @warning This bypasses the single-writer rule of DRT-011.  When the LED
 *          task is already running it will overwrite the pin at its next
 *          transition, which can be tens of milliseconds later, so the call
 *          is not a reliable override.  The current firmware does not call
 *          it; it exists as an early-boot escape hatch.
 * @see app_leds_init
 */
void app_led3_set(bool on);

/**
 * @brief Create the LED queue and the LED task.
 * @details Creates a queue of eight @c led_msg_t entries and then the task
 *          named "LED" with a 384 word stack at priority
 *          @c tskIDLE_PRIORITY + 2.  Must be called once from main() before
 *          the scheduler starts (DRT-004), after @ref app_leds_init.
 *          The task starts with LD1 in @ref APP_PATTERN_HEARTBEAT, LD2 and
 *          LD3 in @ref APP_PATTERN_OFF, all at @ref APP_LED_DEFAULT_PERIOD,
 *          with their phase anchored to the tick at which the task begins.
 *          From then on it loops forever: it evaluates the three patterns,
 *          writes the pins, checks in with the health task every 500 ms, and
 *          sleeps until the earliest of the next segment boundary and the
 *          next check-in; a received message wakes it immediately and any
 *          further queued messages are drained in the same iteration.
 *
 * @return @c true when both the queue and the task were created.
 * @retval false The queue allocation failed or @c xTaskCreate did not return
 *               @c pdPASS; main() treats this as a fatal start-up error.
 *
 * @post  On success @ref led_post_set and the other posting functions work
 *        and the task appears in the health supervision set.
 * @note  A failed @c xTaskCreate leaves the queue allocated; the caller is
 *        expected to reset the board rather than retry.
 * @see led_post_set
 */
bool led_task_create(void);

/**
 * @brief Queue a pattern change for one LED (DRT-011).
 * @details Copies the request into the LED queue.  The task applies it at the
 *          start of its next iteration, restarting the pattern phase at that
 *          moment.  A request for @ref APP_LED_ID_3 is ignored unless
 *          @p force is @c true, so the error display cannot be repurposed by
 *          accident.  Pairing rules for @ref APP_PATTERN_CHASE and for
 *          leaving CHASE match the behaviour described for that pattern.
 *
 * @param[in] led     LED identifier, one of @ref APP_LED_ID_1,
 *                    @ref APP_LED_ID_2 or @ref APP_LED_ID_3.
 * @param[in] pattern Pattern identifier, @ref APP_PATTERN_OFF through
 *                    @ref APP_PATTERN_MORSE_SOS.
 * @param[in] period  Pattern period in milliseconds; only the range
 *                    @ref APP_LED_MIN_PERIOD .. @ref APP_LED_MAX_PERIOD is
 *                    meaningful, and the task does not validate it.
 * @param[in] force   @c true allows the message to override LD3.
 *
 * @return @c true when the request was queued.
 * @retval false The task is not created yet or the queue is full; the
 *               request is dropped and no partial state is applied.
 *
 * @note  Never blocks: the queue send uses a zero timeout, so it is callable
 *        from any task.
 * @warning Not ISR-safe: the implementation uses @c xQueueSend rather than
 *          the @c FromISR variant, so do not call it from an ISR.
 * @post  On success the pattern is applied asynchronously; the LED may take
 *        up to one iteration (bounded by the next segment boundary) to change.
 * @see led_post_set_all
 */
bool led_post_set(uint8_t led, uint8_t pattern, uint16_t period, bool force);

/**
 * @brief Queue the same pattern for both LD1 and LD2.
 * @details Applies the pattern to the LD1/LD2 pair and restarts their phase
 *          together.  LD3 keeps its own configuration, so the error display
 *          is never overwritten by this call.  Requesting
 *          @ref APP_PATTERN_CHASE here is equivalent to a single-LED CHASE
 *          request, and any other pattern simply gives both LEDs that
 *          pattern in phase.
 *
 * @param[in] pattern Pattern identifier, @ref APP_PATTERN_OFF through
 *                    @ref APP_PATTERN_MORSE_SOS.
 * @param[in] period  Pattern period in milliseconds.
 *
 * @return @c true when the request was queued.
 * @retval false The task is not created yet or the queue is full.
 *
 * @note  Non-blocking; the queue depth is eight messages, so a burst of more
 *        than eight requests loses the excess.
 * @see led_post_set
 */
bool led_post_set_all(uint8_t pattern, uint16_t period);

/**
 * @brief Report a button press so the task can drive the demo sequence.
 * @details A short press advances LD1 to the next pattern in wrap order
 *          (OFF -> ON -> BLINK -> HEARTBEAT -> FAST -> CHASE -> MORSE_SOS ->
 *          OFF) and copies the result to LD2, keeping the current LD1 period
 *          unless it is outside the accepted range, in which case
 *          @ref APP_LED_DEFAULT_PERIOD is used.  A long press restores the
 *          defaults: LD1 back to @ref APP_PATTERN_HEARTBEAT and LD2 to
 *          @ref APP_PATTERN_OFF, both at their default periods.  The task
 *          also posts the resulting state to Comms as a ButtonEvent, which is
 *          what BTN-002 and BTN-003 require.
 *
 * @param[in] press Press kind: 1 for a short press, 2 for a long press.
 *
 * @return @c true when the event was queued.
 * @retval false The task is not created yet or the queue is full.
 *
 * @note  Any value other than 2 is treated as a short press.  The button task
 *        decides which value to send from the debounced press duration.
 * @see led_post_set_all
 */
bool led_post_button(uint8_t press); /* 1 = short, 2 = long */

/**
 * @brief Release the latched fault and watchdog indication on LD3 (LED-005).
 * @details Clears @ref APP_ERROR_FAULT and @ref APP_ERROR_WATCHDOG from the
 *          task's local copy of the error mask so LD3 returns to its
 *          configured pattern on the next iteration.
 *
 * @return @c true when the request was queued.
 * @retval false The task is not created yet or the queue is full.
 *
 * @warning This only affects the LED task's local accumulator.  The global
 *          latch must be cleared separately (Comms calls
 *          app_error_clear_fault() before posting this message); otherwise
 *          the still-set bits are ORed back in on the next iteration and LD3
 *          stays latched.
 * @note  @ref APP_ERROR_HEALTH is deliberately not cleared here, so a failed
 *        pending-slot health check keeps blinking after ClearFault.
 * @post  A latched FAULT/WATCHDOG no longer forces LD3 on; a pending HEALTH
 *        failure still does.
 * @see app_error_clear_fault
 */
bool led_post_clear_error(void);

/**
 * @brief Ask the LED task to show the failed health state on LD3.
 * @details Sets @ref APP_ERROR_HEALTH in the task's local error accumulator,
 *          which switches LD3 to a 100 ms blink (50 ms on, 50 ms off).
 *          The health task calls this together with
 *          app_error_set(APP_ERROR_HEALTH) when the pending-slot check fails,
 *          so both the latch and the display agree.
 *
 * @return @c true when the request was queued.
 * @retval false The task is not created yet or the queue is full; the
 *               indication then still appears through the global latch that
 *               the task ORs in on every iteration.
 *
 * @note  This message does not modify @ref app_error_flags; the producer is
 *        responsible for latching the bit.  Comms reports the same condition
 *        through health_state_get().
 * @post  The fast blink cannot be released by @ref led_post_clear_error.
 * @see led_post_clear_error
 */
bool led_post_health_failed(void);

/** @} */

#endif /* LED_TASK_H */
