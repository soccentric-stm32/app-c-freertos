/*
 * app - FreeRTOS demo application entry point
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (STM32F767ZIT6), started by BOOT from slot A or B
 * SRS:     DBT-002, DBT-003, DGEN-003, DRT-001, DRT-004, LED-002
 *
 * Implemented so far: clocks, HAL time base, LEDs and the LED task.
 * Still to come, section by section: Health (IWDG + CONFIRMED record,
 * section 9), Comms (USART3 + HWLINK, section 8), Button (section 7),
 * the boot hand-over record (DBT-004) and the image file (DGEN-001).
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "clock.h"
#include "led_task.h"

int main(void)
{
    /* DBT-002: SystemInit already pointed VTOR at the slot (0x0800C000);
     * the IWDG refresh joins here once BOOT starts it (TRY-004). */
    (void)HAL_Init();
    app_clock_init();
    app_leds_init();

    /* DRT-004: every task exists before the scheduler starts. */
    if (!led_task_create()) {
        app_led3_set(true);
        while (1) {
            __NOP();
        }
    }

    vTaskStartScheduler();

    /* Only reached when the idle task cannot be created. */
    app_led3_set(true);
    while (1) {
        __NOP();
    }
}
