/**
 * @file    button_task.c
 * @brief   User button B1 (PC13) debounce and press classification.
 * @details B1 on the NUCLEO-F767ZI is an active-high input on PC13, and
 *          EXTI line 13 wakes the Button task on both edges.  The task
 *          samples the pin at least every 10 ms and accepts a new level only
 *          after it has been stable for 30 ms.  A press shorter than 1 s
 *          selects the next LED pattern, a press that reaches 2 s while
 *          still held restores the defaults, and a press lasting from 1 s to
 *          just under 2 s produces no request (BTN-001..BTN-003).
 *
 *          The task owns the B1 GPIO and EXTI setup but never drives the
 *          LEDs itself.  It reports each press through led_post_button();
 *          the LED task applies the pattern and forwards a ButtonEvent to
 *          Comms, which sends it to the GUI when the link is open (BTN-004).
 *          The task also checks in with the health task every 500 ms.  The
 *          public interface is documented in @ref button_task.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI
 * @srs     BTN-001..BTN-004
 * @ingroup button_task
 */

#include "app_log.h"
#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_board.h"
#include "health_task.h"
#include "led_task.h"
#include "button_task.h"

#define BUTTON_TASK_STACK_WORDS  384U   /**< Stack depth in words, sized for app_log(). */

/**
 * @brief Priority of the Button task.
 * @details One step below the Comms task and two steps below the Health
 *          task, so a press is handled promptly without displacing either.
 */
#define BUTTON_TASK_PRIORITY     (tskIDLE_PRIORITY + 3U)

/**
 * @brief Debounce interval in milliseconds (30 ms).
 * @details A candidate level must differ from the accepted level for this
 *          long before it becomes stable.  The comparison uses tick deltas,
 *          so the interval is rounded up to whole 1 ms ticks.
 */
#define BUTTON_DEBOUNCE_MS       30U

/**
 * @brief Upper bound of a short press in milliseconds, exclusive.
 * @details A release with a debounced duration below 1000 ms posts a short
 *          press request; exactly 1000 ms is no longer short.
 */
#define BUTTON_SHORT_MAX_MS      1000U

/**
 * @brief Threshold of a long press in milliseconds, inclusive.
 * @details A press still held at 2000 ms is reported once as a long press
 *          and is not reported again on release.
 */
#define BUTTON_LONG_MS           2000U

/**
 * @brief Health check-in period in milliseconds (500 ms).
 * @details The task calls health_checkin() once this much time has elapsed
 *          since the previous check-in, keeping it well inside the Health
 *          task's 2 s supervision window.
 */
#define BUTTON_CHECKIN_MS        500U

/**
 * @brief Maximum wait between button samples in milliseconds (10 ms).
 * @details ulTaskNotifyTake() blocks for this long or until EXTI13 notifies
 *          the task, so an edge is serviced quickly while the loop still
 *          samples often enough for debounce and long-press timing.
 */
#define BUTTON_POLL_MS           10U

/**
 * @brief Handle of the Button task, used by the EXTI13 interrupt handler.
 * @details Set by button_task_create() when xTaskCreate() succeeds and
 *          @c NULL before that, which is why the ISR tests it before
 *          notifying.  The ISR only reads the handle; the task never clears
 *          it.
 */
static TaskHandle_t button_task_handle;

/**
 * @brief Read the undebounced level of B1.
 * @details Reads the GPIOC input data register directly and applies the
 *          active-high convention of the board: a set APP_BTN1_PIN bit means
 *          the button is pressed.  The function performs no debounce and
 *          does not touch the EXTI pending flag.
 *
 * @return @c true when PC13 is high (pressed), @c false when it is low.
 *
 * @note Task context only.  This is a single register read, so it is short
 *       and non-blocking; the interrupt handler does not call it.
 * @see APP_BTN1_PIN
 */
static bool button_raw(void)
{
    return (GPIOC->IDR & APP_BTN1_PIN) != 0U;
}

/**
 * @brief Configure B1 as a pulled-down input and arm EXTI13 on both edges.
 * @details Enables the GPIOC and SYSCFG clocks, configures PC13 as an input
 *          with a pull-down (B1 is high when pressed), routes EXTI13 to port
 *          C through SYSCFG_EXTICR4 field 1, and enables both the rising and
 *          falling triggers.  Any stale pending flag is cleared before
 *          EXTI15_10_IRQn is enabled at pre-emption priority 6 and
 *          subpriority 0.
 *
 * @pre HAL_Init() and the clock tree have run, so NVIC priority grouping is
 *      valid and SYSCFG can be clocked.
 * @post PC13 is an input and EXTI13 can wake the Button task on either edge.
 *
 * @note The NVIC priority value (6) is numerically greater than the kernel's
 *       configMAX_SYSCALL_INTERRUPT_PRIORITY (5), that is, it is a
 *       lower-urgency interrupt, so the handler may use the FreeRTOS FromISR
 *       API.
 * @warning The EXTI line is enabled before vTaskStartScheduler().  An edge in
 *          that start-up window enters EXTI15_10_IRQHandler() before any task
 *          has run; the handler only reads the already-created task handle
 *          and uses FromISR calls, but this path is outside normal operation.
 * @see button_task_create
 */
static void button_exti_init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin  = APP_BTN1_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLDOWN;   /* B1 is high when pressed */
    HAL_GPIO_Init(APP_BTN1_PORT, &gpio);

    /* EXTI13 -> port C (SYSCFG_EXTICR4, field 1). */
    SYSCFG->EXTICR[3] = (SYSCFG->EXTICR[3] & ~(0xFUL << 4U)) | (0x2UL << 4U);
    EXTI->IMR  |= (1UL << 13U);
    EXTI->RTSR |= (1UL << 13U);
    EXTI->FTSR |= (1UL << 13U);
    EXTI->PR    = (1UL << 13U);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/**
 * @brief EXTI15_10 interrupt handler, servicing B1 on EXTI line 13.
 * @details When the EXTI13 pending flag is set, the handler clears it by
 *          writing one to that bit and gives the Button task a notification.
 *          It yields from the ISR when the notification unblocked a task of
 *          higher priority.  Debouncing and press classification are left to
 *          the Button task.
 *
 * @note Interrupt context at priority 6.  The handler does not block, does
 *       not allocate and does not touch the GPIO or the task state beyond
 *       the notification.
 * @note Other EXTI lines 10 to 15 are ignored; B1 line 13 is the only line
 *       enabled by this application.  A future user of the same vector must
 *       be added here.
 * @warning The handler name is fixed by the start-up vector table and must
 *          not change; button_exti_init() enables the line.
 * @see button_exti_init
 * @see button_task
 */
void EXTI15_10_IRQHandler(void)
{
    if ((EXTI->PR & (1UL << 13U)) != 0U) {
        EXTI->PR = (1UL << 13U);
        if (button_task_handle != NULL) {
            BaseType_t woken = pdFALSE;
            vTaskNotifyGiveFromISR(button_task_handle, &woken);
            portYIELD_FROM_ISR(woken);
        }
    }
}

/**
 * @brief Sample B1 forever, debounce it, classify presses and check in.
 * @details The task seeds its debounce state from the first raw sample, so a
 *          button already held when the task starts is treated as an active
 *          press.  Each iteration blocks in ulTaskNotifyTake() for at most
 *          BUTTON_POLL_MS, or until the EXTI handler notifies it, then reads
 *          the raw pin.
 *
 *          Debounce keeps a candidate level and the tick at which it was
 *          first seen.  A candidate that differs from the accepted level
 *          restarts that timestamp; a candidate that stays different for
 *          BUTTON_DEBOUNCE_MS becomes the new accepted level.  On the
 *          accepted press edge the task records @c press_start and clears
 *          @c long_sent.  On the accepted release edge it computes the
 *          duration: below BUTTON_SHORT_MAX_MS it logs and posts
 *          led_post_button(1), and if the press was still active with no
 *          long press sent yet, a duration at or above BUTTON_LONG_MS posts
 *          led_post_button(2) as a release-time fallback.  The 1 s to 2 s gap
 *          deliberately posts nothing.
 *
 *          Independently of the release edge, a press still held at
 *          BUTTON_LONG_MS is posted exactly once as led_post_button(2), and
 *          @c long_sent suppresses a second request on release.  Every
 *          BUTTON_CHECKIN_MS the task also calls health_checkin() with
 *          @ref HEALTH_TASK_BUTTON.
 *
 * @param[in] arg Unused task argument; button_task_create() passes @c NULL.
 *
 * @note Never returns; the loop runs until the board resets.
 *
 * @pre The scheduler is running and the task was created successfully.
 * @post The task remains in its notification wait or check-in path; it never
 *       exits and never suspends itself.
 *
 * @note The notification value is consumed on every take (@c pdTRUE), so
 *       several edges that arrive before the task runs coalesce into one
 *       wake-up; the pin is re-read after every wake-up anyway.
 * @note All timing uses xTaskGetTickCount() deltas, so the 32-bit tick wrap
 *       is handled by unsigned arithmetic.
 * @note Durations are measured between accepted edges, not physical edges,
 *       so debounce latency (up to about 30 ms plus one poll) is included at
 *       both ends of a press.  A release still inside its debounce window
 *       when the 2 s long-press check runs can therefore be reported as a
 *       long press even though the physical hold ended just before 2 s.
 * @warning The log calls and led_post_button() are non-blocking, but the LED
 *          queue is finite: a full queue drops the request silently and the
 *          return value is ignored here.
 * @see button_raw
 * @see button_exti_init
 * @see led_post_button
 * @see health_checkin
 */
static void button_task(void *arg)
{
    (void)arg;
    uint32_t now = xTaskGetTickCount();
    bool stable = button_raw();
    bool candidate = stable;
    uint32_t candidate_since = now;
    uint32_t press_start = now;
    bool pressed = stable;
    bool long_sent = false;
    uint32_t last_checkin = now;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_POLL_MS));
        now = xTaskGetTickCount();
        bool raw = button_raw();

        if (raw != candidate) {
            candidate = raw;
            candidate_since = now;
        } else if (candidate != stable &&
                   (now - candidate_since) >= BUTTON_DEBOUNCE_MS) {
            stable = candidate;
            if (stable) {
                pressed = true;
                long_sent = false;
                press_start = now;
            } else {
                uint32_t duration = now - press_start;
                if (pressed && !long_sent) {
                    if (duration < BUTTON_SHORT_MAX_MS) {
                        LOG_I("button: short press, next LED pattern");
                        (void)led_post_button(1U);
                    } else if (duration >= BUTTON_LONG_MS) {
                        (void)led_post_button(2U);
                    }
                }
                pressed = false;
            }
        }

        if (pressed && !long_sent &&
            (now - press_start) >= BUTTON_LONG_MS) {
            long_sent = true;
            LOG_I("button: long press, default LED patterns");
            (void)led_post_button(2U);
        }

        if ((now - last_checkin) >= BUTTON_CHECKIN_MS) {
            health_checkin(HEALTH_TASK_BUTTON);
            last_checkin = now;
        }
    }
}

/**
 * @brief Create the Button task and then arm the B1 interrupt.
 * @details Calls xTaskCreate() with a NULL argument, the fixed name
 *          "Button", BUTTON_TASK_STACK_WORDS and BUTTON_TASK_PRIORITY, and
 *          stores the handle for the EXTI ISR.  Only after the task exists
 *          does it call button_exti_init(), so an interrupt cannot run
 *          against a NULL handle.  The task does not run until the caller
 *          starts the scheduler.
 *
 * @return @c true when the task was created and EXTI13 was configured.
 * @retval true  xTaskCreate() returned pdPASS.
 * @retval false xTaskCreate() failed; button_exti_init() was not called.
 *
 * @pre Called from start-up code before vTaskStartScheduler(), after
 *      HAL_Init() and the clock setup.
 * @post On success the task handle is valid and EXTI13 is armed for both
 *       edges.
 * @post On failure this call does not enable EXTI13; on a first call the
 *       handle stays @c NULL.
 *
 * @note Intended to be called once.  The LED queue should exist already so
 *       that later press requests can be queued.
 * @warning Not thread-safe.  A second call creates a second task and
 *          overwrites the stored handle; the first task remains with no
 *          handle pointing to it.
 * @see button_exti_init
 * @see button_task
 */
bool button_task_create(void)
{
    if (xTaskCreate(button_task, "Button", BUTTON_TASK_STACK_WORDS, NULL,
                    BUTTON_TASK_PRIORITY, &button_task_handle) != pdPASS) {
        return false;
    }
    button_exti_init();
    return true;
}
