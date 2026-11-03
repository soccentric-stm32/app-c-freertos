#
# CMakeLists.txt - FreeRTOS demo application (DEMO), NUCLEO-F767ZI
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Date:    2026-11-03
# Target:  STM32F767ZIT6
# SRS:     SRS-DEMO-F767 (DCON-001..003, DGEN-001..003, DBT-001, DRT-001)
#
# Build (inside the stm32-dev container, DCON-001):
#   cmake -S . -B build -G Ninja
#   cmake --build build
# or simply: scripts/build.sh [clean]
#
# Outputs in build/: app.elf, app.bin, app.hex, app.map (DGEN-001).
#

cmake_minimum_required(VERSION 3.22)

set(APP_VERSION "0.1.0")

# Cross toolchain: the build runs in the container where arm-none-eabi-gcc
# is the only compiler. Set explicitly so a stray host compiler is never used.
set(CMAKE_SYSTEM_NAME     Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY) # no OS to run a test exe in
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # compile_commands.json for clang-tidy (make lint)
find_program(ARMCC arm-none-eabi-gcc REQUIRED)
find_program(ARMOBJCOPY arm-none-eabi-objcopy REQUIRED)
find_program(SIZE_TOOL arm-none-eabi-size REQUIRED)
set(CMAKE_C_COMPILER   ${ARMCC})
set(CMAKE_ASM_COMPILER ${ARMCC})
set(CMAKE_OBJCOPY      ${ARMOBJCOPY})

project(app LANGUAGES C ASM)

# --- SDK location ----------------------------------------------------------
# STM32CubeF7 v1.17.4 is provided by the docker image (DCON-002).
set(CUBE_ROOT "$ENV{STM32CUBE_F7}")
if(NOT CUBE_ROOT)
    set(CUBE_ROOT "/opt/st/STM32CubeF7")
endif()
if(NOT EXISTS "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Inc/stm32f7xx_hal.h")
    message(FATAL_ERROR
        "STM32CubeF7 not found at ${CUBE_ROOT}.\n"
        "Build through scripts/docker-dev.sh (see docker/README.md).")
endif()

# --- Build metadata (DGEN-002) ---------------------------------------------
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short=12 HEAD
    OUTPUT_VARIABLE APP_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
string(TIMESTAMP APP_BUILD_DATE "%Y-%m-%d %H:%M:%S")

# --- Toolchain flags --------------------------------------------------------
set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(${CPU_FLAGS} -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
# No stdio and no libc heap (FreeRTOS heap_4 is static), so no syscall layer
# is linked.
add_link_options(${CPU_FLAGS} -Wl,--gc-sections -Wl,--cref
    -Wl,-Map=${CMAKE_BINARY_DIR}/app.map
    --specs=nano.specs)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2 -g3")
set(CMAKE_C_FLAGS_DEBUG   "-Og -g3")

add_compile_definitions(
    STM32F767xx
    USE_HAL_DRIVER
    HSE_VALUE=8000000U          # ST-LINK MCO feeds PH0 in bypass mode
    # DBT-002: SystemInit points VTOR at the slot (BOOT also sets it).
    USER_VECT_TAB_ADDRESS
    VECT_TAB_OFFSET=0x0000C000U
    APP_VERSION_STR="${APP_VERSION}"
    APP_SRS_ID="SRS-DEMO-F767"
    APP_GIT_COMMIT="${APP_GIT_COMMIT}"
    APP_BUILD_DATE="${APP_BUILD_DATE}")

include_directories(src board
    "${CUBE_ROOT}/Drivers/CMSIS/Include"
    "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F7xx/Include"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Inc")

# FreeRTOS heap size, configurable at build time (DRT-001).
set(APP_HEAP_KB "32" CACHE STRING "FreeRTOS heap_4 size in KB")
math(EXPR APP_HEAP_BYTES "${APP_HEAP_KB} * 1024")

# --- Sources ----------------------------------------------------------------
# HAL modules used so far. The list grows with each SRS section.
set(HAL_SOURCES
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_cortex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_rcc.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_rcc_ex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_gpio.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_pwr.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_pwr_ex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_flash.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_flash_ex.c")

set(PROJECT_SOURCES
    src/main.c
    src/clock.c
    src/hal_timebase.c
    src/led_task.c
    src/rtos_hooks.c)

set(DEVICE_SOURCES
    "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F7xx/Source/Templates/system_stm32f7xx.c"
    "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F7xx/Source/Templates/gcc/startup_stm32f767xx.s")

# FreeRTOS V10.2.0 kernel + native GCC CM7 port as shipped (DCON-002, DRT-001)
set(FREERTOS_ROOT "${CUBE_ROOT}/Middlewares/Third_Party/FreeRTOS/Source")
include_directories(
    "${FREERTOS_ROOT}/include"
    "${FREERTOS_ROOT}/portable/GCC/ARM_CM7/r0p1")
set(FREERTOS_SOURCES
    "${FREERTOS_ROOT}/list.c"
    "${FREERTOS_ROOT}/queue.c"
    "${FREERTOS_ROOT}/tasks.c"
    "${FREERTOS_ROOT}/timers.c"
    "${FREERTOS_ROOT}/portable/GCC/ARM_CM7/r0p1/port.c"
    "${FREERTOS_ROOT}/portable/MemMang/heap_4.c")

add_executable(app.elf ${PROJECT_SOURCES} ${HAL_SOURCES} ${FREERTOS_SOURCES} ${DEVICE_SOURCES})
target_compile_definitions(app.elf PRIVATE
    APP_RTOS_HEAP_SIZE=${APP_HEAP_BYTES})

# DBT-001: vector table at 0x0800C000 (bootloader slot), at most 976 KB;
# the linker script fails the build otherwise.
target_link_options(app.elf PRIVATE
    "-T${CMAKE_SOURCE_DIR}/ld/app.ld")

# --- Static analysis (DNFR-004) ----------------------------------------------
# clang-tidy reuses the compile flags this very configure step produced, so
# it must run from a configured build tree:  make lint / make lint-strict.
find_program(CLANG_TIDY clang-tidy REQUIRED)
option(APP_LINT_STRICT "Treat any clang-tidy finding as an error (lint-strict)" OFF)
if(APP_LINT_STRICT)
    set(APP_LINT_FLAGS --warnings-as-errors=*)
endif()

# The target runs with the build dir as CWD, so make source paths absolute.
set(APP_LINT_SOURCES)
foreach(_src ${PROJECT_SOURCES})
    list(APPEND APP_LINT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
endforeach()
add_custom_target(clang-tidy
    COMMAND ${CLANG_TIDY} -quiet -p ${CMAKE_BINARY_DIR} ${APP_LINT_FLAGS}
            ${APP_LINT_SOURCES}
    COMMENT "Running clang-tidy over the project sources"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    VERBATIM)

set(ELF "$<TARGET_FILE:app.elf>")
add_custom_command(TARGET app.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary  ${ELF} ${CMAKE_BINARY_DIR}/app.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex    ${ELF} ${CMAKE_BINARY_DIR}/app.hex
    COMMAND ${SIZE_TOOL} $<TARGET_FILE:app.elf>
    BYPRODUCTS ${CMAKE_BINARY_DIR}/app.bin ${CMAKE_BINARY_DIR}/app.hex
    COMMENT "Generating app.bin and app.hex")
