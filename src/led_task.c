/*
 * led_task.c - LED task (section 6)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI
 * SRS:     LED-001, LED-002 (default HEARTBEAT on LD1), LED-006
 *
 * Only the default pattern so far. The other Appendix A patterns, SetLed /
 * SetLedAll from the GUI (LED-003/004) and the button events (section 7)
 * attach with the Comms and Button tasks.
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "app_board.h"
#include "led_task.h"

#define LED_TASK_STACK_WORDS    256U
#define LED_TASK_PRIORITY       (tskIDLE_PRIORITY + 2U)

/* HEARTBEAT (Appendix A): two 50 ms flashes 200 ms apart in a 1 s period. */
#define HEARTBEAT_PERIOD_MS     1000U
#define HEARTBEAT_FLASH_MS      50U
#define HEARTBEAT_GAP_MS        150U

void app_leds_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(APP_LED1_PORT, APP_LED1_PIN | APP_LED2_PIN | APP_LED3_PIN,
                      GPIO_PIN_RESET);
    gpio.Pin   = APP_LED1_PIN | APP_LED2_PIN | APP_LED3_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(APP_LED1_PORT, &gpio);
}

void app_led3_set(bool on)
{
    APP_LED3_PORT->BSRR = on ? APP_LED3_PIN : ((uint32_t)APP_LED3_PIN << 16U);
}

static void led1_set(bool on)
{
    APP_LED1_PORT->BSRR = on ? APP_LED1_PIN : ((uint32_t)APP_LED1_PIN << 16U);
}

static void led_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();

    while (1) {
        led1_set(true);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_FLASH_MS));
        led1_set(false);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_GAP_MS));
        led1_set(true);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_FLASH_MS));
        led1_set(false);
        /* LED-006: the period is anchored to the tick, not to the delays. */
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

bool led_task_create(void)
{
    return xTaskCreate(led_task, "LED", LED_TASK_STACK_WORDS, NULL,
                       LED_TASK_PRIORITY, NULL) == pdPASS;
}
