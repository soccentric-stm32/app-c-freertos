/**
 * @file    FreeRTOSConfig.h
 * @brief   Project-owned FreeRTOS V10.2.0 kernel configuration.
 * @details The single configuration header for the FreeRTOS V10.2.0 kernel
 *          and its native GCC Cortex-M7 port (r0p1) that ship in the build
 *          image.  @c FreeRTOS.h includes this file before it defines
 *          TickType_t, StackType_t and the kernel typedefs, so the file must
 *          stay free of project headers and of anything that needs those
 *          types at definition time: only @c <stdint.h> is included and every
 *          entry is a plain macro.  The single exception is the heap size,
 *          which the build supplies (see @ref configTOTAL_HEAP_SIZE).
 *
 *          The macros are grouped as listed below: scheduler, memory, kernel
 *          objects, timers, hooks, stack statistics, interrupt priorities,
 *          debug/assert, port handler mapping and the optional API includes.
 *          Each entry records its value, unit or meaning, the SRS clause that
 *          requires it and the consequence of the choice, because this file is
 *          the only place where the RTOS behaviour of the firmware is decided.
 *
 *          Every object in the system - the four application tasks, the idle
 *          and timer service tasks, their queues and the timer command queue -
 *          is created before vTaskStartScheduler() and nothing allocates
 *          afterwards (DRT-004), which is why the dynamic-only, heap_4-only
 *          configuration below is consistent at all.
 * @note    The kernel compiles this header; keep it valid C preprocessor input
 *          and do not add declarations, code or project includes.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6)
 * @srs     DRT-001..DRT-004; FreeRTOS V10.2.0 as shipped in the image
 *          (same kernel and port as the validation firmware)
 * @ingroup freertos_config
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Needed by configTICK_RATE_HZ, which casts to TickType_t before FreeRTOS.h
 * has defined that type; the cast is only evaluated at the call sites. */
#include <stdint.h>

/**
 * @defgroup freertos_config FreeRTOS kernel configuration
 * @brief  Every FreeRTOS V10.2.0 knob the application sets, with units.
 * @details The group holds the whole configuration header.  Values are chosen
 *          to match the SRS FreeRTOS section (DRT-001..DRT-004, DRT-010..
 *          DRT-013) and the task set that @c main() creates: Health, Comms,
 *          Button and LED, plus the idle and timer service tasks.  Changing a
 *          value here changes kernel behaviour for the whole firmware, so the
 *          documentation states what each one controls and what happens when
 *          it is left as is.
 * @{
 */

/* --- scheduler ----------------------------------------------------------- */
/**
 * Preemptive scheduling (1 = enabled).
 * @details A task made ready at a higher priority than the running task
 *          preempts it immediately.  The four application tasks are strictly
 *          priority ordered - Health at @c tskIDLE_PRIORITY + 5, then Comms
 *          +4, Button +3 and LED +2 (DRT-010) - so the IWDG supervision of
 *          DRT-012 depends on preemption rather than on voluntary yields.
 */
#define configUSE_PREEMPTION                    1

/**
 * Use the port's CLZ-based ready-list search (1 = enabled).
 * @details The Cortex-M7 has a single-cycle CLZ instruction, so the port finds
 *          the highest ready priority in one instruction instead of walking a
 *          bitmap.  This method is only available for up to 32 priorities,
 *          which is why configMAX_PRIORITIES is 8; the kernel fails the build
 *          if the limit is exceeded.  The gain is a slightly shorter context
 *          switch, paid for with the CLZ dependency.
 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1

/**
 * Tick suppression while idle (0 = disabled).
 * @details The tick keeps running at 1 kHz in the idle loop, so the tick count,
 *          every pdMS_TO_TICKS() timeout and the 100 ms IWDG refresh cadence of
 *          the Health task are unaffected by CPU idling.  Enabling tickless
 *          idle would require a low-power time base and would stretch every
 *          tick-based timeout; the firmware also stays at 216 MHz (DGEN-003).
 */
#define configUSE_TICKLESS_IDLE                 0

/**
 * Core clock in Hz: 216000000 (216 MHz, DGEN-003).
 * @details The port divides this value by configTICK_RATE_HZ to program the
 *          SysTick reload, so it must equal the clock tree that
 *          app_clock_init() sets up in @ref clock.h (PLL from the 8 MHz HSE
 *          bypass).  A mismatch scales the tick period and with it every
 *          pdMS_TO_TICKS() timeout in the firmware.
 */
#define configCPU_CLOCK_HZ                      216000000UL /* DGEN-003 */

/**
 * Tick frequency in Hz: 1 kHz from SysTick (DRT-001).
 * @details One tick is exactly 1 ms, so pdMS_TO_TICKS(n) == n for integer
 *          millisecond values and the 20 ms health loop, the 500 ms check-ins
 *          and the 200 ms USART3 TX timeout are exact.  The reload value is
 *          216000 - 1.  TickType_t is 32 bits wide (see
 *          configUSE_16_BIT_TICKS), so the tick count wraps after about 49.7
 *          days; the supervision compares unsigned differences, which remain
 *          correct across the wrap (DRT-012).  SysTick belongs to the kernel,
 *          so the HAL time base runs on TIM6 instead (DGEN-006).
 */
#define configTICK_RATE_HZ                      ((TickType_t)1000) /* DRT-001 */

/**
 * Number of priority levels: 8, numbered 0 (idle) to 7 (highest).
 * @details The task set uses 0 for idle, 2 for the LED task and the timer
 *          service task, 3 for Button, 4 for Comms and 5 for Health
 *          (DRT-010), leaving 6 and 7 unused.  Each level costs one ready list
 *          in RAM, and the port-optimised selection above caps the value at
 *          32.  A task created with a higher priority trips configASSERT and
 *          is then clamped to 7.
 */
#define configMAX_PRIORITIES                    8

/**
 * Idle task stack depth: 256 StackType_t words, that is 1 KiB.
 * @details StackType_t is 32 bits wide on this port, so this and every other
 *          depth in the project is a word count, not a byte count.  The idle
 *          task only runs the idle loop, the (unused) idle hook and the
 *          deletion cleanup, which is not compiled because
 *          INCLUDE_vTaskDelete is 0, so 1 KiB is generous.  The value is a
 *          convenient lower bound for the application stacks (all four are
 *          384 words or more).
 */
#define configMINIMAL_STACK_SIZE                ((uint16_t)256)

/**
 * Task name capacity in bytes including the NUL terminator: 15 characters.
 * @details "Health", "Comms", "Button", "LED", the idle task and the timer
 *          service task all fit.  The name lives in the TCB and is what Status
 *          reports per task (DRT-013); APP_FAULT_TASK_LEN in @ref app_fault.h
 *          is deliberately the same width, so a task name copied into the
 *          no-init fault record is never truncated (DGEN-004).
 */
#define configMAX_TASK_NAME_LEN                 16

/**
 * Width of TickType_t: 0 selects 32 bits, 1 would select 16 bits.
 * @details 32-bit ticks give the 49.7-day wrap at 1 kHz and let the Health
 *          task compare "now - last_seen" against 2000 ms without ambiguity.
 *          16-bit ticks would wrap every 65.5 s and silently break that
 *          arithmetic, so this must stay 0 for DRT-012.
 */
#define configUSE_16_BIT_TICKS                  0

/**
 * Let the idle task yield to other priority-0 tasks (1 = enabled).
 * @details Every application task runs above priority 0, so nothing shares the
 *          idle priority and the switch currently has no observable effect on
 *          the task set.  The standard FreeRTOS behaviour is kept so that
 *          adding a priority-0 task later does not change scheduling again.
 */
#define configIDLE_SHOULD_YIELD                 1

/**
 * Round-robin time slicing between equal priorities (1 = enabled).
 * @details The LED task and the FreeRTOS timer service task both run at
 *          priority 2 (see configTIMER_TASK_PRIORITY), so slicing guarantees
 *          that a busy LED pattern cannot starve the timer task.  The cost is
 *          at most one extra context switch per tick per equal-priority group;
 *          tasks that block on a queue or notification are unaffected.
 */
#define configUSE_TIME_SLICING                  1

/* --- memory (DRT-001, DRT-004) ------------------------------------------- */
/**
 * Static task/queue creation APIs (0 = not compiled).
 * @details vTaskCreateStatic(), xQueueCreateStatic() and friends are not part
 *          of the build and no application-supplied buffers are expected.
 *          Every object therefore comes from heap_4 (DRT-001), which is why
 *          DRT-004 requires all of them to exist before the scheduler starts.
 * @warning There is no way to create an object in this build without touching
 *          the heap; a late creation would allocate after start-up, which
 *          DRT-004 forbids.
 */
#define configSUPPORT_STATIC_ALLOCATION         0

/**
 * Dynamic (heap) task/queue creation APIs (1 = compiled).
 * @details Must stay 1 while static allocation is 0, otherwise neither
 *          creation path would exist.  @c main() creates the LED, Button,
 *          Comms and Health tasks and their queues this way, all before
 *          vTaskStartScheduler() (DRT-004); the kernel then creates the idle
 *          and timer service tasks and the timer command queue from the same
 *          heap.
 */
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/**
 * Size of the heap_4 arena in bytes.
 * @details The number is not written in this header: CMake defines
 *          APP_RTOS_HEAP_SIZE from the cache variable APP_HEAP_KB (24 KB by
 *          default) in CMakeLists.txt, so the heap can be resized without
 *          touching the kernel configuration.  heap_4 is linked from the
 *          FreeRTOS source list (DRT-001) and serves the six task stacks and
 *          TCBs (DRT-010), the LED queue (8 messages), the Comms event queue
 *          (16 messages) and the timer command queue; the health check
 *          additionally requires at least 4096 bytes to remain free at the
 *          5 s mark (DRT-012).  heap_4 coalesces adjacent free blocks, but a
 *          long-lived mix of allocations of different sizes can still
 *          fragment the arena.
 * @warning This header does not define APP_RTOS_HEAP_SIZE.  Building any
 *          target that includes it without the definition fails in heap_4.c
 *          with an unrelated-looking error.
 * @see configAPPLICATION_ALLOCATED_HEAP
 */
#define configTOTAL_HEAP_SIZE                   APP_RTOS_HEAP_SIZE

/**
 * Who provides the heap array (0 = heap_4.c provides it).
 * @details With 0, heap_4.c defines ucHeap as a static array of
 *          configTOTAL_HEAP_SIZE bytes in @c .bss.  The C start-up code zeroes
 *          it and no linker-script section is needed, so the whole 24 KB shows
 *          up in the static RAM footprint that the post-build check in
 *          CMakeLists.txt measures.  Setting it to 1 would require the
 *          application to define ucHeap, for example to place the heap in a
 *          particular memory region.
 */
#define configAPPLICATION_ALLOCATED_HEAP        0

/* --- kernel objects ------------------------------------------------------ */
/**
 * Mutexes with priority inheritance (1 = enabled).
 * @details No module creates a mutex at present: cross-task data is exchanged
 *          through the Comms and LED queues and through task notifications
 *          (DRT-011) and no shared peripheral is locked.  The switch is kept
 *          on so a mutex can be added without rebuilding the kernel
 *          configuration; it costs two words per TCB plus the mutex code.
 */
#define configUSE_MUTEXES                       1

/**
 * Recursive mutexes (0 = not compiled).
 * @details No task takes a mutex twice, so xSemaphoreCreateRecursiveMutex()
 *          and the recursive give/take paths are left out.  Enabling it also
 *          requires configUSE_MUTEXES, which is on.
 */
#define configUSE_RECURSIVE_MUTEXES             0

/**
 * Counting semaphores (0 = not compiled).
 * @details Events are signalled with queues and task notifications only
 *          (DRT-011), so xSemaphoreCreateCounting() is not available and the
 *          counting code is absent from the image.
 */
#define configUSE_COUNTING_SEMAPHORES           0

/**
 * Queue sets (0 = not compiled).
 * @details No task waits on more than one queue: the Comms task owns its event
 *          queue and the LED task its message queue.  Leaving the feature out
 *          removes the set/read/remove code and the extra list members.
 */
#define configUSE_QUEUE_SETS                    0

/**
 * Queue registry entries: 0, so no queue names are stored.
 * @details The registry exists only to let a debugger list and name queues.
 *          No module registers a queue, so nothing is lost, and the table plus
 *          its name pointers are saved.  Status reports tasks, not queues
 *          (DRT-013), so no requirement depends on it.
 */
#define configQUEUE_REGISTRY_SIZE               0

/**
 * Task notifications (1 = enabled, DRT-011).
 * @details Every TCB gains a notification value and a notification state.
 *          The Health task collects the per-task check-in bits with
 *          xTaskNotifyWait() and each task posts one through health_checkin()
 *          (DRT-012); the Comms task is woken for USART3 TX completion with
 *          ulTaskNotifyTake() (DBT-005 path).  Notifications replace the
 *          semaphores a conventional design would need and add about 8 bytes
 *          to each TCB.
 */
#define configUSE_TASK_NOTIFICATIONS            1   /* DRT-011 */

/* --- timers -------------------------------------------------------------- */
/**
 * Software timers and the timer service task (1 = enabled).
 * @details timers.c is linked, and vTaskStartScheduler() creates the "Tmr Svc"
 *          task together with its command queue from the heap.  DRT-010 counts
 *          that task among the six in the system, alongside idle.
 *          The application creates no software timer: timeouts are tick based
 *          (xTaskNotifyWait() with pdMS_TO_TICKS()), so the feature is enabled
 *          for the standard task set and costs one TCB, one 1 KiB stack and
 *          the queue out of configTOTAL_HEAP_SIZE.
 */
#define configUSE_TIMERS                        1

/**
 * Priority of the timer service task: 2.
 * @details Equal to the LED task priority (@c tskIDLE_PRIORITY + 2), so
 *          configUSE_TIME_SLICING makes those two share the CPU; it is below
 *          Button (3), Comms (4) and Health (5) and above the idle task.  A
 *          software timer callback would execute at this priority, so it must
 *          stay short if one is ever added.
 */
#define configTIMER_TASK_PRIORITY               2

/**
 * Timer command queue length in messages: 8.
 * @details Allocated from the heap when the timer service task starts.  The
 *          queue only carries timer commands (start, stop, reset, change
 *          period) and no module issues any, so it cannot overflow in this
 *          build; a full queue makes a timer API call fail with pdFAIL
 *          instead of blocking.
 */
#define configTIMER_QUEUE_LENGTH                8

/**
 * Timer service task stack depth: 256 words, that is 1 KiB.
 * @details The same value as configMINIMAL_STACK_SIZE.  The task runs no
 *          application callback at present, so 1 KiB is ample; the stack is
 *          charged to configTOTAL_HEAP_SIZE like every other one.
 */
#define configTIMER_TASK_STACK_DEPTH            256

/* --- hooks (DRT-002) ----------------------------------------------------- */
/**
 * Call vApplicationMallocFailedHook() when pvPortMalloc() fails (DRT-002).
 * @details The hook is implemented in @c rtos_hooks.c.  It records
 *          APP_FAULT_MALLOC_FAILED with the current task name and stores the
 *          free heap size in the @c line field of the no-init record, then
 *          resets through app_fault_fatal(), so the next boot reports the
 *          failure in Status (DGEN-004).  The hook must not return, and this
 *          one does not; its purpose is to make a heap shortfall visible
 *          instead of leaving a NULL handle behind.
 */
#define configUSE_MALLOC_FAILED_HOOK            1

/**
 * Stack overflow detection method 2 (DRT-002).
 * @details Method 1 only compares the stack pointer with the stack limit at a
 *          context switch.  Method 2 additionally checks the 20 bytes at the
 *          bottom of the stack for the 0xa5a5a5a5 fill pattern, so an overflow
 *          that has already written over the limit is still detected.  On
 *          detection the kernel calls vApplicationStackOverflowHook() in
 *          @c rtos_hooks.c, which records APP_FAULT_STACK_OVERFLOW with the
 *          offending task name and resets (DGEN-004).  The check runs in the
 *          context-switch path and costs a few instructions per switch.
 */
#define configCHECK_FOR_STACK_OVERFLOW          2

/**
 * Application idle hook (0 = no vApplicationIdleHook()).
 * @details The idle task has no application work: low-power entry is not used
 *          because tickless idle is off and the core stays at 216 MHz
 *          (DGEN-003), and the IWDG is refreshed by the Health task, not from
 *          idle (DRT-012).  The hook is therefore not compiled.
 */
#define configUSE_IDLE_HOOK                     0

/**
 * Application tick hook (0 = no vApplicationTickHook()).
 * @details A tick hook runs in the SysTick interrupt, must not block and adds
 *          to every 1 ms interrupt.  The periodic work of this firmware is done
 *          by tasks (DRT-012), and the HAL time base is on TIM6 (DGEN-006), so
 *          the hook is not compiled.
 */
#define configUSE_TICK_HOOK                     0

/* --- stack statistics (DRT-013) ------------------------------------------ */
/**
 * Task status and trace helpers (1 = enabled, DRT-013).
 * @details Enables TaskStatus_t, uxTaskGetSystemState() and vTaskGetInfo().
 *          The Comms task uses uxTaskGetSystemState() to fill the per-task
 *          part of the Status reply: task name, state, current priority and
 *          stack high-water mark.  The switch adds two bookkeeping words per
 *          TCB (TCB and task numbers) and the code that fills the structure,
 *          and it also compiles prvTaskCheckFreeStackSpace().
 */
#define configUSE_TRACE_FACILITY                1

/**
 * Record each stack's high address in the TCB (1 = enabled).
 * @details Adds one pxEndOfStack word per TCB holding the highest valid stack
 *          address.  A debugger uses it to draw the full extent of a task
 *          stack, and the kernel needs it for the top/end-of-stack fields of
 *          TaskStatus_t on ports whose stacks grow upwards.  This port grows
 *          downwards, so the high-water mark of DRT-013 is still measured from
 *          pxStack by scanning for the fill pattern; nothing depends on the
 *          extra field at run time.
 */
#define configRECORD_STACK_HIGH_ADDRESS         1

/**
 * Type of stack depths and of the reported high-water mark: uint16_t.
 * @details Halves the width of the stack-depth parameter of xTaskCreate()
 *          compared with the 32-bit default and makes
 *          TaskStatus_t.usStackHighWaterMark 16 bits.  A depth of at most
 *          65535 words (256 KiB) can be requested, far above the 1536-word
 *          Comms stack, so the Status high-water marks of DRT-013 are exact
 *          word counts.  A larger depth would be truncated silently.
 */
#define configSTACK_DEPTH_TYPE                  uint16_t

/* --- interrupt priorities (DRT-003) -------------------------------------- */
/* 4 priority bits on the F767. Kernel and every ISR that uses a FromISR
 * API run at a priority number >= 5 (less urgent). TIM6 time base = 15. */
/**
 * Implemented NVIC priority bits on the STM32F767: 4.
 * @details The F767 implements the high nibble of each 8-bit priority field,
 *          matching the CMSIS @c __NVIC_PRIO_BITS value in stm32f7xx.h.
 *          Priority numbers therefore range from 0 (most urgent) to 15 (least
 *          urgent), and each step is a shift of 16 in the register value.  This
 *          is the single source for the count: configPRIO_BITS and both
 *          shifted values below are derived from it, so it must stay in step
 *          with the device header.
 */
#define APP_NVIC_PRIO_BITS                      4U

/**
 * Number of priority bits known to the FreeRTOS port.
 * @details The Cortex-M port needs the count to interpret the 8-bit priority
 *          fields: it derives the priority-grouping value it tolerates and
 *          validates, in vPortValidateInterruptPriority(), that an ISR calling
 *          a FromISR() API is not more urgent than the syscall threshold
 *          (DRT-003).  The port would default the count to 4 if it were
 *          undefined; defining it from APP_NVIC_PRIO_BITS keeps one source for
 *          the number, which must match the 4 bits the STM32F767 implements.
 */
#define configPRIO_BITS                         APP_NVIC_PRIO_BITS

/**
 * Lowest interrupt urgency as an unshifted library priority number: 15.
 * @details 15 is the least urgent of the 16 levels, and the value the HAL uses
 *          for its TIM6 time base (TICK_INT_PRIORITY in
 *          board/stm32f7xx_hal_conf.h).  Shifting it produces
 *          configKERNEL_INTERRUPT_PRIORITY, which puts PendSV and SysTick at
 *          the bottom of the priority order so they never delay a peripheral
 *          interrupt.
 */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15

/**
 * Highest urgency that may call a FromISR() API: library priority number 5.
 * @details Numbers 0..4 are more urgent than this threshold and must never call
 *          a FreeRTOS API, because the BASEPRI mask set by
 *          taskENTER_CRITICAL() cannot hold them off.  Every kernel-aware
 *          interrupt uses 6 or 15: USART3, DMA1 stream 1, DMA1 stream 3 and
 *          EXTI15_10 are at 6, and the USART6 debug log is at 15 (DRT-003).
 * @warning An ISR placed above this threshold still compiles and still runs,
 *          but it would corrupt kernel critical sections;
 *          vPortValidateInterruptPriority() traps it the first time it calls a
 *          FromISR() API.
 */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5

/**
 * Shifted priority written to the PendSV and SysTick fields of SHPR3:
 * 15 << (8 - 4) = 0xF0.
 * @details The Cortex-M7 port writes this value into @c SCB->SHPR3 when the
 *          scheduler starts, which places the context-switch exceptions at the
 *          lowest urgency.  The shift converts the library number into the bit
 *          position of the implemented priority bits in the register.
 */
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/**
 * Shifted syscall threshold: 5 << (8 - 4) = 0x50.
 * @details taskENTER_CRITICAL(), taskDISABLE_INTERRUPTS() and the interrupt
 *          mask helpers behind the FromISR() API write this value to BASEPRI,
 *          which masks every interrupt whose priority number is 5 or greater
 *          and lets the more urgent ones keep running.  DRT-003 holds only
 *          while every kernel-aware ISR keeps a priority number of 5 or more.
 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* --- debug (DRT-002) ----------------------------------------------------- */
/**
 * Assertion sink used by configASSERT, implemented in @c rtos_hooks.c.
 * @details Declared here rather than through @ref app_fault.h so that the
 *          kernel's translation units pull in no project headers.  The
 *          implementation calls app_fault_fatal(APP_FAULT_ASSERT, ...) with the
 *          running task name, the file basename and the line number, and
 *          resets; it never returns, although this prototype cannot express
 *          that (the noreturn attribute is not visible to the kernel).
 * @param[in] file Source file name, from @c __FILE__.
 * @param[in] line Source line number, from @c __LINE__.
 * @see configASSERT
 * @see app_fault_fatal
 */
extern void app_fault_assert(const char *file, int line);

/**
 * FreeRTOS assertion macro (DRT-002).
 * @details Evaluates @p x once; when it is zero, records a fault through
 *          app_fault_assert() and resets down the no-init fault path, so the
 *          next boot reports the file and line in Status (DGEN-004).  The
 *          kernel uses the macro for its own invariants (queue ownership,
 *          priority bounds, interrupt priority validation), so a failure is
 *          always a real programming error.  The @c do/@c while wrapper keeps
 *          it a single statement, safe in an unbraced @c if or @c else.
 * @note CMake defines NDEBUG=1 for the target, but configASSERT comes from the
 *       kernel, not from @c assert.h, so every check stays in the release
 *       build; the cost is flash only.
 * @warning A failed assertion does not halt in a debugger: it shows the LD3
 *          fault pattern for about 2 s and resets the MCU.
 * @see app_fault_show_and_reset
 */
#define configASSERT(x) do { if ((x) == 0) { app_fault_assert(__FILE__, __LINE__); } } while (0)

/* --- port mapping (CM7 GCC port, DRT-001) -------------------------------- */
/**
 * Route the kernel's SVC handler onto the CMSIS vector name.
 * @details The CM7 port implements vPortSVCHandler() to start the first task;
 *          the rename makes the vector table entry in startup_stm32f767xx.s,
 *          @c SVC_Handler, call it.  The application must not define
 *          @c SVC_Handler itself, or this mapping is lost and the scheduler
 *          never starts.
 */
#define vPortSVCHandler    SVC_Handler

/**
 * Route the kernel's PendSV handler onto the CMSIS vector name.
 * @details PendSV carries out the actual context switch.  The kernel pends it
 *          from the tick and from every API call that makes a higher-priority
 *          task ready, and the vector entry is @c PendSV_Handler in the
 *          startup file, so the port's xPortPendSVHandler() must be mapped
 *          here for any task switch to happen.
 */
#define xPortPendSVHandler PendSV_Handler

/**
 * Route the kernel's SysTick handler onto the CMSIS vector name.
 * @details SysTick is the kernel tick source (DRT-001): it increments
 *          xTickCount, pends PendSV and drives the pdMS_TO_TICKS() timeouts.
 *          The HAL must therefore not use SysTick for its own tick, which is
 *          why the time base runs on TIM6 (DGEN-006) and TICK_INT_PRIORITY in
 *          board/stm32f7xx_hal_conf.h is 15.
 */
#define xPortSysTickHandler SysTick_Handler

/* --- optional API includes ----------------------------------------------- */
/* FreeRTOS.h defaults every INCLUDE_* that is not listed to 0, so the entries
 * below record the decision explicitly for both readers and the build. */
/**
 * vTaskPrioritySet() (0 = not compiled).
 * @details No task changes another task's priority; the order documented in
 *          DRT-010 is fixed at creation time.  Leaving it out removes the
 *          priority-change path and its ready-list re-insertion.
 */
#define INCLUDE_vTaskPrioritySet            0

/**
 * uxTaskPriorityGet() (0 = not compiled).
 * @details Status reports priorities through TaskStatus_t.uxCurrentPriority,
 *          which configUSE_TRACE_FACILITY already provides, so no separate
 *          query function is needed.
 */
#define INCLUDE_uxTaskPriorityGet           0

/**
 * vTaskDelete() (0 = not compiled).
 * @details All six tasks run forever, so the deletion path, the idle-task
 *          cleanup of deleted TCBs and the accompanying free logic are left
 *          out.  A failed health check resets the MCU instead of deleting
 *          tasks (DRT-012).
 */
#define INCLUDE_vTaskDelete                 0

/**
 * vTaskSuspend() and vTaskResume() (1 = compiled).
 * @details No module suspends a task today.  The switch is enabled because it
 *          is the documented prerequisite for an indefinite block time
 *          (portMAX_DELAY) in queue, semaphore and notification calls, and
 *          because it lets uxTaskGetSystemState() report the suspended state
 *          in the Status task table (DRT-013).
 */
#define INCLUDE_vTaskSuspend                1

/**
 * vTaskDelayUntil() (1 = compiled).
 * @details No source file calls it at present: the tasks block on
 *          notifications and queues with a timeout instead of waiting for a
 *          fixed period (DRT-011).  The switch is enabled but unused, and it
 *          costs a small amount of flash.
 */
#define INCLUDE_vTaskDelayUntil             1

/**
 * vTaskDelay() (1 = compiled).
 * @details No source file calls it at present; the tasks wait on
 *          notifications, queues and the USART3 completion handle instead of
 *          sleeping for a fixed time.  The switch costs a few bytes of flash.
 */
#define INCLUDE_vTaskDelay                  1

/**
 * xTaskGetSchedulerState() (1 = compiled).
 * @details Required by the hooks in @c rtos_hooks.c.  A failed task creation
 *          reaches the assert or malloc-failed hook before the scheduler runs,
 *          and current_task_name() only calls pcTaskGetName() when this
 *          function reports taskSCHEDULER_RUNNING (DRT-002).
 */
#define INCLUDE_xTaskGetSchedulerState      1

/**
 * xTaskGetCurrentTaskHandle() (1 = compiled).
 * @details Used by the USART3 driver in @ref uart3.c to remember which task is
 *          waiting for DMA transmit completion, so the interrupt callback wakes
 *          exactly that task through its notification value (DRT-011).
 */
#define INCLUDE_xTaskGetCurrentTaskHandle   1

/**
 * uxTaskGetStackHighWaterMark() (1 = compiled).
 * @details Exposes a task's high-water mark as a standalone call.  Status
 *          obtains the same number through TaskStatus_t.usStackHighWaterMark,
 *          which configUSE_TRACE_FACILITY already provides, so no source file
 *          calls this function today; the switch is kept for ad-hoc debugging
 *          and costs only the small wrapper, because the measuring helper is
 *          compiled for configCHECK_FOR_STACK_OVERFLOW anyway.  The result is
 *          a count of StackType_t words, found by scanning for the fill
 *          pattern the kernel writes when the stack is created (DRT-013).
 */
#define INCLUDE_uxTaskGetStackHighWaterMark 1

/** @} */

#endif /* FREERTOS_CONFIG_H */
