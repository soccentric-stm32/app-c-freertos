/*
 * board/stm32f7xx_hal_conf.h - project HAL configuration
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, NUCLEO-F767ZI
 * SRS:     DCON-002 (drivers used as shipped; this is the project-owned
 *          configuration file)
 */

#ifndef STM32F7xx_HAL_CONF_H
#define STM32F7xx_HAL_CONF_H

/* Oscillator values in Hz. PH0 carries the ST-LINK 8 MHz MCO in bypass
 * mode (Hardware.md section 6). */
#define HSE_VALUE    8000000U
#define HSE_STARTUP_TIMEOUT 100U
#define HSI_VALUE   16000000U
#define LSI_VALUE    32000U
#define LSE_VALUE    32768U
#define LSE_STARTUP_TIMEOUT 5000U
#define EXTERNAL_SAI1_CLOCK_VALUE 48000U
#define EXTERNAL_SAI2_CLOCK_VALUE 48000U
#define EXTERNAL_CLOCK_VALUE 12288000U

#define VDD_VALUE   3300U
#define TICK_INT_PRIORITY 15U    /* TIM6 time base, below the kernel (DRT-003) */
#define USE_RTOS 0U
#define PREFETCH_ENABLE 1U

/* Enabled HAL modules - the list grows with each SRS section. */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED    /* RCC needs the latency macros; HLT-002 */

#include "stm32f7xx_hal_rcc.h"
#include "stm32f7xx_hal_gpio.h"
#include "stm32f7xx_hal_cortex.h"
#include "stm32f7xx_hal_pwr.h"
#include "stm32f7xx_hal_pwr_ex.h"
#include "stm32f7xx_hal_flash.h"
#include "stm32f7xx_hal_flash_ex.h"

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line);
#endif

/* assert_param: parameter checks compile out unless USE_FULL_ASSERT. */
#ifdef  USE_FULL_ASSERT
  #define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
  #define assert_param(expr) ((void)0U)
#endif

#endif /* STM32F7xx_HAL_CONF_H */
