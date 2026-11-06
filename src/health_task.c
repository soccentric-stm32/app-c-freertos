/*
 * health_task.c - task supervision, health check and CONFIRMED record
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DRT-012, DRT-013, HLT-001..HLT-005
 *
 * The Health task is the only task that refreshes the IWDG.  Every other
 * task sets a bit in its notification value at least once per second.  If a
 * bit is missing for 2 s the Health task stops refreshing the watchdog and
 * the 4 s IWDG in BOOT resets the board.  For a pending slot it also runs
 * the health check after 5 s and writes CONFIRMED once.
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

#define HEALTH_TASK_STACK_WORDS  384U
#define HEALTH_TASK_PRIORITY     (tskIDLE_PRIORITY + 5U)
#define HEALTH_LOOP_MS           20U
#define HEALTH_CHECKIN_TIMEOUT_MS 2000U
#define HEALTH_CHECK_AT_MS       5000U
#define HEALTH_STOP_KICK_MS      10000U
#define HEALTH_REFRESH_MS        100U
#define HEALTH_MIN_CHECKINS      5U
#define HEALTH_MIN_FREE_HEAP     4096U

#define HEALTH_STATE_NOT_REQUIRED 0U
#define HEALTH_STATE_RUNNING      1U
#define HEALTH_STATE_PASSED       2U
#define HEALTH_STATE_FAILED       3U

static TaskHandle_t health_task_handle;
static uint32_t checkin_count[HEALTH_TASK_COUNT + 1U];
static uint32_t last_seen[HEALTH_TASK_COUNT + 1U];
static uint32_t start_tick;
static uint32_t last_kick;
static volatile uint8_t health_state = HEALTH_STATE_NOT_REQUIRED;
static bool check_done;
static bool supervision_lost;

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

static void health_check_fail(void)
{
    check_done = true;
    health_state = HEALTH_STATE_FAILED;
    app_error_set(APP_ERROR_HEALTH);
    (void)led_post_health_failed();
}

static void health_check_pass(void)
{
    check_done = true;
    health_state = HEALTH_STATE_PASSED;
}

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
            if (health_conditions_met()) {
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
            } else if ((now - start_tick) >= HEALTH_STOP_KICK_MS) {
                health_check_fail();
                LOG_E("health: conditions not met, stopping the watchdog "
                      "kick (trial fails)");
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

bool health_task_create(void)
{
    return xTaskCreate(health_task, "Health", HEALTH_TASK_STACK_WORDS, NULL,
                       HEALTH_TASK_PRIORITY, &health_task_handle) == pdPASS;
}

void health_checkin(health_task_id_t task)
{
    if (health_task_handle == NULL || task >= HEALTH_TASK_COUNT) {
        return;
    }
    (void)xTaskNotify(health_task_handle, 1UL << (uint32_t)task, eSetBits);
}

uint8_t health_state_get(void)
{
    return health_state;
}

bool health_is_failed(void)
{
    return health_state == HEALTH_STATE_FAILED;
}
