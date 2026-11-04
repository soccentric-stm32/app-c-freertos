/*
 * app_fault.h - no-init fault record and fatal paths
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DGEN-004, DRT-002, LED-005
 */

#ifndef APP_FAULT_H
#define APP_FAULT_H

#include <stdbool.h>
#include <stdint.h>

#define APP_FAULT_MAGIC          0x4641554CUL  /* "FAUL" */
#define APP_FAULT_TASK_LEN       16U
#define APP_FAULT_FILE_LEN       32U

typedef enum {
    APP_FAULT_NONE = 0,
    APP_FAULT_HARDFAULT,
    APP_FAULT_MEMMANAGE,
    APP_FAULT_BUSFAULT,
    APP_FAULT_USAGEFAULT,
    APP_FAULT_ASSERT,
    APP_FAULT_STACK_OVERFLOW,
    APP_FAULT_MALLOC_FAILED,
    APP_FAULT_STARTUP
} app_fault_reason_t;

typedef struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint32_t pc;
    uint32_t lr;
    uint32_t psr;
    uint32_t line;
    uint32_t boot_id;
    char task[APP_FAULT_TASK_LEN];
    char file[APP_FAULT_FILE_LEN];
} app_fault_record_t;

/* Lives in the .noinit linker section; the C start-up code does not touch it,
 * so it survives a system reset (DGEN-004). */
extern app_fault_record_t g_app_fault;

/* Increment the no-init boot id at the first instruction of main. */
void app_fault_boot_start(void);

bool app_fault_is_valid(void);
bool app_fault_is_current_boot(void);
bool app_fault_is_previous_boot(void);
const char *app_fault_reason_text(app_fault_reason_t reason);
const char *app_fault_task(void);
const char *app_fault_file(void);

/* Save a reason from a normal or fault context.  Safe with interrupts off. */
void app_fault_save(app_fault_reason_t reason, uint32_t cfsr, uint32_t hfsr,
                    uint32_t mmfar, uint32_t bfar, uint32_t pc, uint32_t lr,
                    uint32_t psr, const char *task, const char *file,
                    uint32_t line);

/* Show the fault pattern on LD3 for about two seconds and reset. */
void app_fault_show_and_reset(void) __attribute__((noreturn));

/* Record a fatal software error and reset. */
void app_fault_fatal(app_fault_reason_t reason, const char *task,
                     const char *file, uint32_t line) __attribute__((noreturn));

void app_fault_clear(void);

#endif /* APP_FAULT_H */
