/*
 * board/app_board.h - NUCLEO-F767ZI pins used by the demo application
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (STM32F767ZIT6)
 * SRS:     DASM-002, DSAFE-003; pins from Hardware.md section 9
 */

#ifndef APP_BOARD_H
#define APP_BOARD_H

#include "stm32f7xx_hal.h"

#define APP_LED1_PORT           GPIOB      /* LD1 green, high = on         */
#define APP_LED1_PIN            GPIO_PIN_0
#define APP_LED2_PORT           GPIOB      /* LD2 blue                     */
#define APP_LED2_PIN            GPIO_PIN_7
#define APP_LED3_PORT           GPIOB      /* LD3 red: errors only (LED-005) */
#define APP_LED3_PIN            GPIO_PIN_14
#define APP_BTN1_PORT           GPIOC      /* B1, high when pressed        */
#define APP_BTN1_PIN            GPIO_PIN_13

/* APB prescalers of the 216 MHz tree (DGEN-003): APB1 54 MHz (timers
 * 108 MHz), APB2 108 MHz (timers 216 MHz). */
#define APP_APB1_PRESCALER      RCC_HCLK_DIV4
#define APP_APB2_PRESCALER      RCC_HCLK_DIV2

#endif /* APP_BOARD_H */
