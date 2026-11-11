/**
 * @file    task_report.h
 * @brief   Once-a-second "task <n> <name>: <purpose>" line on the debug UART.
 * @details Each of the four application tasks calls @ref task_report once per
 *          loop iteration.  The call returns at once unless a second has
 *          passed since that task last printed, so a task that wakes every
 *          10 ms still prints exactly one line a second.  The line is
 *
 *              [  12034] C-FRT I task 2 Button: debounces B1, short press cycles the LED pattern
 *
 *          so one terminal shows that all four tasks are scheduled.
 */
#ifndef TASK_REPORT_H
#define TASK_REPORT_H

#include <stdint.h>

/** Report state kept by each task; zero-initialise it. */
typedef struct
{
    uint32_t last_ms; /**< Tick of the last printed line. */
    uint8_t  primed;  /**< Non-zero once the first line was printed. */
} task_report_t;

/**
 * @brief Print the task's status line if a second has passed.
 * @param state   The calling task's own @ref task_report_t.
 * @param number  Task number, 1 to 4.
 * @param name    Task name, as given to xTaskCreate().
 * @param purpose What the task is for, one short sentence.
 */
void task_report(task_report_t *state, unsigned number, const char *name, const char *purpose);

#endif /* TASK_REPORT_H */
