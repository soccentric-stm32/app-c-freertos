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
set(APP_FIRMWARE_NAME "stm32-application")
option(DEMO_FAIL_HEALTH "Force the pending-slot health check to fail (HLT-004)" OFF)

# APP_VERSION feeds the firmware macros and the 8-bit image-header fields, so
# the shape and each component's range must be checked before the split.
if(NOT APP_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR
        "APP_VERSION must be major.minor.patch with decimal components, "
        "got '${APP_VERSION}'.")
endif()
foreach(part IN ITEMS "${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
    if(part GREATER 255)
        message(FATAL_ERROR
            "APP_VERSION components must be 0..255 for the 8-bit image-header "
            "fields, got '${APP_VERSION}'.")
    endif()
endforeach()

string(REPLACE "." ";" APP_VERSION_PARTS "${APP_VERSION}")
list(GET APP_VERSION_PARTS 0 APP_VERSION_MAJOR)
list(GET APP_VERSION_PARTS 1 APP_VERSION_MINOR)
list(GET APP_VERSION_PARTS 2 APP_VERSION_PATCH)

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
execute_process(COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse HEAD
    OUTPUT_VARIABLE APP_GIT_COMMIT_FULL OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT APP_GIT_COMMIT)
    set(APP_GIT_COMMIT "unknown")
    set(APP_GIT_COMMIT_FULL "")
endif()
string(TIMESTAMP APP_BUILD_DATE "%Y-%m-%d %H:%M:%S" UTC)
string(TIMESTAMP APP_BUILD_TIMESTAMP "%s" UTC)

# --- Toolchain flags --------------------------------------------------------
set(CPU_FLAGS -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(${CPU_FLAGS} -Wall -Wextra -Werror -ffunction-sections -fdata-sections)
# flatcc runtime allocation hooks (DRT-004) must be visible wherever the
# runtime headers expand FLATCC_ALLOC.  Applied to C sources only.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -include ${CMAKE_SOURCE_DIR}/src/flatcc_port.h")
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
    APP_VERSION_MAJOR=${APP_VERSION_MAJOR}
    APP_VERSION_MINOR=${APP_VERSION_MINOR}
    APP_VERSION_PATCH=${APP_VERSION_PATCH}
    APP_FIRMWARE_NAME="${APP_FIRMWARE_NAME}"
    APP_SRS_ID="SRS-DEMO-F767"
    APP_GIT_COMMIT="${APP_GIT_COMMIT}"
    APP_BUILD_DATE="${APP_BUILD_DATE}"
    APP_BUILD_TIMESTAMP=${APP_BUILD_TIMESTAMP})

include_directories(src board
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_SOURCE_DIR}/src/generated"
    "${CMAKE_SOURCE_DIR}/third_party/flatcc/include"
    "${CUBE_ROOT}/Drivers/CMSIS/Include"
    "${CUBE_ROOT}/Drivers/CMSIS/Device/ST/STM32F7xx/Include"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Inc")

# FreeRTOS heap size, configurable at build time (DRT-001).
set(APP_HEAP_KB "24" CACHE STRING "FreeRTOS heap_4 size in KB")
math(EXPR APP_HEAP_BYTES "${APP_HEAP_KB} * 1024")
# Fixed arena for the flatcc builder/verifier (DRT-004: no malloc).
set(APP_FLATCC_HEAP_KB "12" CACHE STRING "flatcc static arena size in KB")
math(EXPR APP_FLATCC_HEAP_BYTES "${APP_FLATCC_HEAP_KB} * 1024")

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
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_flash_ex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_dma.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_dma_ex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_uart.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_uart_ex.c"
    "${CUBE_ROOT}/Drivers/STM32F7xx_HAL_Driver/Src/stm32f7xx_hal_iwdg.c")

# The generated flatcc headers use compact macro formatting that trips
# -Wmisleading-indentation in the one translation unit that includes them.
set_source_files_properties(src/comms_task.c PROPERTIES
    COMPILE_OPTIONS "-Wno-misleading-indentation")

set(PROJECT_SOURCES
    src/main.c
    src/clock.c
    src/hal_timebase.c
    src/cache_mpu.c
    src/crc32.c
    src/identity.c
    src/hwlink.c
    src/flatcc_port.c
    src/app_error.c
    src/app_fault.c
    src/app_log.c
    src/reset_cause.c
    src/iwdg.c
    src/handover.c
    src/flash_confirm.c
    src/uart3.c
    src/led_task.c
    src/button_task.c
    src/health_task.c
    src/comms_task.c
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

# flatcc 0.6.1 runtime as released (DCON-002).  Only builder, verifier and
# emitter are linked; JSON support is not used.
set(FLATCC_ROOT "${CMAKE_SOURCE_DIR}/third_party/flatcc")
set(FLATCC_SOURCES
    "${FLATCC_ROOT}/src/runtime/builder.c"
    "${FLATCC_ROOT}/src/runtime/verifier.c"
    "${FLATCC_ROOT}/src/runtime/emitter.c"
    "${FLATCC_ROOT}/src/runtime/refmap.c")

add_executable(app.elf ${PROJECT_SOURCES} ${HAL_SOURCES} ${FREERTOS_SOURCES}
    ${DEVICE_SOURCES} ${FLATCC_SOURCES})
target_compile_definitions(app.elf PRIVATE
    NDEBUG=1
    APP_RTOS_HEAP_SIZE=${APP_HEAP_BYTES}
    APP_FLATCC_HEAP_SIZE=${APP_FLATCC_HEAP_BYTES}
    FLATCC_ALLOC=app_flatcc_alloc
    FLATCC_FREE=app_flatcc_free
    FLATCC_REALLOC=app_flatcc_realloc
    FLATCC_CALLOC=app_flatcc_calloc
    FLATCC_ALIGNED_ALLOC=app_flatcc_aligned_alloc
    FLATCC_ALIGNED_FREE=app_flatcc_aligned_free
    FLATCC_EMITTER_ALLOC=app_flatcc_alloc
    FLATCC_EMITTER_FREE=app_flatcc_free)
if(DEMO_FAIL_HEALTH)
    target_compile_definitions(app.elf PRIVATE DEMO_FAIL_HEALTH=1)
endif()

# DBT-001: vector table at 0x0800C000 (bootloader slot), at most 976 KB;
# the linker script fails the build otherwise.
target_link_options(app.elf PRIVATE
    "-T${CMAKE_SOURCE_DIR}/ld/app.ld")

# --- Static analysis (DNFR-004) ----------------------------------------------
# clang-tidy reuses the compile flags this very configure step produced, so
# it must run from a configured build tree:  make lint / make lint-strict.
# The tool is optional: a normal firmware build must still configure on a host
# that has no clang-tidy, and only the lint target then reports the missing
# dependency.
find_program(CLANG_TIDY clang-tidy)
option(APP_LINT_STRICT "Treat any clang-tidy finding as an error (lint-strict)" OFF)
if(APP_LINT_STRICT)
    set(APP_LINT_FLAGS --warnings-as-errors=*)
endif()

# The target runs with the build dir as CWD, so make source paths absolute.
set(APP_LINT_SOURCES)
foreach(_src ${PROJECT_SOURCES})
    list(APPEND APP_LINT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
endforeach()
math(EXPR MAX_FLASH_BYTES "128 * 1024")
math(EXPR MAX_RAM_BYTES "64 * 1024")

# clang-tidy runs the host clang but needs the newlib headers used by the
# arm-none-eabi toolchain in the container; otherwise <string.h> is missing.
set(APP_LINT_EXTRA_ARGS --extra-arg=-isystem
    --extra-arg=/usr/lib/arm-none-eabi/include)

if(CLANG_TIDY)
    add_custom_target(clang-tidy
        COMMAND ${CLANG_TIDY} -quiet -p ${CMAKE_BINARY_DIR} ${APP_LINT_FLAGS}
                ${APP_LINT_EXTRA_ARGS} ${APP_LINT_SOURCES}
        COMMENT "Running clang-tidy over the project sources"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM)
else()
    add_custom_target(clang-tidy
        COMMAND ${CMAKE_COMMAND} -E echo
                "clang-tidy not found; install it to run make lint"
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "clang-tidy is not available"
        VERBATIM)
endif()

set(ELF "$<TARGET_FILE:app.elf>")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(TARGET app.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary  ${ELF} ${CMAKE_BINARY_DIR}/app.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex    ${ELF} ${CMAKE_BINARY_DIR}/app.hex
    COMMAND ${CMAKE_COMMAND} -E copy ${ELF} ${CMAKE_BINARY_DIR}/demo.elf
    COMMAND ${CMAKE_OBJCOPY} -O binary  ${ELF} ${CMAKE_BINARY_DIR}/demo.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex    ${ELF} ${CMAKE_BINARY_DIR}/demo.hex
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/app.map
            ${CMAKE_BINARY_DIR}/demo.map
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/mkimage.py
            --input ${CMAKE_BINARY_DIR}/demo.bin
            --output ${CMAKE_BINARY_DIR}/demo.img
            --version ${APP_VERSION}
            --commit "${APP_GIT_COMMIT_FULL}"
            --timestamp ${APP_BUILD_TIMESTAMP}
            --name ${APP_FIRMWARE_NAME}
    COMMAND ${SIZE_TOOL} $<TARGET_FILE:app.elf>
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_footprint.py
            --size-tool ${SIZE_TOOL}
            --max-flash ${MAX_FLASH_BYTES}
            --max-ram ${MAX_RAM_BYTES}
            $<TARGET_FILE:app.elf>
    BYPRODUCTS
        ${CMAKE_BINARY_DIR}/app.bin
        ${CMAKE_BINARY_DIR}/app.hex
        ${CMAKE_BINARY_DIR}/demo.elf
        ${CMAKE_BINARY_DIR}/demo.bin
        ${CMAKE_BINARY_DIR}/demo.hex
        ${CMAKE_BINARY_DIR}/demo.map
        ${CMAKE_BINARY_DIR}/demo.img
    COMMENT "Generating app.*, demo.* and demo.img; checking footprint")

# --- HWLINK schema (LNK-012) -------------------------------------------------
# The generated C files are committed so the container does not need a host
# flatcc compiler.  When flatcc is available, regenerate them as part of the
# build; the build-tree copy takes precedence over src/generated.
find_program(FLATCC flatcc)
if(FLATCC)
    set(FLATCC_OUT "${CMAKE_BINARY_DIR}/generated")
    add_custom_command(
        OUTPUT
            "${FLATCC_OUT}/hwlink_reader.h"
            "${FLATCC_OUT}/hwlink_builder.h"
            "${FLATCC_OUT}/hwlink_verifier.h"
            "${FLATCC_OUT}/flatbuffers_common_reader.h"
            "${FLATCC_OUT}/flatbuffers_common_builder.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${FLATCC_OUT}"
        COMMAND ${FLATCC} -a -o "${FLATCC_OUT}" "${CMAKE_SOURCE_DIR}/protocol/hwlink.fbs"
        DEPENDS "${CMAKE_SOURCE_DIR}/protocol/hwlink.fbs"
        COMMENT "Generating HWLINK FlatBuffers C headers")
    add_custom_target(hwlink_schema DEPENDS
        "${FLATCC_OUT}/hwlink_reader.h"
        "${FLATCC_OUT}/hwlink_builder.h"
        "${FLATCC_OUT}/hwlink_verifier.h")
    add_dependencies(app.elf hwlink_schema)
endif()
