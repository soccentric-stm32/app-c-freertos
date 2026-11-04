/*
 * led_task.c - LED patterns and LD3 error display (section 6)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI
 * SRS:     LED-001..LED-007, BTN-002, BTN-003
 *
 * The task owns the pattern state.  Other tasks send queue messages; button
 * presses arrive through the same queue and the selected pattern is reported
 * back to Comms as a ButtonEvent.  Pattern transitions are scheduled to the
 * next tick boundary (LED-006), not to a fixed polling rate.
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "app_board.h"
#include "app_error.h"
#include "comms_task.h"
#include "health_task.h"
#include "led_task.h"

#define LED_TASK_STACK_WORDS    384U
#define LED_TASK_PRIORITY       (tskIDLE_PRIORITY + 2U)
#define LED_QUEUE_LENGTH        8U
#define LED_CHECKIN_MS          500U

#define LED_ERROR_FAST_PERIOD   100U
#define LED_DEFAULT_LED1_PERIOD 1000U
#define LED_DEFAULT_LED2_PERIOD 1000U

#define NO_TRANSITION           0xFFFFFFFFUL

typedef enum {
    LED_MSG_SET = 0,
    LED_MSG_SET_ALL,
    LED_MSG_BUTTON,
    LED_MSG_CLEAR_ERROR,
    LED_MSG_HEALTH_FAILED
} led_msg_type_t;

typedef struct {
    uint8_t type;
    uint8_t led;
    uint8_t pattern;
    uint8_t press;
    uint16_t period;
    bool force;
} led_msg_t;

typedef struct {
    uint8_t pattern;
    uint16_t period;
    uint32_t start;
} led_cfg_t;

static QueueHandle_t led_queue;

static uint32_t next_pattern(uint32_t current)
{
    return (current + 1U) % APP_PATTERN_COUNT;
}

static void led_write(GPIO_TypeDef *port, uint32_t pin, bool on)
{
    port->BSRR = on ? pin : (pin << 16U);
}

static uint32_t segments_for(const led_cfg_t *cfg, bool second_led,
                             uint16_t dur[20], bool on[20])
{
    uint32_t n = 0U;
    uint32_t period = cfg->period;

    switch (cfg->pattern) {
    case APP_PATTERN_OFF:
        break;

    case APP_PATTERN_ON:
        dur[n] = 0xFFFFU;
        on[n] = true;
        n++;
        break;

    case APP_PATTERN_BLINK:
        dur[n] = (uint16_t)(period / 2U);
        on[n] = true;
        n++;
        dur[n] = (uint16_t)(period - (period / 2U));
        on[n] = false;
        n++;
        break;

    case APP_PATTERN_FAST: {
        uint32_t fast = period / 5U;
        if (fast < 2U) {
            fast = 2U;
        }
        dur[n] = (uint16_t)(fast / 2U);
        on[n] = true;
        n++;
        dur[n] = (uint16_t)(fast - (fast / 2U));
        on[n] = false;
        n++;
        break;
    }

    case APP_PATTERN_HEARTBEAT: {
        uint32_t cycle = period < 250U ? 250U : period;
        dur[n] = 50U;
        on[n] = true;
        n++;
        dur[n] = 150U;
        on[n] = false;
        n++;
        dur[n] = 50U;
        on[n] = true;
        n++;
        if (cycle > 250U) {
            dur[n] = (uint16_t)(cycle - 250U);
            on[n] = false;
            n++;
        }
        break;
    }

    case APP_PATTERN_CHASE: {
        uint32_t half = period / 2U;
        if (second_led) {
            dur[n] = (uint16_t)half;
            on[n] = false;
            n++;
            dur[n] = (uint16_t)(period - half);
            on[n] = true;
        } else {
            dur[n] = (uint16_t)half;
            on[n] = true;
            n++;
            dur[n] = (uint16_t)(period - half);
            on[n] = false;
        }
        n++;
        break;
    }

    case APP_PATTERN_MORSE_SOS:
    default:
        /* ... --- ...  with a 100 ms unit. */
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 300U; on[n] = false; n++;
        dur[n] = 300U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 300U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 300U; on[n] = true;  n++;
        dur[n] = 300U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 100U; on[n] = false; n++;
        dur[n] = 100U; on[n] = true;  n++;
        dur[n] = 700U; on[n] = false; n++;
        break;
    }

    return n;
}

static void evaluate(const led_cfg_t *cfg, bool second_led, uint32_t now,
                     bool *level, uint32_t *next_ms)
{
    uint16_t dur[20];
    bool on[20];

    if (cfg->pattern == APP_PATTERN_OFF) {
        *level = false;
        *next_ms = NO_TRANSITION;
        return;
    }
    if (cfg->pattern == APP_PATTERN_ON) {
        *level = true;
        *next_ms = NO_TRANSITION;
        return;
    }

    uint32_t count = segments_for(cfg, second_led, dur, on);
    uint32_t cycle = 0U;
    for (uint32_t i = 0U; i < count; i++) {
        cycle += dur[i];
    }
    if (cycle == 0U) {
        *level = false;
        *next_ms = NO_TRANSITION;
        return;
    }

    uint32_t pos = (now - cfg->start) % cycle;
    uint32_t acc = 0U;
    for (uint32_t i = 0U; i < count; i++) {
        if (dur[i] == 0U) {
            continue;
        }
        if (pos < (acc + dur[i])) {
            *level = on[i];
            *next_ms = (acc + dur[i]) - pos;
            return;
        }
        acc += dur[i];
    }

    *level = false;
    *next_ms = cycle - pos;
}

static void send_button_event(uint8_t press, uint8_t pattern, uint16_t period)
{
    comms_post_button_event(press, pattern, period);
}

static void apply_set(led_cfg_t cfg[3], uint8_t led, uint8_t pattern,
                      uint16_t period, uint32_t now)
{
    if (led == APP_LED_ID_3) {
        cfg[2].pattern = pattern;
        cfg[2].period = period;
        cfg[2].start = now;
        return;
    }

    if (pattern == APP_PATTERN_CHASE) {
        cfg[0].pattern = APP_PATTERN_CHASE;
        cfg[0].period = period;
        cfg[0].start = now;
        cfg[1].pattern = APP_PATTERN_CHASE;
        cfg[1].period = period;
        cfg[1].start = now;
        return;
    }

    /* Leaving CHASE breaks the pair. */
    if (cfg[0].pattern == APP_PATTERN_CHASE ||
        cfg[1].pattern == APP_PATTERN_CHASE) {
        if (led == APP_LED_ID_1) {
            cfg[1].pattern = APP_PATTERN_OFF;
            cfg[1].period = period;
            cfg[1].start = now;
        } else {
            cfg[0].pattern = APP_PATTERN_OFF;
            cfg[0].period = period;
            cfg[0].start = now;
        }
    }

    uint32_t index = (led == APP_LED_ID_2) ? 1U : 0U;
    cfg[index].pattern = pattern;
    cfg[index].period = period;
    cfg[index].start = now;
}

static void handle_message(led_cfg_t cfg[3], const led_msg_t *msg,
                           uint32_t now, uint32_t *error_flags)
{
    switch (msg->type) {
    case LED_MSG_SET:
        if (msg->led == APP_LED_ID_3 && !msg->force) {
            return;
        }
        apply_set(cfg, msg->led, msg->pattern, msg->period, now);
        break;

    case LED_MSG_SET_ALL:
        apply_set(cfg, APP_LED_ID_1, msg->pattern, msg->period, now);
        cfg[1].pattern = msg->pattern;
        cfg[1].period = msg->period;
        cfg[1].start = now;
        break;

    case LED_MSG_BUTTON:
        if (msg->press == 2U) {   /* long: restore defaults */
            cfg[0].pattern = APP_PATTERN_HEARTBEAT;
            cfg[0].period = LED_DEFAULT_LED1_PERIOD;
            cfg[0].start = now;
            cfg[1].pattern = APP_PATTERN_OFF;
            cfg[1].period = LED_DEFAULT_LED2_PERIOD;
            cfg[1].start = now;
            send_button_event(2U, APP_PATTERN_HEARTBEAT,
                              LED_DEFAULT_LED1_PERIOD);
        } else {
            uint16_t period = cfg[0].period;
            if (period < APP_LED_MIN_PERIOD || period > APP_LED_MAX_PERIOD) {
                period = LED_DEFAULT_LED1_PERIOD;
            }
            uint8_t pattern = (uint8_t)next_pattern(cfg[0].pattern);
            apply_set(cfg, APP_LED_ID_1, pattern, period, now);
            cfg[1].pattern = pattern;
            cfg[1].period = period;
            cfg[1].start = now;
            send_button_event(1U, pattern, period);
        }
        break;

    case LED_MSG_CLEAR_ERROR:
        *error_flags = 0U;
        break;

    case LED_MSG_HEALTH_FAILED:
        *error_flags |= APP_ERROR_HEALTH;
        break;

    default:
        break;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    led_cfg_t cfg[3] = {
        { APP_PATTERN_HEARTBEAT, LED_DEFAULT_LED1_PERIOD, 0U },
        { APP_PATTERN_OFF,       LED_DEFAULT_LED2_PERIOD, 0U },
        { APP_PATTERN_OFF,       LED_DEFAULT_LED2_PERIOD, 0U }
    };
    uint32_t now = xTaskGetTickCount();
    cfg[0].start = now;
    cfg[1].start = now;
    cfg[2].start = now;
    uint32_t error_flags = app_error_flags;
    uint32_t last_checkin = now;

    for (;;) {
        now = xTaskGetTickCount();
        error_flags |= app_error_flags;

        bool led1_on = false;
        bool led2_on = false;
        bool led3_on = false;
        uint32_t next1 = NO_TRANSITION;
        uint32_t next2 = NO_TRANSITION;
        uint32_t next3 = NO_TRANSITION;

        evaluate(&cfg[0], false, now, &led1_on, &next1);
        evaluate(&cfg[1], true, now, &led2_on, &next2);

        if ((error_flags & APP_ERROR_HEALTH) != 0U) {
            uint32_t pos = now % LED_ERROR_FAST_PERIOD;
            led3_on = pos < (LED_ERROR_FAST_PERIOD / 2U);
            next3 = (led3_on ? (LED_ERROR_FAST_PERIOD / 2U)
                             : LED_ERROR_FAST_PERIOD) - pos;
        } else if ((error_flags & (APP_ERROR_FAULT | APP_ERROR_WATCHDOG)) != 0U) {
            led3_on = true;
            next3 = NO_TRANSITION;
        } else {
            evaluate(&cfg[2], false, now, &led3_on, &next3);
        }

        led_write(APP_LED1_PORT, APP_LED1_PIN, led1_on);
        led_write(APP_LED2_PORT, APP_LED2_PIN, led2_on);
        led_write(APP_LED3_PORT, APP_LED3_PIN, led3_on);

        if ((now - last_checkin) >= LED_CHECKIN_MS) {
            health_checkin(HEALTH_TASK_LED);
            last_checkin = now;
        }

        uint32_t wait = LED_CHECKIN_MS - (now - last_checkin);
        if (next1 < wait) {
            wait = next1;
        }
        if (next2 < wait) {
            wait = next2;
        }
        if (next3 < wait) {
            wait = next3;
        }
        if (wait == 0U) {
            wait = 1U;
        }

        led_msg_t msg;
        if (xQueueReceive(led_queue, &msg, pdMS_TO_TICKS(wait)) == pdPASS) {
            now = xTaskGetTickCount();
            handle_message(cfg, &msg, now, &error_flags);
            while (xQueueReceive(led_queue, &msg, 0U) == pdPASS) {
                handle_message(cfg, &msg, now, &error_flags);
            }
        }
    }
}

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

bool led_task_create(void)
{
    led_queue = xQueueCreate(LED_QUEUE_LENGTH, sizeof(led_msg_t));
    if (led_queue == NULL) {
        return false;
    }
    return xTaskCreate(led_task, "LED", LED_TASK_STACK_WORDS, NULL,
                       LED_TASK_PRIORITY, NULL) == pdPASS;
}

static bool led_post(const led_msg_t *msg)
{
    if (led_queue == NULL) {
        return false;
    }
    return xQueueSend(led_queue, msg, 0U) == pdPASS;
}

bool led_post_set(uint8_t led, uint8_t pattern, uint16_t period, bool force)
{
    led_msg_t msg = {
        .type = LED_MSG_SET,
        .led = led,
        .pattern = pattern,
        .period = period,
        .force = force
    };
    return led_post(&msg);
}

bool led_post_set_all(uint8_t pattern, uint16_t period)
{
    led_msg_t msg = {
        .type = LED_MSG_SET_ALL,
        .pattern = pattern,
        .period = period
    };
    return led_post(&msg);
}

bool led_post_button(uint8_t press)
{
    led_msg_t msg = {
        .type = LED_MSG_BUTTON,
        .press = press
    };
    return led_post(&msg);
}

bool led_post_clear_error(void)
{
    led_msg_t msg = { .type = LED_MSG_CLEAR_ERROR };
    return led_post(&msg);
}

bool led_post_health_failed(void)
{
    led_msg_t msg = { .type = LED_MSG_HEALTH_FAILED };
    return led_post(&msg);
}
