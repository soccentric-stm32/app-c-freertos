/*
 * app_log.h - debug text output on USART6 (PG14, Arduino D1)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-06
 * Target:  STM32F767ZIT6
 * SRS:     DLOG-001..DLOG-004
 *
 * USART3 carries HWLINK only, so debug lines go to USART6, 115200 8N1, TX
 * only.  BOOT and every application use the same port and line format, so
 * one terminal shows the whole boot chain:
 *
 *     [   5012] C-FRT I health: slot A confirmed
 *
 * app_log() never blocks: it formats into a stack buffer, copies the line
 * into a ring buffer and returns; the USART6 TXE interrupt drains it.  A
 * full ring drops the line and counts it.  Safe from tasks, interrupts and
 * before the scheduler starts.
 */

#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdint.h>

/* Configure PG14 as USART6_TX and start the interrupt-driven output. */
void app_log_init(void);

/* One line: level is 'E', 'W', 'I' or 'D'; printf-style message without a
 * trailing newline. */
void app_log(char level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Wait until every queued character has left the wire, at most timeout_ms.
 * Call before a deliberate reset so the last lines are not lost. */
void app_log_flush(uint32_t timeout_ms);

/* Lines dropped because the ring was full (DLOG-003). */
uint32_t app_log_dropped(void);

#define LOG_E(...) app_log('E', __VA_ARGS__)
#define LOG_W(...) app_log('W', __VA_ARGS__)
#define LOG_I(...) app_log('I', __VA_ARGS__)
#define LOG_D(...) app_log('D', __VA_ARGS__)

#endif /* APP_LOG_H */
