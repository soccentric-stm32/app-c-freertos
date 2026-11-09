/**
 * @file    button_task.h
 * @brief   User button B1 (PC13) debounce and press classification.
 * @details B1 on the NUCLEO-F767ZI is an active-high input on PC13.  EXTI
 *          line 13 is armed for both edges and wakes the Button task, which
 *          samples the pin and accepts a new level only after it has been
 *          stable for 30 ms (BTN-001).  A press shorter than 1 s is reported
 *          as a short press (BTN-002); a press that reaches 2 s while still
 *          held is reported once as a long press (BTN-003).  Durations from
 *          1 s up to but not including 2 s are neither short nor long and
 *          produce no request.
 *
 *          The task does not drive the LEDs or the HWLINK link directly.
 *          It calls led_post_button() with 1 for a short press or 2 for a
 *          long press; the LED task applies the pattern and forwards a
 *          ButtonEvent to Comms, which sends it to the GUI when a link is
 *          open (BTN-004).  The LED queue is non-blocking, so a full queue
 *          drops the request and the return value is ignored.  The later
 *          Comms hop is also best effort and needs an open link to report.
 *
 *          The Button task runs forever at tskIDLE_PRIORITY + 3, samples at
 *          least every 10 ms, and checks in with the health task every
 *          500 ms.  It is created before the scheduler starts.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI
 * @srs     BTN-001..BTN-004
 * @ingroup button_task
 */

#ifndef BUTTON_TASK_H
#define BUTTON_TASK_H

#include <stdbool.h>

/**
 * @defgroup button_task Button B1 handling
 * @brief Debounced user-button input and short/long press requests.
 * @details The public API only creates the Button task.  Press reporting is
 *          asynchronous: the task owns the GPIO and EXTI setup and sends
 *          requests through the LED task instead of touching LED state.
 * @{
 */

/**
 * @brief Create the Button task and configure EXTI line 13 for B1.
 * @details Creates a 384-word task named "Button" at
 *          tskIDLE_PRIORITY + 3, then configures the B1 input and its
 *          interrupt path:
 *
 *          - enables the GPIOC and SYSCFG clocks and configures PC13 as a
 *            pull-down input, matching the active-high button;
 *          - routes EXTI13 to port C, enables both-edge triggers, and clears
 *            any stale pending flag;
 *          - enables EXTI15_10_IRQn at pre-emption priority 6, subpriority 0.
 *
 *          The interrupt handler clears the EXTI13 pending bit and notifies
 *          the task; it does not debounce or classify the press itself.  The
 *          task starts running only when the caller starts the scheduler.
 *
 * @return true when the task and the EXTI line are ready.
 * @retval true  xTaskCreate() returned pdPASS and EXTI13 was configured.
 * @retval false xTaskCreate() failed; the EXTI setup is not attempted.
 *
 * @pre Called from start-up code before vTaskStartScheduler().  The LED
 *      task should already exist so that later press requests have a queue.
 * @post On success the Button task exists and EXTI13 is armed for both edges.
 * @note Intended for start-up only; do not call from interrupt context.
 * @warning A second call creates another task and overwrites the stored task
 *          handle; the function has no guard against repeated calls.
 * @see led_post_button
 * @see health_checkin
 */
bool button_task_create(void);

/** @} */

#endif /* BUTTON_TASK_H */
