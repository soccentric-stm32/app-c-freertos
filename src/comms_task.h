/**
 * @file    comms_task.h
 * @brief   HWLINK Comms task: frame reception, request dispatch and replies.
 * @details Declares the public surface of the single USART3 protocol task.
 *          The task owns the whole HWLINK data path: it drains the USART3 DMA
 *          ring, reassembles COBS frames, verifies every FlatBuffer with the
 *          flatcc verifier before use (COM-004), executes the DEMO request set
 *          and transmits exactly one reply per request (COM-003).  Requests it
 *          does not implement are answered with @c Nak UNSUPPORTED and change
 *          no state (COM-005).
 *
 *          The command set is the section 8.1 set: @c Hello, @c Ping,
 *          @c GetStatus, @c SetLed, @c SetLedAll, @c ClearFault,
 *          @c SetBaud, @c EnterBootloader and @c Reset.  The last two send the
 *          @c Ack and then reset into BOOT or the application (COM-006).
 *          Unsolicited output is limited to a @c ButtonEvent on B1 activity
 *          (BTN-004) and HWLINK frames only, never free text on USART3
 *          (COM-007).
 *
 *          Repeated request sequence numbers resend the previously built
 *          reply without executing the request a second time (LNK-008).  All
 *          buffers, the flatcc builder and the event queue are static and
 *          created once at start-up, so the task never allocates afterwards
 *          (DRT-004).  Reception errors and DMA aborts re-arm the receiver
 *          instead of killing the link, and the task checks in with the health
 *          task every 500 ms (COM-002, HLT-001).
 *
 *          This header only declares the task entry helpers; the parser, the
 *          command handlers and the reply builders are file-scope statics in
 *          @ref comms_task.c.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6
 * @srs     COM-001..COM-007, BTN-004, section 8
 * @ingroup comms_task
 */

#ifndef COMMS_TASK_H
#define COMMS_TASK_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup comms_task HWLINK Comms task and its event queue
 * @brief USART3 protocol task, its start-up helper and the two event posters.
 * @details The group covers the only task allowed to parse HWLINK traffic and
 *          the only task allowed to transmit on USART3.
 *
 *          **Queue ownership.**  @ref comms_task_create allocates one static
 *          FreeRTOS queue of 16 @c comms_event_t entries.  The queue is
 *          private to this module: @ref comms_task is its only consumer, and
 *          the only producers are @ref comms_post_rx_from_isr (UART ISR/DMA
 *          callbacks) and @ref comms_post_button_event (LED task).  Events
 *          are hints, not data: an RX event only wakes the task, which then
 *          copies bytes out of the DMA ring itself with
 *          @c app_uart3_rx_read.  Dropping an RX event therefore loses no
 *          bytes, and a full queue is not fatal.
 *
 *          **Task lifecycle.**  @ref comms_task_create must run on the
 *          start-up thread after the HAL, clocks and USART3 are up and before
 *          the scheduler starts.  It creates the queue, initialises the
 *          single flatcc builder and creates the @c Comms task at priority
 *          @c tskIDLE_PRIORITY + 4 with a 1536-word stack.  The task then runs
 *          forever: it blocks up to 10 ms for an event, drains queued events,
 *          polls the DMA ring, restarts the receiver when it has stopped,
 *          applies the 5 s baud revert (COM-001) and checks in with the health
 *          task every 500 ms.  It never returns and never suspends itself.
 *
 *          **Last-reply replay.**  After a reply is fully on the wire, the
 *          unencoded frame and the request sequence number are cached.  When
 *          the host retries the same sequence number the cached frame is
 *          COBS-encoded and resent without re-running the handler, so
 *          non-idempotent requests are executed once (LNK-008).  A new
 *          @c Hello clears the cache, and requests with sequence number 0 are
 *          never cached.  Only the most recent reply is kept.
 *
 *          **Timing.**  A request is answered from the task loop, so the
 *          worst-case latency is one loop period plus the transmit time; the
 *          @c Status builder is kept small enough to stay inside COM-003.
 * @note Task names, priorities and buffer sizes are compile-time constants in
 *       @ref comms_task.c; nothing here is configurable at run time.
 * @warning @ref comms_task_create is not idempotent and not thread-safe.
 *          Call it exactly once, before the scheduler, or the queue or the
 *          flatcc builder will be created twice.
 * @{
 */

/**
 * @brief Create the Comms task, its event queue and the flatcc builder.
 * @details Allocates the 16-entry event queue, initialises the module-wide
 *          flatcc builder used for every reply, then creates the @c Comms
 *          task with a 1536-word stack at @c tskIDLE_PRIORITY + 4.  The task
 *          is created in the ready state and starts running as soon as the
 *          scheduler is started, so it does not run during this call.
 *
 *          The three steps are ordered deliberately: a failure in any of them
 *          returns @c false so the caller can refuse to start a link that
 *          cannot answer requests.  Resources already created by an earlier,
 *          successful step are not released on a later failure.
 *
 * @return true when the queue, the builder and the task all exist.
 * @retval true  The @c Comms task is ready to be scheduled.
 * @retval false Queue creation, flatcc builder initialisation or
 *               @c xTaskCreate failed; the link is not usable.
 *
 * @pre HAL, the clocks, the caches/MPU and @c app_uart3_init have completed,
 *      and HWLINK callbacks may already post RX events.
 * @pre Called from the start-up thread before @c vTaskStartScheduler, while
 *      the FreeRTOS heap still has room for the stack and the queue.
 * @post On success @ref comms_post_rx_from_isr and
 *       @ref comms_post_button_event accept events instead of returning
 *       immediately.
 * @post On failure the module's queue handle remains @c NULL, so both posters
 *       stay no-ops.
 *
 * @note Called once from @c main() as part of the task creation sequence; the
 *       flatcc builder is shared by all reply paths and is reset (not
 *       re-initialised) after each message.
 * @warning Partial failures leak the queue and/or the builder.  There is no
 *          teardown path because the application treats a failed creation as
 *          fatal and never retries.
 * @see comms_post_rx_from_isr
 * @see comms_post_button_event
 */
bool comms_task_create(void);

/**
 * @brief Wake the Comms task after the UART receiver produced data or failed.
 * @details Posts a single @c COMMS_EVT_RX hint to the module queue using
 *          @c xQueueSendFromISR and yields if a higher-priority task was
 *          unblocked.  Called from the USART3 RX complete and half-complete
 *          callbacks, the UART error callback and the USART3 interrupt
 *          handler for receive errors and IDLE-line detection.
 *
 *          The event carries no payload: the task reads the bytes from the
 *          DMA ring in @c poll_receive, which it runs on every wake-up and
 *          once per loop period anyway.  Posts therefore coalesce, and a full
 *          queue only delays the wake-up, it does not drop received bytes.
 *          The ISR is re-armed separately by the task when
 *          @c app_uart3_rx_running reports the receiver has stopped
 *          (COM-002, HLT-001).
 *
 * @pre @ref comms_task_create has run; before that the queue handle is @c NULL
 *      and the call returns without touching FreeRTOS.
 * @post A successful post leaves one more RX hint in the queue; received
 *       bytes stay in the DMA ring until the task copies them.
 *
 * @note ISR context only.  It is not safe to call from a task, and it must not
 *       be used before the scheduler is running.
 * @note The queue-full result is deliberately ignored: the receiver is polled
 *       periodically, so the hint is advisory.
 * @warning Never call the task-level @c xQueueSend variant from the UART
 *          callbacks; only this wrapper is interrupt-safe.
 * @see comms_task_create
 * @see comms_post_button_event
 */
void comms_post_rx_from_isr(void);

/**
 * @brief Report a B1 press and the resulting LED pattern to the host.
 * @details Called by the LED task after it debounces a press and has already
 *          queued the new pattern.  The event carries the press kind, the
 *          pattern and the period so the Comms task can do two things:
 *          -# mirror the new LD1/LD2 state in its local @c led_mirror table so
 *             the next @c Status reply reports what the LEDs are doing;
 *          -# send an unsolicited @c ButtonEvent with sequence number 0 and
 *             the event flag, but only after a @c Hello has opened the link.
 *
 *          A long press (@p press == 2) is reported as the heartbeat reset
 *          that the LED task applies and forces the mirror entry to
 *          @ref APP_PATTERN_HEARTBEAT with @ref APP_LED_DEFAULT_PERIOD.  For a
 *          short press the mirror is only updated when @p pattern is below
 *          @ref APP_PATTERN_COUNT and @p period lies between
 *          @ref APP_LED_MIN_PERIOD and @ref APP_LED_MAX_PERIOD.
 *
 *          The event is dropped when the queue stays full for 20 ms.  The LED
 *          task has already applied the pattern, but the Comms task never
 *          sees the change, so the @c Status reply keeps reporting the old
 *          pattern until the next LED command or button event arrives.
 *
 * @param[in] press   Press kind: 1 for a short press, 2 for a long press.
 * @param[in] pattern LED pattern the button selected; one of the
 *                    @c APP_PATTERN_* values from @ref led_task.h.
 * @param[in] period  Pattern period in milliseconds, typically
 *                    @ref APP_LED_DEFAULT_PERIOD.
 *
 * @pre @ref comms_task_create has run; otherwise the call returns immediately.
 * @post Either one @c COMMS_EVT_BUTTON entry is queued, or the event was
 *       dropped after the 20 ms timeout.
 * @post When the event was queued, the Comms task mirrors the state and, if
 *       the link is open, sends a single @c ButtonEvent.
 *
 * @note Task context (the LED task), never an ISR.  The 20 ms wait implements
 *       BTN-004: a press must not be lost just because a burst of RX events
 *       filled the queue.
 * @note Values of @p press other than 1 and 2 are forwarded unchanged; the
 *       host decodes them with @c hwlink_ButtonPress_enum_t.
 * @warning Blocks the caller for up to 20 ms, which can delay the LED task
 *          and therefore pattern timing by that amount.
 * @see comms_post_rx_from_isr
 * @see led_post_button
 */
void comms_post_button_event(uint8_t press, uint8_t pattern, uint16_t period);

/** @} */

#endif /* COMMS_TASK_H */
