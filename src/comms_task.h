/*
 * comms_task.h - HWLINK Comms task
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     COM-001..COM-007, BTN-004, section 8
 */

#ifndef COMMS_TASK_H
#define COMMS_TASK_H

#include <stdbool.h>
#include <stdint.h>

/* Create the Comms task and its event queue before the scheduler starts. */
bool comms_task_create(void);

/* UART RX DMA / error callback from interrupt context. */
void comms_post_rx_from_isr(void);

/* LED task reports a short (1) or long (2) press and the resulting pattern. */
void comms_post_button_event(uint8_t press, uint8_t pattern, uint16_t period);

#endif /* COMMS_TASK_H */
