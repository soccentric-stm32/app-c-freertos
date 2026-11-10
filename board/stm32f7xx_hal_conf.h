/**
 * @file    stm32f7xx_hal_conf.h
 * @brief   Project-owned STM32F7 HAL configuration for the demo firmware.
 * @details Every STM32Cube HAL translation unit includes this file through
 *          @c stm32f7xx_hal.h before any driver header, so the macros below
 *          decide which drivers exist in the image and how they are
 *          parameterised.  The driver sources themselves are used as shipped
 *          from STM32CubeF7 v1.17.4 (DCON-002); only this configuration file
 *          is project code.
 *
 *          The file has three jobs:
 *          -# supply the oscillator, supply-voltage and tick settings that the
 *             HAL and the RCC clock tree expect (`DGEN-003`, `DBT-003`);
 *          -# select the HAL modules that the application links, one per
 *             peripheral actually used (`COM-001`, `COM-002`, `DRT-012`,
 *             `HLT-002`);
 *          -# define @c assert_param, the argument-check hook every driver
 *             calls.
 *
 *          The module list grows with each SRS section, in step with
 *          @c HAL_SOURCES in CMakeLists.txt: a switch without a compiled
 *          source file fails at link time, and a compiled source file without
 *          its switch produces no usable code.  The file is kept structurally
 *          identical to the HWVAL configuration so the two can share one copy
 *          later (`DGEN-007`, OPEN-D02).
 *
 * @note CMakeLists.txt also passes `HSE_VALUE=8000000U` on the compiler
 *       command line.  That duplicate is harmless only because the two token
 *       sequences are identical; if they ever differ, the compiler reports a
 *       macro redefinition.
 * @note Everything here is compile-time configuration.  Changing a value
 *       changes the HAL behaviour of every driver in the image, so treat it as
 *       build configuration rather than a tuning knob.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  STM32F767ZIT6, NUCLEO-F767ZI
 * @srs     DCON-002 (drivers used as shipped; this is the project-owned
 *          configuration file)
 * @ingroup hal_conf
 */

#ifndef STM32F7xx_HAL_CONF_H
#define STM32F7xx_HAL_CONF_H

/**
 * @defgroup hal_conf HAL configuration
 * @brief Clock sources, VDD, tick priority, HAL module set and assert hook.
 * @details Compile-time contract between the STM32Cube HAL and this
 *          application.  Nothing in this group executes; the values are read
 *          by the drivers, by @ref app_clock_init in @ref clock.c and by the
 *          project @c HAL_InitTick override in @ref hal_timebase.c.
 *
 *          Rule for extending the file: enable a module only when the SRS
 *          requires the peripheral, and add the matching source file to
 *          @c HAL_SOURCES in the same change.  The image is footprint-checked
 *          (128 KB flash / 64 KB RAM), so an unused driver is real cost.
 * @see stm32f7xx_hal.h, clock.c, hal_timebase.c, uart3.c, iwdg.c
 * @{
 */
/**
 * @name Oscillator and external clock values
 * @brief Frequencies in Hz, consumed by the RCC driver.
 * @details The values are nominal datasheet figures.  Only @ref HSE_VALUE is
 *          used at run time, because the clock tree is built exclusively from
 *          the external 8 MHz clock (`DGEN-003`); the others stay at the Cube
 *          defaults so that any future use of the corresponding oscillator is
 *          correctly parameterised.
 * @{
 */
/**
 * @brief Frequency of the high-speed external clock in Hz.
 * @details PH0 carries the ST-LINK MCO output at 8 MHz and the clock tree is
 *          configured for bypass mode, not crystal mode, so this is the
 *          frequency of a driven square wave (`DGEN-003`, `DCON-005`,
 *          Hardware.md section 6).  @ref app_clock_init derives 216 MHz from
 *          it: PLLM 8, PLLN 432, PLLP 2 for SYSCLK, PLLQ 9 for the 48 MHz
 *          domain.
 * @note Also defined on the compiler command line by CMakeLists.txt; both
 *       definitions must stay identical.
 * @warning The HSE pins (PH0/PH1) must never be reconfigured while this value
 *          is in use (`DCON-005`).
 */
#define HSE_VALUE 8000000U
/**
 * @brief HSE ready timeout in milliseconds.
 * @details Polled by @c HAL_RCC_OscConfig() while it waits for `HSERDY`.  In
 *          bypass mode there is no crystal to start, so 100 ms is pure safety
 *          margin; expiry is treated as a fatal clock failure
 *          (@ref app_fault_fatal with `APP_FAULT_STARTUP`).
 */
#define HSE_STARTUP_TIMEOUT 100U
/**
 * @brief Frequency of the internal 16 MHz RC oscillator in Hz.
 * @details The reset clock, so it clocks everything that runs before
 *          @ref app_clock_init (HAL init, the early IWDG refresh, the fault
 *          path) and the part of that function that programs the PLL.
 *          @ref clock.c requests `RCC_HSI_OFF` in the same oscillator
 *          configuration, so the 16 MHz figure matters for start-up only.
 */
#define HSI_VALUE 16000000U
/**
 * @brief Nominal frequency of the internal low-speed oscillator in Hz.
 * @details This is the IWDG kernel clock and the only clock that keeps
 *          running through a system reset, which is what makes the 4 s
 *          watchdog timeout independent of the clock tree (`DRT-012`).
 * @note The real LSI is far less accurate than 32 kHz; iwdg.c treats the
 *       resulting 4 s as nominal, not as a guaranteed value.
 */
#define LSI_VALUE 32000U
/**
 * @brief Nominal frequency of the 32.768 kHz external crystal in Hz.
 * @note Unused: the NUCLEO-F767ZI has no 32.768 kHz crystal fitted and the
 *       demo never starts the LSE.  Retained from the Cube template.  The
 *       retained backup-domain state used by the boot contract is held by VDD,
 *       not by an LSE-clocked RTC.
 */
#define LSE_VALUE 32768U
/**
 * @brief LSE ready timeout in milliseconds.
 * @note Unused for the same reason as @ref LSE_VALUE; the LSE is never
 *       started.
 */
#define LSE_STARTUP_TIMEOUT 5000U
/**
 * @brief External SAI1 master clock in Hz (48 kHz).
 * @note Unused: no SAI peripheral is enabled or configured in this build.
 *       Retained from the Cube template.
 */
#define EXTERNAL_SAI1_CLOCK_VALUE 48000U
/**
 * @brief External SAI2 master clock in Hz (48 kHz).
 * @note Unused, as @ref EXTERNAL_SAI1_CLOCK_VALUE.
 */
#define EXTERNAL_SAI2_CLOCK_VALUE 48000U
/**
 * @brief External I2S/SAI clock input in Hz (12.288 MHz).
 * @note Unused: no I2S or SAI clock source is selected in this build.
 *       Retained from the Cube template.
 */
#define EXTERNAL_CLOCK_VALUE 12288000U

/** @} */
/**
 * @name Supply, tick and runtime model
 * @brief Values that describe the board and the execution environment.
 * @{
 */
/**
 * @brief Nominal supply voltage in millivolts (3.3 V).
 * @details Documents the board rail that the clock configuration assumes:
 *          voltage scale 1 with over-drive is only valid at the supply voltage
 *          used by @ref app_clock_init for 216 MHz (`DGEN-003`).
 * @note Kept for the HAL configuration contract rather than as a switch: the
 *       clock setup assumes exactly this rail (voltage scale 1 at 216 MHz), so
 *       the value must stay in step with the supply documented in
 *       Hardware.md.
 */
#define VDD_VALUE 3300U
/**
 * @brief Priority of the HAL time base interrupt (TIM6), lowest urgency.
 * @details @c HAL_Init() hands this value to the project's @c HAL_InitTick
 *          override in @ref hal_timebase.c, which programs both
 *          `NVIC_SetPriority(TIM6_DAC_IRQn, 15)` and the global @c uwTickPrio.
 *          With the four implemented priority bits of `APP_NVIC_PRIO_BITS`,
 *          15 is the least urgent level, i.e. the same level as
 *          `configKERNEL_INTERRUPT_PRIORITY`.
 *
 *          Because 15 is numerically greater than
 *          `configMAX_SYSCALL_INTERRUPT_PRIORITY` (5 in FreeRTOSConfig.h), the
 *          tick is masked for the duration of every FreeRTOS critical section
 *          (`DRT-003`); the handler calls no FreeRTOS API, so this is the
 *          intended arrangement.
 * @note A masked tick loses counts, so the @c uwTick-based uptime reported in
 *       Status and read through @c HAL_GetTick() can lag real time by the sum
 *       of the critical sections.  It stays monotonic; do not use it for long
 *       absolute timing.
 */
#define TICK_INT_PRIORITY 15U
/**
 * @brief Set to 0: the HAL is built without RTOS awareness.
 * @details FreeRTOS owns SysTick (`DRT-001`), so the HAL time base was moved
 *          to TIM6 by the @c HAL_InitTick override in @ref hal_timebase.c.
 *          The HAL needs no RTOS hooks of its own for the drivers used here.
 */
#define USE_RTOS 0U
/**
 * @brief Records that the flash prefetch buffer is required at 216 MHz.
 * @details `DGEN-003` requires 7 wait states, ART and prefetch with the core
 *          at 216 MHz.  The enable itself is done explicitly by
 *          @ref app_clock_init with `__HAL_FLASH_PREFETCH_BUFFER_ENABLE()` and
 *          `__HAL_FLASH_ART_ENABLE()`, so this macro documents the decision
 *          rather than driving it.
 * @note The I-cache and D-cache, also part of `DGEN-003`, are not controlled
 *       here; `app_cache_mpu_init()` enables them (`cache_mpu.c`).
 */
#define PREFETCH_ENABLE 1U

/** @} */
/**
 * @name Enabled HAL modules
 * @brief One switch per peripheral the application actually uses.
 * @details Each `HAL_<X>_MODULE_ENABLED` makes the HAL compile the
 *          corresponding driver and expose its API.  The set below is the
 *          minimum for the clock tree, the three LEDs, the user button, the
 *          HWLINK UART and the watchdog.
 * @note Do not enable a module speculatively: every referenced driver adds
 *       flash to the image that `check_footprint.py` size-checks, and each one
 *       still needs its `HAL_SOURCES` entry.
 * @{
 */
/**
 * @brief Legacy top-level module switch from older Cube templates.
 * @note Leftover from those templates; the `HAL_*_MODULE_ENABLED` switches
 *       below are the ones the drivers test.  Kept so the file stays diffable
 *       against the HWVAL configuration (`DGEN-007`).
 */
#define HAL_MODULE_ENABLED
/**
 * @brief CMSIS core and NVIC helpers.
 * @details Provides @c NVIC_SetPriority()/@c NVIC_EnableIRQ(), used by
 *          @ref hal_timebase.c and by `app_log.c` for the USART6 interrupt,
 *          and the CMSIS core definitions behind the vector-table relocation
 *          and fault configuration that `main.c` does with raw `SCB` writes
 *          (`DBT-002`, `DGEN-004`).  Interrupt priority handling is what
 *          `DRT-003` constrains.
 */
#define HAL_CORTEX_MODULE_ENABLED
/**
 * @brief Reset and clock control.
 * @details Builds the 216 MHz tree from the 8 MHz HSE bypass clock
 *          (`DGEN-003`) starting from the reset state left by BOOT
 *          (`DBT-003`), and supplies @c HAL_RCC_GetPCLK1Freq() for the TIM6
 *          prescaler calculation in @ref hal_timebase.c.
 */
#define HAL_RCC_MODULE_ENABLED
/**
 * @brief GPIO driver.
 * @details Drives LD1/LD2/LD3 on PB0/PB7/PB14 and reads B1 on PC13
 *          (`led_task.c`, `button_task.c`), and configures PD8/PD9 as the
 *          USART3 alternate function (`uart3.c`, `DCON-004`).
 * @note The fault path programs the LED port with raw register writes so that
 *       it works before this driver is usable (`DGEN-004`).
 */
#define HAL_GPIO_MODULE_ENABLED
/**
 * @brief Power control, including the `_ex` over-drive API.
 * @details @ref app_clock_init selects voltage scale 1 and calls
 *          `HAL_PWREx_EnableOverDrive()` before raising SYSCLK to 216 MHz
 *          (`DGEN-003`); without over-drive the part must not run at that
 *          frequency.
 */
#define HAL_PWR_MODULE_ENABLED
/**
 * @brief Flash driver and wait-state programming.
 * @details Needed for two reasons: the RCC clock configuration requires the
 *          `FLASH_LATENCY_*` definitions from `stm32f7xx_hal_flash.h`, and
 *          `flash_confirm.c` programs the 32-byte CONFIRMED metadata record
 *          with @c HAL_FLASH_Program() (`HLT-002`, [BOOT] LAY-005).
 */
#define HAL_FLASH_MODULE_ENABLED
/**
 * @brief DMA driver.
 * @details Used by @ref uart3.h.  A circular DMA1 stream fills the 4 KB
 *          receive ring and a second stream sends HWLINK replies without
 *          blocking other tasks (`COM-002`).  The receive ring in `uart3.c`
 *          and the transmit frame buffer in `comms_task.c` both live in the
 *          non-cacheable `.dma_buffer` SRAM2 window (`cache_mpu.c`).
 */
#define HAL_DMA_MODULE_ENABLED
/**
 * @brief UART driver for USART3.
 * @details USART3 on the ST-LINK virtual COM port carries HWLINK and nothing
 *          else (`COM-001`, `DCON-004`); free-text logging uses USART6
 *          instead.  The driver supplies the handle and transfer API used by
 *          @ref uart3.h.
 */
#define HAL_UART_MODULE_ENABLED
/**
 * @brief Independent watchdog driver.
 * @details Enabled for the `DRT-012` / `HLT-003` supervision.  The driver
 *          header supplies the `IWDG_PRESCALER_*` and key-register definitions
 *          used by @ref iwdg.c; that file then writes the registers directly
 *          so the early refresh works before any driver state exists.
 */
#define HAL_IWDG_MODULE_ENABLED

/** @} */
/**
 * @brief Module headers included for every HAL translation unit.
 * @details The order follows the Cube template: the clock and GPIO headers
 *          come first because the later drivers use their types and RCC
 *          macros.  The `_ex` headers are listed explicitly because the base
 *          headers do not reliably provide the extended API, and this project
 *          uses extended functions (over-drive, flash flag clearing and flash
 *          programming).  All HAL headers are include-guarded, so the second
 *          inclusion from `stm32f7xx_hal.h` is harmless.
 * @note Every entry here must have a matching `HAL_*_MODULE_ENABLED` above and
 *       a source file in @c HAL_SOURCES.  Do not reorder: the sequence is
 *       inherited from the HWVAL configuration (`DGEN-007`).
 */
#include "stm32f7xx_hal_rcc.h"
#include "stm32f7xx_hal_gpio.h"
#include "stm32f7xx_hal_cortex.h"
#include "stm32f7xx_hal_pwr.h"
#include "stm32f7xx_hal_pwr_ex.h"
#include "stm32f7xx_hal_flash.h"
#include "stm32f7xx_hal_flash_ex.h"
#include "stm32f7xx_hal_dma.h"
#include "stm32f7xx_hal_uart.h"
#include "stm32f7xx_hal_iwdg.h"

/**
 * @brief Optional full-parameter-check mode.
 * @details When `USE_FULL_ASSERT` is defined, the HAL validates every argument
 *          and calls @c assert_failed() with the file name and line number
 *          instead of silently accepting a bad handle or an out-of-range
 *          value.  The build does not define it: neither CMakeLists.txt nor
 *          any source sets it.
 * @warning No definition of @c assert_failed() exists anywhere in the project.
 *          Defining `USE_FULL_ASSERT` therefore fails at link time; an
 *          implementation would have to forward to @ref app_fault_assert the
 *          way `configASSERT` does in `FreeRTOSConfig.h`.
 * @see assert_param
 */
#ifdef USE_FULL_ASSERT
/**
 * @brief Report a failing HAL argument check (supplied by the application).
 * @details Called from @ref assert_param when `USE_FULL_ASSERT` is defined.
 *          Only this prototype exists; the project provides no definition.
 *
 * @param[in] file Name of the source file that failed the check (`__FILE__`).
 * @param[in] line Line number of the failed check (`__LINE__`).
 *
 * @note A real implementation must not return: it should latch the fault and
 *       stop the board, as `configASSERT` does (`DGEN-004`).
 */
void assert_failed(uint8_t *file, uint32_t line);
#endif

/**
 * @brief HAL argument-check hook expanded by every driver entry point.
 * @details Drivers wrap their parameter validation in `assert_param(expr)`.
 *          With `USE_FULL_ASSERT` defined the expression is evaluated and, on
 *          failure, @c assert_failed() is called with `__FILE__` and
 *          `__LINE__`; otherwise the macro expands to `((void)0U)`.
 *
 * @param expr Boolean HAL parameter check, not evaluated in the default build.
 *
 * @note In the default build the expression is not evaluated at all, so the
 *       drivers perform no argument checking and an invalid handle or a bad
 *       range is accepted silently.  @c configASSERT() in `FreeRTOSConfig.h`
 *       (which routes to @ref app_fault_assert) is the check that is actually
 *       active.
 * @warning Do not put side effects in @p expr; they would disappear together
 *          with the check.
 */
#ifdef USE_FULL_ASSERT
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
#define assert_param(expr) ((void)0U)
#endif

/** @} */
#endif /* STM32F7xx_HAL_CONF_H */
