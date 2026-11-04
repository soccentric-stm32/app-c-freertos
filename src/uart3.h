/*
 * uart3.h - USART3 on the ST-LINK VCP
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (PD8 TX, PD9 RX)
 * SRS:     COM-001, COM-002, DCON-004
 */

#ifndef UART3_H
#define UART3_H

#include <stdbool.h>
#include <stdint.h>

#define APP_UART_DEFAULT_BAUD   115200U
#define APP_UART_FAST_BAUD      921600U

/* Bring up USART3 at 115200 8N1 with circular DMA receive and interrupt/DMA
 * transmit.  Call after the clocks and before the scheduler. */
bool app_uart3_init(void);

/* Change between the two supported rates.  The caller must have sent the
 * Ack before calling this (COM-001). */
bool app_uart3_set_baud(uint32_t baud);
uint32_t app_uart3_baud(void);

/* Copy received bytes out of the DMA ring; returns the number copied. */
uint32_t app_uart3_rx_read(uint8_t *dst, uint32_t max_len);

/* True while the DMA receiver is armed (HLT-001). */
bool app_uart3_rx_running(void);

/* Re-arm the circular DMA receiver after a receive error or a DMA abort.
 * Safe to call from the Comms task; no-op when the receiver is running. */
bool app_uart3_rx_restart(void);

/* Transmit one frame and wait for the last stop bit.  Safe to call from one
 * task only; it does not block any other task. */
bool app_uart3_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* Called by the UART ISR/DMA callbacks to wake the Comms task. */
void comms_post_rx_from_isr(void);

#endif /* UART3_H */
