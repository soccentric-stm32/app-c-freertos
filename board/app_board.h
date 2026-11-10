/**
 * @file    app_board.h
 * @brief   NUCLEO-F767ZI pin map and bus prescalers used by the demo.
 * @details Single source of truth for the board pins that the application
 *          touches: the three user LEDs LD1 (green), LD2 (blue) and LD3
 *          (red) on port B and the user button B1 on port C, plus the APB1
 *          and APB2 prescalers of the 216 MHz clock tree (DGEN-003).  All
 *          three LEDs and B1 are active high; the LED pins are configured as
 *          push-pull outputs by @ref app_leds_init and B1 is configured as a
 *          pulled-down input by @ref button_task_create.
 *
 *          The header defines constants only.  It performs no hardware
 *          access, allocates nothing and is safe to include from tasks,
 *          from ISRs and from pre-scheduler start-up code, so the same pin
 *          definitions are shared by the normal LED path and by the
 *          fault/assert path in @ref app_fault.h (DGEN-004).
 * @note    The LEDs on PB0, PB7, PB14 and B1 on PC13 are the only pins the
 *          demo uses as plain GPIO; USART3 (PD8/PD9) and the debug UART
 *          (PG14) are owned by their peripherals (DSAFE-003).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     DASM-002, DSAFE-003; pins from Hardware.md section 9
 * @ingroup app_board
 */

/**
 * @defgroup app_board Board pin map and APB prescalers
 * @brief LED and button pin assignments plus the clock-tree prescalers.
 * @details The group is a pure compile-time contract between the board
 *          documentation (Hardware.md section 9), the clock start-up code in
 *          @ref clock.h and the GPIO users in @ref led_task.h,
 *          @ref button_task.h and @ref app_fault.h.  Consumers take the port
 *          and pin as a pair; the pin macros are HAL masks intended for
 *          @c HAL_GPIO_Init(), @c HAL_GPIO_WritePin() and @c BSRR/IDR
 *          register writes.
 * @note    The macros carry no type and no storage; they must stay usable
 *          from code that runs before the HAL and before the C run-time is
 *          fully set up (DGEN-004).
 * @{
 */

/**
 * @brief Include guard of @ref app_board.h.
 * @details Defined empty after the first inclusion so that repeated
 *          inclusion of the pin map is harmless.
 */
#ifndef APP_BOARD_H
#define APP_BOARD_H

#include "stm32f7xx_hal.h"

/**
 * @brief GPIO port of LD1, the green user LED.
 * @details LD1 sits on port B (DASM-002).  The port clock is enabled once by
 *          @ref app_leds_init, which also uses this macro as the @c GPIO_Init()
 *          port for all three LED pins.
 * @note The LD1 connection depends on a solder bridge: the default bridge
 *       selects PB0, the alternative selects PA5 (OPEN-D03).  Changing it to
 *       PA5 requires a new port macro and a matching clock enable.
 * @see APP_LED1_PIN
 */
#define APP_LED1_PORT GPIOB

/**
 * @brief Pin mask of LD1 on @ref APP_LED1_PORT (PB0).
 * @details Push-pull output, high = LED on (DASM-002).  LD1 is the primary
 *          user indicator: it starts with @ref APP_PATTERN_HEARTBEAT and is
 *          the LED steered by a short press of B1.
 */
#define APP_LED1_PIN GPIO_PIN_0

/**
 * @brief GPIO port of LD2, the blue user LED.
 * @details LD2 shares port B with LD1 and LD3 (DASM-002); the same
 *          @c GPIO_Init() call in @ref app_leds_init covers it.
 * @see APP_LED2_PIN
 */
#define APP_LED2_PORT GPIOB

/**
 * @brief Pin mask of LD2 on @ref APP_LED2_PORT (PB7).
 * @details Push-pull output, high = LED on (DASM-002).  LD2 forms a pair
 *          with LD1: @ref APP_PATTERN_CHASE set on either one is applied to
 *          both, and LD2 starts in @ref APP_PATTERN_OFF.
 */
#define APP_LED2_PIN GPIO_PIN_7

/**
 * @brief GPIO port of LD3, the red status/error LED.
 * @details LD3 shares port B with LD1 and LD2 (DASM-002).  It is also
 *          driven through this macro by @ref app_led3_set.
 * @warning The register-level fault path in @ref app_fault.c hard-codes
 *          GPIOB and pin 14 instead of using @ref APP_LED3_PORT and
 *          @ref APP_LED3_PIN, so moving LD3 to another port needs a matching
 *          edit there.
 * @see APP_LED3_PIN
 */
#define APP_LED3_PORT GPIOB

/**
 * @brief Pin mask of LD3 on @ref APP_LED3_PORT (PB14).
 * @details Push-pull output, high = LED on (DASM-002).  It is a plain
 *          output with no alternate function and no EXTI line attached.
 * @note LD3 is reserved for error and health indication and is not a
 *       general-purpose pattern LED: a pending health failure produces a
 *       100 ms fast blink (HLT-003), a latched fault or watchdog reset keeps
 *       it solid (LED-005), and a requested pattern is only applied when
 *       @ref led_post_set passes @c force.  The fault path also flashes LD3
 *       three times before resetting (DGEN-004).
 */
#define APP_LED3_PIN GPIO_PIN_14

/**
 * @brief GPIO port of B1, the blue user button.
 * @details B1 is on port C (DASM-002); @ref button_task_create enables the
 *          port clock and configures the pin as a pulled-down input.
 * @see APP_BTN1_PIN
 */
#define APP_BTN1_PORT GPIOC

/**
 * @brief Pin mask of B1 on @ref APP_BTN1_PORT (PC13).
 * @details Input, high when pressed (DASM-002).  The pin is read directly
 *          from @c GPIOC->IDR by the button task and is routed to EXTI line
 *          13, which wakes the task on both edges (BTN-001); the 30 ms
 *          debounce is implemented in software, not in hardware.
 * @note EXTI13 is selected for port C in SYSCFG_EXTICR4 and serviced by
 *       @c EXTI15_10_IRQHandler() at NVIC priority 6, i.e. below the
 *       kernel's @c configMAX_SYSCALL_INTERRUPT_PRIORITY (DRT-003), so the
 *       handler may use the FromISR notification API.
 * @warning B1 drives the pin high only while it is pressed, so the input
 *          must stay configured with a pull-down; a pull-up would invert the
 *          level and read as a permanent press.
 */
#define APP_BTN1_PIN GPIO_PIN_13

/**
 * @brief APB1 prescaler of the 216 MHz clock tree, HCLK divided by 4.
 * @details HCLK is 216 MHz (DGEN-003), so PCLK1 = 54 MHz and the APB1 timer
 *          clock is twice that, 108 MHz.  Used as @c APB1CLKDivider in
 *          app_clock_init() in @ref clock.c.
 * @note APB1 hosts the peripherals that must stay at or below 54 MHz: PWR,
 *       IWDG and USART3.  Raising PCLK1 beyond the limit violates the
 *       datasheet maximum for that bus.
 */
#define APP_APB1_PRESCALER RCC_HCLK_DIV4

/**
 * @brief APB2 prescaler of the 216 MHz clock tree, HCLK divided by 2.
 * @details HCLK is 216 MHz (DGEN-003), so PCLK2 = 108 MHz and the APB2 timer
 *          clock is twice that, 216 MHz; SYSCFG, which routes EXTI13 to
 *          @ref APP_BTN1_PORT, lives on this bus.  Used as @c APB2CLKDivider
 *          in app_clock_init() in @ref clock.c.
 * @note APB2 is the fast peripheral bus and PCLK2 is already at its 108 MHz
 *       maximum, so the divider cannot be reduced; raising it would slow
 *       SYSCFG and USART access.
 */
#define APP_APB2_PRESCALER RCC_HCLK_DIV2

#endif /* APP_BOARD_H */

/** @} */
