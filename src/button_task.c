/*
 * button_task.c - user button B1 (PC13)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI
 * SRS:     BTN-001..BTN-004
 *
 * B1 is high when pressed.  EXTI13 wakes the task on both edges; a 30 ms
 * software debounce filters the contact.  A short press (< 1 s) selects the
 * next pattern, a long press (>= 2 s) restores the defaults.  The LED task
 * owns the patterns; this task only reports the press through its queue.
 */

#include "app_log.h"
#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_board.h"
#include "health_task.h"
#include "led_task.h"
#include "button_task.h"

#define BUTTON_TASK_STACK_WORDS  384U   /* room for app_log() formatting */
#define BUTTON_TASK_PRIORITY     (tskIDLE_PRIORITY + 3U)
#define BUTTON_DEBOUNCE_MS       30U
#define BUTTON_SHORT_MAX_MS      1000U
#define BUTTON_LONG_MS           2000U
#define BUTTON_CHECKIN_MS        500U
#define BUTTON_POLL_MS           10U

static TaskHandle_t button_task_handle;

static bool button_raw(void)
{
    return (GPIOC->IDR & APP_BTN1_PIN) != 0U;
}

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

bool button_task_create(void)
{
    if (xTaskCreate(button_task, "Button", BUTTON_TASK_STACK_WORDS, NULL,
                    BUTTON_TASK_PRIORITY, &button_task_handle) != pdPASS) {
        return false;
    }
    button_exti_init();
    return true;
}
