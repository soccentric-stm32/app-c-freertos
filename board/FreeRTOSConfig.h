/*
 * board/FreeRTOSConfig.h - project-owned FreeRTOS configuration
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * SRS:     DRT-001..DRT-004; FreeRTOS V10.2.0 as shipped in the image
 *          (same kernel and port as the validation firmware)
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* --- scheduler ----------------------------------------------------------- */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      216000000UL /* DGEN-003 */
#define configTICK_RATE_HZ                      ((TickType_t)1000) /* DRT-001 */
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                ((uint16_t)256)
#define configMAX_TASK_NAME_LEN                 16
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1

/* --- memory (DRT-001, DRT-004) ------------------------------------------- */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   APP_RTOS_HEAP_SIZE
#define configAPPLICATION_ALLOCATED_HEAP        0

/* --- kernel objects ------------------------------------------------------ */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_TASK_NOTIFICATIONS            1   /* DRT-011 */

/* --- timers -------------------------------------------------------------- */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               2
#define configTIMER_QUEUE_LENGTH                8
#define configTIMER_TASK_STACK_DEPTH            256

/* --- hooks (DRT-002) ----------------------------------------------------- */
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0

/* --- stack statistics (DRT-013) ------------------------------------------ */
#define configUSE_TRACE_FACILITY                1
#define configRECORD_STACK_HIGH_ADDRESS         1
#define configSTACK_DEPTH_TYPE                  uint16_t

/* --- interrupt priorities (DRT-003) -------------------------------------- */
/* 4 priority bits on the F767. Kernel and every ISR that uses a FromISR
 * API run at a priority number >= 5 (less urgent). TIM6 time base = 15. */
#define APP_NVIC_PRIO_BITS                      4U
#define configPRIO_BITS                         APP_NVIC_PRIO_BITS
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* --- debug (DRT-002) ----------------------------------------------------- */
extern void app_fault_assert(const char *file, int line);
#define configASSERT(x) if ((x) == 0) { app_fault_assert(__FILE__, __LINE__); }

/* --- port mapping (CM7 GCC port, DRT-001) -------------------------------- */
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/* --- optional API includes ----------------------------------------------- */
#define INCLUDE_vTaskPrioritySet            0
#define INCLUDE_uxTaskPriorityGet           0
#define INCLUDE_vTaskDelete                 0
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_vTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

#endif /* FREERTOS_CONFIG_H */
