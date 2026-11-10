/**
 * @file    uart3.h
 * @brief   USART3 on the ST-LINK VCP: DMA ring reception and DMA transmit.
 * @details USART3 is the HWLINK transport (DCON-004).  PD8 is TX and PD9 is
 *          RX, both on AF7, wired to the on-board ST-LINK virtual COM port.
 *          The peripheral runs 8N1, no parity, no hardware flow control and
 *          16x oversampling at @ref APP_UART_DEFAULT_BAUD, with
 *          @ref APP_UART_FAST_BAUD as the only alternative rate (COM-001).
 *
 *          This header exposes the transport only: no framing, no COBS and no
 *          FlatBuffers.  The HWLINK parser lives in @ref comms_task.h and is
 *          the sole consumer of @ref app_uart3_rx_read.
 *
 *          **Reception.**  A 4 KB circular DMA buffer in the non-cacheable
 *          @c .dma_buffer window of SRAM2 (see @ref cache_mpu.h) is filled by
 *          DMA1 Stream1, channel 4, byte-wise, while the CPU derives the write
 *          position from the DMA remaining-count register.  The IDLE-line
 *          interrupt, the RX complete/half-complete callbacks and the error
 *          interrupt only post an "RX may be ready" hint to the Comms task.
 *
 *          **Transmission.**  DMA1 Stream3, channel 4, is armed per frame in
 *          normal (non-circular) mode and completion is signalled to the
 *          sending task through a task notification.  Only the Comms task
 *          transmits and only the calling task waits, so a slow host link
 *          cannot stall the LED, button or health tasks (COM-002).
 *
 *          **Framing and errors.**  Overrun, framing, noise and parity errors
 *          are cleared by the USART3 ISR through @c USART3->ICR only; the HAL
 *          receive interrupt handler is never used for them because it aborts
 *          the circular receiver on an overrun.  The consumer instead notices
 *          a stopped receiver and re-arms it with
 *          @ref app_uart3_rx_restart, so a receive error never needs a board
 *          reset (COM-002, HLT-001).
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (PD8 TX, PD9 RX)
 * @srs     COM-001, COM-002, DCON-004
 * @ingroup uart3
 */

#ifndef UART3_H
#define UART3_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup uart3 USART3 ST-LINK VCP transport
 * @brief DMA ring receiver and DMA transmitter for the HWLINK serial link.
 * @details The group holds the whole public surface of the driver: one
 *          bring-up call, the rate switch, the ring reader, two receiver
 *          health probes, the blocking transmit and the ISR-to-task wake-up
 *          used by the driver callbacks.
 *
 *          @ref app_uart3_init runs once before the scheduler; everything else
 *          is used by the Comms task while the application runs.  Except for
 *          @ref comms_post_rx_from_isr, none of these functions may be called
 *          from an interrupt, and none of them may be called concurrently from
 *          two tasks.
 * @see comms_task.h
 * @{
 */

/**
 * @brief Default link rate: 115200 baud, 8N1 (COM-001).
 * @details Programmed by @ref app_uart3_init and restored by
 *          @ref app_uart3_set_baud when the host asks for the default rate or
 *          when the Comms task reverts after a silent fast-rate session.
 */
#define APP_UART_DEFAULT_BAUD 115200U

/**
 * @brief Fast link rate: 921600 baud, 8N1 (COM-001).
 * @details The only rate @ref app_uart3_set_baud accepts besides
 *          @ref APP_UART_DEFAULT_BAUD.  The host selects it with @c SetBaud
 *          and keeps it while valid frames arrive; the 5 s revert timer lives
 *          in @ref comms_task.c, not in this driver.
 */
#define APP_UART_FAST_BAUD 921600U

/**
 * @brief Bring up USART3 and arm the circular DMA receiver.
 * @details Configures USART3 for 8 data bits, 1 stop bit, no parity, no
 *          hardware flow control, 16x oversampling and both directions, then
 *          starts DMA reception into the 4 KB ring.  @c HAL_UART_Init calls
 *          the file-local MSP hook in @ref uart3.c, which enables the USART3
 *          clock, configures PD8/PD9 as AF7, initialises DMA1 Stream1
 *          (receive, circular) and Stream3 (transmit, normal), sets the three
 *          NVIC lines to priority 6 and enables the USART3 error and IDLE
 *          interrupts.
 *
 *          The ring read, consumed and wrap counters are cleared, so the first
 *          call starts with an empty ring and a receiver aligned with it.
 *
 * @return true when the peripheral is initialised and reception is armed.
 * @retval true  @c HAL_UART_Init and @c HAL_UART_Receive_DMA both succeeded;
 *               @ref app_uart3_rx_running reports true and
 *               @ref app_uart3_baud returns @ref APP_UART_DEFAULT_BAUD.
 * @retval false @c HAL_UART_Init failed, or the receive DMA could not be
 *               started; the receiver is not armed and the link is unusable.
 *
 * @pre The clocks, the cache/MPU setup (@ref cache_mpu.h) and the HAL tick are
 *      already running, and the ring is still mapped non-cacheable.
 * @pre Called once from @c main before @c vTaskStartScheduler, where a failure
 *      is fatal (@c startup_fail("uart3 init")).
 * @post On success received bytes accumulate in the ring until the Comms task
 *       reads them, and reception continues without further calls.
 * @post On failure nothing usable is left armed; there is no run-time retry.
 *
 * @note The driver guarantees only that bytes are not lost while the consumer
 *       is busy replying; framing and validation belong to @ref comms_task.h.
 * @warning Not reentrant and not thread-safe.  Calling it again while the
 *          scheduler is running re-initialises the peripheral underneath a
 *          live reception.
 * @see app_uart3_rx_running
 * @see cache_mpu.h
 */
bool app_uart3_init(void);

/**
 * @brief Switch the link between the two supported rates.
 * @details Stops the DMA, de-initialises USART3, re-initialises it at
 *          @p baud and re-arms the receiver, which resets the ring counters.
 *          Any bytes still in the ring are therefore discarded, which is why
 *          the caller must have put the @c SetBaud @c Ack on the wire at the
 *          old rate first and must reset its own frame parser afterwards
 *          (COM-001).
 *
 *          A failure is rolled back: the peripheral is de-initialised again,
 *          brought back up at the previous rate and the receiver restarted, so
 *          a rejected switch normally leaves the link working at the old rate.
 *
 * @param[in] baud Requested rate: @ref APP_UART_DEFAULT_BAUD or
 *                 @ref APP_UART_FAST_BAUD.
 *
 * @return true when the link runs at @p baud afterwards.
 * @retval true  @p baud was already active (no hardware access at all), or the
 *               switch completed and the receiver was re-armed.
 * @retval false @p baud is not one of the two supported rates, or
 *               re-initialisation or re-arming failed; the old rate is kept.
 *
 * @pre The scheduler is running and the caller is the only user of USART3,
 *      that is the Comms task, with no transmit in flight.
 * @post On success @ref app_uart3_baud returns @p baud and the ring is empty.
 * @post On failure the previous rate is restored and the ring is empty.
 *
 * @note The revert to @ref APP_UART_DEFAULT_BAUD after 5 s without a valid
 *       frame is implemented in @ref comms_task.c; this function only performs
 *       the switch it is asked for.
 * @note The rollback is best effort: if the second @c HAL_UART_Init also
 *       fails, the peripheral is left de-initialised while
 *       @ref app_uart3_baud still reports the old rate.
 * @warning Not reentrant and not ISR-safe.  It aborts everything the
 *          peripheral is doing, so calling it while @ref app_uart3_send waits
 *          in another task loses that frame and its completion.
 * @see app_uart3_baud
 */
bool app_uart3_set_baud(uint32_t baud);

/**
 * @brief Report the link rate the driver has configured.
 * @details Returns the cached rate maintained by @ref app_uart3_init and
 *          @ref app_uart3_set_baud.  The USART3 register is not read, so the
 *          value is available even before the peripheral is initialised and
 *          reflects intent rather than the bit timing currently on the wire.
 *
 * @return Configured rate in baud; @ref APP_UART_DEFAULT_BAUD until the first
 *         switch away from it succeeds.
 *
 * @note Read-only and free of side effects, but the cache is a plain variable:
 *       a concurrent @ref app_uart3_set_baud can make the result stale before
 *       the caller uses it.
 * @see app_uart3_set_baud
 */
uint32_t app_uart3_baud(void);

/**
 * @brief Copy received bytes out of the DMA ring.
 * @details The 4 KB ring is written by the DMA while the CPU reads it.  The
 *          write position is derived from the DMA remaining-count register and
 *          the number of full-ring completions, so no byte is lost while the
 *          Comms task is busy building a reply.  At most @p max_len bytes are
 *          copied, oldest first, in a plain byte loop; bytes are not erased
 *          from the ring, they are simply overwritten by later traffic.
 *
 *          If the DMA lapped the reader, earlier unread bytes have already
 *          been overwritten.  The function then resynchronises at the current
 *          write position, drops the backlog and returns 0 rather than
 *          returning out-of-order data.
 *
 * @param[out] dst     Destination buffer for the copied bytes.
 * @param[in]  max_len Capacity of @p dst in bytes.
 *
 * @return Number of bytes copied into @p dst.
 * @retval 0 No unread bytes were waiting, @p max_len is 0, or the reader was
 *           lapped and the ring was resynchronised.
 *
 * @pre @p dst points to at least @p max_len writable bytes.
 * @pre Called from the single consumer task (Comms); the ring indices are
 *      shared with @ref app_uart3_rx_restart.
 * @post On return the consumed count has advanced by the returned value, so
 *       the next call continues where this one stopped.
 *
 * @note Non-blocking and allocation-free; it neither stops nor reconfigures
 *       the DMA, so reception runs on during the copy.
 * @warning Not reentrant and not ISR-safe.  Two concurrent readers would
 *          return the same bytes and corrupt the read index.
 * @see app_uart3_rx_running
 * @see comms_task.h
 */
uint32_t app_uart3_rx_read(uint8_t *dst, uint32_t max_len);

/**
 * @brief Check whether the DMA receiver is armed and receiving.
 * @details Requires all three independent facts: the HAL reports the USART
 *          handle busy in receive, the RX DMA stream is busy, and the receiver
 *          enable bit is set in @c USART3->CR1.  A peripheral that was
 *          aborted, overrun or de-initialised therefore reports false even
 *          when one of the three states still looks healthy.
 *
 * @return true when USART3 is armed and the DMA ring is being filled.
 *
 * @note Read-only probe with no side effects.  The reads are not atomic with
 *       respect to the UART and DMA interrupts, so the answer is a snapshot
 *       that a restart or a receive error may already have invalidated.
 * @note The health check uses this to prove the HWLINK receiver is running
 *       while the slot is pending (HLT-001).
 * @see app_uart3_rx_restart
 */
bool app_uart3_rx_running(void);

/**
 * @brief Re-arm the circular DMA receiver after an error or an abort.
 * @details No-op while @ref app_uart3_rx_running reports true.  Otherwise it
 *          clears the overrun, framing, noise, parity and IDLE flags through
 *          @c USART3->ICR, aborts whatever receive the HAL still believes is
 *          active and starts the ring again from the beginning, resetting the
 *          read, consumed and wrap counters.
 *
 *          This is the recovery path that keeps the link alive without a
 *          board reset: the Comms task calls it whenever it observes that the
 *          receiver is no longer running (COM-002, HLT-001).
 *
 * @return true when the receiver is armed after the call.
 * @retval true  The receiver was already running, or it was restarted.
 * @retval false Restarting the DMA receive failed; the link stays down until
 *               the next attempt.
 *
 * @pre Called from the Comms task, the only consumer of the ring.
 * @post On success @ref app_uart3_rx_running returns true and the ring is
 *       empty; any unread bytes are discarded.
 *
 * @note The error flags are cleared here rather than in the HAL handler,
 *       because @c HAL_UART_IRQHandler aborts the circular receiver on an
 *       overrun and never re-arms it.  Reading @c RDR instead would steal a
 *       valid byte from the ring.
 * @warning Not reentrant and not ISR-safe; it must not race with
 *          @ref app_uart3_rx_read from another task, because both update the
 *          shared ring indices.
 * @see app_uart3_rx_running
 * @see app_uart3_init
 */
bool app_uart3_rx_restart(void);

/**
 * @brief Transmit one frame over DMA and wait for the last stop bit.
 * @details Starts a normal-mode DMA transfer from the caller's buffer and
 *          blocks the calling task until the USART3 transmit-complete callback
 *          notifies it.  After the DMA has handed the last byte to
 *          @c USART3->DR, the function additionally waits up to 10 ms for the
 *          @c TC flag, so the frame has physically left the shift register
 *          before the caller resets the board or switches the rate (DBT-005).
 *
 *          The buffer is not copied: it must stay valid and unchanged until
 *          the function returns.  Nothing is retried, and a stale completion
 *          notification from an earlier transfer is dropped before the DMA is
 *          started, so a late wake-up cannot be mistaken for this frame.
 *
 * @param[in] data       Frame bytes to send; may be @c NULL only when
 *                       @p len is 0.
 * @param[in] len        Frame length in bytes, at most @c UINT16_MAX.
 * @param[in] timeout_ms Maximum time to wait for the completion, in
 *                       milliseconds; converted with @c pdMS_TO_TICKS, so a
 *                       value below one tick does not block at all.
 *
 * @return true when the frame has been transmitted, or there was nothing to
 *         send.
 * @retval true  @p data is @c NULL or @p len is 0 and nothing was sent, or the
 *               transfer completed and @c TC was observed (or the 10 ms @c TC
 *               wait expired).
 * @retval false The DMA transfer could not be started, or no completion
 *               arrived within @p timeout_ms; the transmit is aborted.
 *
 * @pre A task context with the scheduler running, because the function uses a
 *      task notification and the millisecond tick counter.
 * @post On failure the transmit DMA is aborted, the module's transmit task
 *       handle is cleared and the line is left idle.
 *
 * @note Only the calling task blocks; the LED, button, health and HWLINK
 *       parsing paths keep running (COM-002).
 * @note A missing @c TC flag is not reported as an error: the wait is bounded
 *       to 10 ms and the function still returns true.
 * @warning Single sender: the driver keeps one transmit task handle, so a
 *          second task calling this concurrently would steal or clear the
 *          first transfer's completion wake-up.  Only the Comms task sends.
 * @warning Not reentrant and not ISR-safe.
 * @see comms_task.h
 * @see app_uart3_set_baud
 */
bool app_uart3_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);

/**
 * @brief Wake the Comms task from the USART3 ISR or a DMA callback.
 * @details Posts one payload-free "RX may be ready" hint to the Comms event
 *          queue from interrupt context and yields if a higher-priority task
 *          was unblocked.  The bytes themselves stay in the DMA ring; the task
 *          copies them later with @ref app_uart3_rx_read.
 *
 *          The declaration lives here so @ref uart3.c can post without
 *          including @ref comms_task.h.  The definition and the queue belong
 *          to @ref comms_task.c, and @ref comms_task.h declares the function
 *          again for its own users.
 *
 * @pre Called from the USART3, DMA1 Stream1 or DMA1 Stream3 interrupt.  Before
 *      @ref comms_task_create has run, the queue handle is @c NULL and the
 *      call returns without touching FreeRTOS.
 * @post At most one further hint is pending; no received byte is consumed.
 *
 * @note Hints coalesce, and a full queue only drops the hint, never data,
 *       because the task polls the ring on every loop iteration anyway.
 * @warning Interrupt context only: it uses the @c ...FromISR FreeRTOS API and
 *          must not be called from a task.
 * @see comms_task.h
 */
void comms_post_rx_from_isr(void);

/** @} */

#endif /* UART3_H */
