/**
 * @file    uart3.c
 * @brief   USART3 on the ST-LINK VCP: circular DMA reception, DMA transmit and
 *          the interrupt plumbing that keeps the HWLINK link alive.
 * @details Implements the transport declared in @ref uart3.h and belongs to the
 *          @ref uart3 module group.  USART3 carries HWLINK (DCON-004): PD8 is
 *          TX and PD9 is RX, both in AF7, wired to the on-board ST-LINK
 *          virtual COM port of the NUCLEO-F767ZI.
 *
 *          **Reception.**  DMA1 Stream1, channel 4, copies bytes from
 *          @c USART3->RDR into the 4 KB @ref uart_rx_ring in circular mode.
 *          The CPU never reads the data register; it derives the DMA write
 *          position from the stream's remaining-count register
 *          (@c __HAL_DMA_GET_COUNTER) plus a count of completed full-ring
 *          transfers, so no byte is lost while the Comms task is busy building
 *          a reply (COM-002).  The IDLE-line interrupt, the RX
 *          complete/half-complete callbacks and the error interrupt only post
 *          an "RX may be ready" hint through @ref comms_post_rx_from_isr; the
 *          Comms task then drains the ring with @ref app_uart3_rx_read.
 *
 *          **Transmission.**  DMA1 Stream3, channel 4, is armed per frame in
 *          normal (non-circular) mode from the caller's buffer.  The sending
 *          task publishes its own handle in @ref uart_tx_task and blocks on a
 *          task notification that @ref HAL_UART_TxCpltCallback posts from
 *          interrupt context.  Only the Comms task transmits and only the
 *          caller waits, so a slow host cannot stall the LED, button or health
 *          tasks (COM-002).
 *
 *          **Baud switching.**  @ref app_uart3_set_baud is the only way the
 *          rate changes: it stops the DMA, de-initialises the peripheral,
 *          re-initialises it at the requested rate and re-arms the ring, with a
 *          best-effort rollback to the previous rate (COM-001).
 *
 *          **Errors and recovery.**  Overrun, framing, noise and parity errors
 *          are cleared from @ref USART3_IRQHandler through @c USART3->ICR only.
 *          The HAL receive interrupt handler is deliberately never used for
 *          them, because it aborts the circular receiver on an overrun and
 *          never re-arms it (RM0410 30.6.9).  A consumer that observes a
 *          stopped receiver calls @ref app_uart3_rx_restart, so a receive error
 *          never needs a board reset (COM-002, HLT-001).
 *
 *          The receive ring lives in the non-cacheable @c .dma_buffer window of
 *          SRAM2 that @ref cache_mpu.c maps with the MPU, and the transmit
 *          buffer handed in by the Comms task is placed there as well, so the
 *          interrupt path needs no cache maintenance and the CPU may read the
 *          ring while the DMA writes it.
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (PD8 TX, PD9 RX)
 * @srs     COM-001, COM-002, DCON-004
 * @ingroup uart3
 *
 * @note This file owns the whole USART3 interrupt plumbing: no other
 *       translation unit defines @ref HAL_UART_MspInit, the three USART3
 *       callbacks or @ref USART3_IRQHandler, so the driver alone decides which
 *       HAL paths may run.
 * @see uart3.h, cache_mpu.h, comms_task.h
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "uart3.h"

/**
 * @brief Size of the circular DMA receive ring, in bytes (COM-002).
 * @details Also the length of one DMA transfer in circular mode, so it is the
 *          unit in which @ref uart_rx_wraps counts and the granularity at which
 *          the "DMA lapped the reader" condition is detected.  It must exceed
 *          the largest backlog the DMA can accumulate while the Comms task is
 *          not draining the ring; 4 KB satisfies the 2 KB minimum of COM-002
 *          with margin.  Index arithmetic uses a modulo, so a power of two is
 *          convenient here but not required by the code.
 * @note Only @ref uart_rx_ring, the DMA transfer length and the index arithmetic
 *       depend on this value; the public interface in @ref uart3.h does not
 *       expose it.
 */
#define UART_RX_RING_SIZE       4096U
/**
 * @brief Nominal budget for one DMA frame on the wire, in milliseconds.
 * @details Expresses the driver's default transmit timeout, matching the value
 *          the Comms task uses.  The timeout actually applied is the parameter
 *          of @ref app_uart3_send, so the caller decides
 *          (@ref comms_task.c passes its own @c COMMS_TX_TIMEOUT_MS, also
 *          200 ms).
 * @note This translation unit never references the macro, so it is not
 *       expanded anywhere; it records the intended budget rather than enforcing
 *       it.
 */
#define UART_TX_TIMEOUT_MS      200U

/**
 * @brief USART3 HAL handle, owned by this file.
 * @details Passed to every HAL UART call and to @c HAL_UART_IRQHandler on the
 *          transmit-complete path.  @ref app_uart3_init sets the instance, the
 *          two DMA handles are linked into it by @ref uart_dma_init, and its
 *          @c gState / @c RxState fields are the first input of
 *          @ref app_uart3_rx_running.  Static storage is zero-initialised, so
 *          the first @c HAL_UART_Init sees @c HAL_UART_STATE_RESET and calls
 *          @ref HAL_UART_MspInit.
 * @note No lock protects it: it is touched by the bring-up task, by the Comms
 *       task and by the HAL handlers at interrupt level.
 */
static UART_HandleTypeDef huart3;
/** Receive stream: DMA1 Stream1, channel 4, circular, peripheral-to-memory. */
static DMA_HandleTypeDef hdma_usart3_rx;
/** Transmit stream: DMA1 Stream3, channel 4, normal, memory-to-peripheral. */
static DMA_HandleTypeDef hdma_usart3_tx;

/**
 * @brief 4 KB circular receive ring filled by DMA1 Stream1.
 * @details Placed in @c .dma_buffer so the linker puts it in SRAM2 and the MPU
 *          region programmed by @ref app_cache_mpu_init makes it normal,
 *          non-cacheable memory: DMA writes are immediately visible to the CPU
 *          and @ref app_uart3_rx_read needs no cache invalidation.  The
 *          section is @c NOLOAD, so the buffer is not part of the initialised
 *          image, and 32-byte alignment matches the Cortex-M7 D-cache line and
 *          the stream's burst granularity.
 * @warning Removing the section attribute makes the ring cacheable and the
 *          reader then sees stale bytes; the ring must never be moved out of
 *          @c .dma_buffer.
 */
static uint8_t uart_rx_ring[UART_RX_RING_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));
/**
 * @brief Ring offset of the next byte the consumer will read.
 * @details Always in the range 0..UART_RX_RING_SIZE-1.  Advanced by
 *          @ref app_uart3_rx_read together with @ref uart_rx_consumed, set to
 *          the current write offset on the lapped path, and cleared by
 *          @ref uart_start_receiver.  No interrupt touches it, so only task
 *          contexts have to be serialised against each other.
 */
static uint32_t uart_rx_read;
/**
 * @brief Total bytes consumed from the ring since the receiver was armed.
 * @details Monotonic counter in the same domain as the producer position
 *          @c uart_rx_wraps*UART_RX_RING_SIZE+head computed in
 *          @ref app_uart3_rx_read, so the unread backlog is simply the
 *          difference and no wrap-around comparison is needed.
 */
static uint32_t uart_rx_consumed;   /* total bytes consumed since arming */
/**
 * @brief Number of completed full-ring DMA transfers since arming.
 * @details Volatile because @ref HAL_UART_RxCpltCallback increments it in
 *          interrupt context while @ref app_uart3_rx_read reads it in task
 *          context.  Each increment means the DMA wrapped once, that is
 *          @ref UART_RX_RING_SIZE further bytes were produced.  Cleared by
 *          @ref uart_start_receiver before the DMA is armed, which is what keeps
 *          the producer and consumer domains aligned after a restart.
 * @warning A 32-bit read is atomic on the Cortex-M7, but the value is only
 *          consistent with @c __HAL_DMA_GET_COUNTER when no wrap happens
 *          between the two reads; see @ref app_uart3_rx_read.
 */
static volatile uint32_t uart_rx_wraps; /* full-ring DMA completions */
/**
 * @brief Cached link rate in baud, as last configured successfully.
 * @details Initialised to @ref APP_UART_DEFAULT_BAUD at load time, so
 *          @ref app_uart3_baud is meaningful before @ref app_uart3_init runs.
 *          Updated only after a successful initialisation in
 *          @ref app_uart3_init and @ref app_uart3_set_baud, so a failed switch
 *          keeps reporting the rate that is still on the wire.  This variable,
 *          not @c USART3->BRR, is the record of intent read by
 *          @ref app_uart3_baud and by the early return in
 *          @ref app_uart3_set_baud.
 */
static uint32_t uart_baud = APP_UART_DEFAULT_BAUD;
/**
 * @brief Task waiting for the DMA transmit-complete notification, or @c NULL.
 * @details @ref app_uart3_send stores the calling task before arming the DMA and
 *          clears the field to @c NULL on every exit path.
 *          @ref HAL_UART_TxCpltCallback notifies only while the field is
 *          non-@c NULL, so a late completion cannot wake an unrelated task.
 *          Volatile because the reader runs in interrupt context.
 * @warning Single-slot state: it is why the driver supports exactly one sender
 *          at a time, and why only the Comms task may call
 *          @ref app_uart3_send.
 */
static volatile TaskHandle_t uart_tx_task;

/**
 * @brief Configure PD8/PD9 as USART3 alternate-function pins.
 * @details Enables the GPIOD clock and programs both pins as push-pull
 *          alternate function at very high speed with the internal pull-up and
 *          AF7.  The internal pull-up keeps both pins defined while nothing is
 *          driving them on the ST-LINK side, and the slew rate is the one that
 *          covers the 921600 baud rate of COM-001.
 *
 * @pre The pin mapping is a static configuration: no runtime check is made that
 *      these pins really are the VCP lines.
 * @post PD8 and PD9 are owned by USART3 and must not be used as plain GPIO.
 *
 * @note Runs in the context of whoever initialises the peripheral, that is
 *       @c main before the scheduler or the Comms task from
 *       @ref app_uart3_set_baud.
 * @note The function cannot fail and reports nothing; a wrong pin or
 *       alternate function shows up only as a dead link.
 * @internal File-local helper, called only from @ref HAL_UART_MspInit.
 */
static void uart_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9;   /* PD8 TX, PD9 RX */
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOD, &gpio);
}

/**
 * @brief Create, link and enable the two USART3 DMA streams.
 * @details Enables the DMA1 clock and then, in this order:
 *          -# configures DMA1 Stream1 on channel 4 for byte-wise
 *             peripheral-to-memory transfers with memory increment, in
 *             **circular** mode and at very high stream priority, for reception;
 *          -# configures DMA1 Stream3 on channel 4 for byte-wise
 *             memory-to-peripheral transfers with memory increment, in
 *             **normal** mode and at high stream priority, for transmission;
 *          -# links both handles into @ref huart3 with @c __HAL_LINKDMA so the
 *             HAL UART driver finds them;
 *          -# sets both NVIC lines to pre-emption priority 6, subpriority 0, and
 *             enables them.
 *
 *          Both streams run with the FIFO disabled, single-beat memory and
 *          peripheral bursts.  Byte granularity is what an 8N1 UART needs, and
 *          disabling the FIFO keeps the stream from holding bytes that an abort
 *          would have to flush.  Priority 6 is a kernel-aware priority (at or
 *          below @c configMAX_SYSCALL_INTERRUPT_PRIORITY, library number 5), so
 *          the stream callbacks may use the FromISR API (DRT-003).
 *
 * @pre Called from @ref HAL_UART_MspInit, that is after the USART3 clock is on
 *      and after @ref app_cache_mpu_init has made the DMA window non-cacheable.
 * @post Both streams are enabled in the NVIC but not armed: reception starts
 *       with @c HAL_UART_Receive_DMA, transmission per frame with
 *       @c HAL_UART_Transmit_DMA.
 *
 * @note The result of both @c HAL_DMA_Init calls is discarded.  A failure
 *       surfaces later as @c HAL_UART_Receive_DMA returning an error, which
 *       @ref uart_start_receiver and therefore @ref app_uart3_init report.
 * @note The function is re-entered on every @c HAL_UART_Init because
 *       @ref app_uart3_set_baud de-initialises the peripheral first.  Programming
 *       an already configured stream again is safe here: no transfer is in
 *       flight, and there is no @c HAL_UART_MspDeInit that would undo it.
 * @warning The stream numbers must match the NVIC enables and the
 *          @ref DMA1_Stream1_IRQHandler / @ref DMA1_Stream3_IRQHandler handlers
 *          below, and they must stay off every other peripheral's channels.
 * @internal File-local helper, called only from @ref HAL_UART_MspInit.
 */
static void uart_dma_init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_usart3_rx.Instance                 = DMA1_Stream1;
    hdma_usart3_rx.Init.Channel             = DMA_CHANNEL_4;
    hdma_usart3_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_usart3_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart3_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode                = DMA_CIRCULAR;
    hdma_usart3_rx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
    hdma_usart3_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    hdma_usart3_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_1QUARTERFULL;
    hdma_usart3_rx.Init.MemBurst            = DMA_MBURST_SINGLE;
    hdma_usart3_rx.Init.PeriphBurst         = DMA_PBURST_SINGLE;
    (void)HAL_DMA_Init(&hdma_usart3_rx);

    hdma_usart3_tx.Instance                 = DMA1_Stream3;
    hdma_usart3_tx.Init.Channel             = DMA_CHANNEL_4;
    hdma_usart3_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_usart3_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_usart3_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_usart3_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_usart3_tx.Init.Mode                = DMA_NORMAL;
    hdma_usart3_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_usart3_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    hdma_usart3_tx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_1QUARTERFULL;
    hdma_usart3_tx.Init.MemBurst            = DMA_MBURST_SINGLE;
    hdma_usart3_tx.Init.PeriphBurst         = DMA_PBURST_SINGLE;
    (void)HAL_DMA_Init(&hdma_usart3_tx);

    __HAL_LINKDMA(&huart3, hdmarx, hdma_usart3_rx);
    __HAL_LINKDMA(&huart3, hdmatx, hdma_usart3_tx);

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
    HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Stream3_IRQn);
}

/**
 * @brief HAL MSP hook: clocks, pins, DMA and NVIC for USART3.
 * @details Called by the HAL from @c HAL_UART_Init whenever the handle is in the
 *          reset state, which happens on the first bring-up in
 *          @ref app_uart3_init and again after every de-initialisation in
 *          @ref app_uart3_set_baud.  For the USART3 instance it enables the
 *          USART3 clock, calls @ref uart_gpio_init and @ref uart_dma_init and
 *          finally enables the USART3 interrupt itself at pre-emption priority
 *          6, subpriority 0.
 *
 *          Handles for any other instance return immediately, so a second UART
 *          added to the project cannot accidentally inherit the USART3 pins or
 *          DMA streams.  The debug UART on USART6 (@ref app_log.c) bypasses the
 *          HAL UART driver entirely and is unaffected.
 *
 * @param[in] huart Handle being initialised.  Only @c USART3 is handled; the
 *                  instance field is dereferenced before any other test, so
 *                  @c NULL faults.
 *
 * @pre Called by the HAL in task context with interrupts enabled.
 * @post On the USART3 path every resource the driver needs exists and the three
 *       NVIC lines are enabled; nothing is armed yet.
 *
 * @note The USART3 interrupt is enabled before the first
 *       @c HAL_UART_Receive_DMA.  @ref USART3_IRQHandler tolerates that because
 *       it acts only on flags that are set in @c USART3->ISR.
 * @note There is no @c HAL_UART_MspDeInit in this project, so the clocks, pins,
 *       streams and NVIC lines survive @c HAL_UART_DeInit and are simply
 *       re-programmed when this hook runs again.
 * @warning Priority 6 must remain a kernel-aware priority; raising it above
 *          @c configMAX_SYSCALL_INTERRUPT_PRIORITY would let the callbacks
 *          corrupt kernel critical sections (DRT-003).
 * @see app_uart3_init, app_uart3_set_baud
 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART3) {
        return;
    }

    __HAL_RCC_USART3_CLK_ENABLE();
    uart_gpio_init();
    uart_dma_init();

    HAL_NVIC_SetPriority(USART3_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
}

/**
 * @brief Reset the ring bookkeeping and arm the circular DMA receiver.
 * @details Clears @ref uart_rx_read, @ref uart_rx_consumed and
 *          @ref uart_rx_wraps so consumer and DMA share one origin, then calls
 *          @c HAL_UART_Receive_DMA over the whole ring and, if that succeeded,
 *          enables the error interrupt (EIE in @c USART3->CR3) and the IDLE
 *          interrupt (IDLEIE in @c USART3->CR1).
 *
 *          The order matters: the counters are cleared before the DMA is armed,
 *          so the first poll cannot see a wrap or a backlog belonging to the
 *          previous session.  Bytes already in the ring are neither cleared nor
 *          read; they are simply overwritten as the DMA advances.
 *
 * @return true when the receive DMA started and both interrupts were enabled.
 * @retval false @c HAL_UART_Receive_DMA reported an error.  The counters have
 *               already been cleared while the receiver is not armed, so
 *               @ref app_uart3_rx_running reports false.
 *
 * @pre The peripheral is initialised and @ref uart_rx_ring is its receive
 *      buffer: @ref HAL_UART_MspInit has run and linked the receive stream.
 * @post On success the ring is filling and the module state matches an empty
 *       ring.
 *
 * @note Not ISR-safe and must not race with @ref app_uart3_rx_read, because it
 *       rewrites the indices that function advances.
 * @internal File-local helper shared by bring-up, baud switch and restart.
 * @see app_uart3_rx_restart, app_uart3_init
 */
static bool uart_start_receiver(void)
{
    uart_rx_read = 0U;
    uart_rx_consumed = 0U;
    uart_rx_wraps = 0U;
    if (HAL_UART_Receive_DMA(&huart3, uart_rx_ring, UART_RX_RING_SIZE) !=
        HAL_OK) {
        return false;
    }
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_ERR);
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_IDLE);
    return true;
}

/**
 * @brief Configure USART3 for 8N1 at the default rate and arm reception.
 * @details Fills the init structure - @ref APP_UART_DEFAULT_BAUD, 8 data bits,
 *          1 stop bit, no parity, transmitter and receiver enabled, no hardware
 *          flow control, 16x oversampling - and calls @c HAL_UART_Init, which
 *          runs @ref HAL_UART_MspInit for the clocks, pins, DMA streams and
 *          NVIC lines.  The cached rate is forced back to
 *          @ref APP_UART_DEFAULT_BAUD once the peripheral is up, and the ring is
 *          armed through @ref uart_start_receiver.
 *
 * @return true when the peripheral is initialised and reception is armed.
 * @retval false @c HAL_UART_Init failed and the peripheral is unusable, or
 *               @ref uart_start_receiver could not start the receive DMA.
 *
 * @pre Called once from @c main before @c vTaskStartScheduler, after
 *      @ref app_clock_init, @ref app_cache_mpu_init and the HAL time base.
 *      A failure there is fatal (@c startup_fail("uart3 init")).
 * @post On success the ring is armed, @ref app_uart3_rx_running reports true and
 *       @ref app_uart3_baud returns @ref APP_UART_DEFAULT_BAUD.  On failure
 *       nothing usable is left armed and recovery is the caller's decision.
 *
 * @note Task context only: the HAL, the NVIC and the peripheral registers are
 *       touched without any locking, and the driver assumes it is the only user
 *       of USART3.
 * @warning Not reentrant and not safe on a running link; a second call
 *          re-initialises the peripheral underneath a live reception and drops
 *          every queued byte.
 * @see app_uart3_set_baud, cache_mpu.h, uart3.h
 */
bool app_uart3_init(void)
{
    huart3.Instance          = USART3;
    huart3.Init.BaudRate     = APP_UART_DEFAULT_BAUD;
    huart3.Init.WordLength   = UART_WORDLENGTH_8B;
    huart3.Init.StopBits     = UART_STOPBITS_1;
    huart3.Init.Parity       = UART_PARITY_NONE;
    huart3.Init.Mode         = UART_MODE_TX_RX;
    huart3.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart3.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart3) != HAL_OK) {
        return false;
    }
    uart_baud = APP_UART_DEFAULT_BAUD;
    return uart_start_receiver();
}

/**
 * @brief Switch the link between the default and the fast rate.
 * @details Rejects any rate other than @ref APP_UART_DEFAULT_BAUD or
 *          @ref APP_UART_FAST_BAUD, and returns early - without touching the
 *          hardware at all - when the requested rate is already active.
 *          Otherwise it:
 *          -# stops every DMA transfer of the peripheral
 *             (@c HAL_UART_DMAStop);
 *          -# de-initialises the peripheral, which drops the HAL state to reset;
 *          -# re-initialises at @p baud, which re-runs @ref HAL_UART_MspInit;
 *          -# re-arms the ring, discarding every byte still queued in it.
 *
 *          When initialisation or re-arming fails the link is rolled back: the
 *          peripheral is de-initialised again, brought back up at the previous
 *          rate and, if that worked, re-armed.  A rejected switch therefore
 *          normally leaves a working link at the old rate.
 *
 * @param[in] baud Requested rate in baud: @ref APP_UART_DEFAULT_BAUD or
 *                 @ref APP_UART_FAST_BAUD.
 *
 * @return true when the link runs at @p baud afterwards.
 * @retval true  @p baud was already active, or the switch and the re-arm both
 *               succeeded.
 * @retval false @p baud is unsupported, or the switch failed and the rollback
 *               ran; the old rate is kept unless even the rollback failed, in
 *               which case the peripheral is left de-initialised.
 *
 * @pre Called from the Comms task, the only user of USART3, with no transmit in
 *      flight: the caller has already put its @c SetBaud @c Ack on the wire at
 *      the old rate and resets its frame parser afterwards (COM-001).
 * @post On success @ref uart_baud reports @p baud; on every failure path it
 *       still reports the rate that was active before the call.  The ring
 *       counters are reset on every path that reaches the hardware.
 *
 * @note The 5 s revert to @ref APP_UART_DEFAULT_BAUD when no valid frame arrives
 *       at the new rate lives in @ref comms_task.c; this function only performs
 *       the switch it is asked for.
 * @note The rollback's own @ref uart_start_receiver result is ignored, so the
 *       caller cannot distinguish a recovered rollback from a dead link except
 *       through @ref app_uart3_rx_running.
 * @warning Aborts everything the peripheral is doing, including a transmit
 *          started from another task; that task loses its frame and never gets
 *          its completion notification.  Not reentrant and not ISR-safe.
 * @see app_uart3_baud, app_uart3_init
 */
bool app_uart3_set_baud(uint32_t baud)
{
    if (baud != APP_UART_DEFAULT_BAUD && baud != APP_UART_FAST_BAUD) {
        return false;
    }
    if (baud == uart_baud) {
        return true;
    }

    uint32_t old_baud = uart_baud;
    (void)HAL_UART_DMAStop(&huart3);
    (void)HAL_UART_DeInit(&huart3);

    huart3.Init.BaudRate = baud;
    bool ok = HAL_UART_Init(&huart3) == HAL_OK && uart_start_receiver();
    if (!ok) {
        /* Get back to the previous rate. */
        (void)HAL_UART_DeInit(&huart3);
        huart3.Init.BaudRate = old_baud;
        if (HAL_UART_Init(&huart3) == HAL_OK) {
            (void)uart_start_receiver();
        }
        return false;
    }

    uart_baud = baud;
    return true;
}

/**
 * @brief Return the cached link rate.
 * @details Reads @ref uart_baud only; the peripheral and the HAL are not touched,
 *          so the call is valid before @ref app_uart3_init and cheap in any
 *          context.
 *
 * @return Configured rate in baud; @ref APP_UART_DEFAULT_BAUD until a switch
 *         away from it succeeds.
 *
 * @note Read-only and free of side effects, but the value is stale as soon as
 *       @ref app_uart3_set_baud runs concurrently.
 * @see app_uart3_set_baud
 */
uint32_t app_uart3_baud(void)
{
    return uart_baud;
}

/**
 * @brief Copy unread bytes out of the circular DMA ring.
 * @details The producer position is derived, never read from a register the DMA
 *          owns:
 *          @code
 *          head     = UART_RX_RING_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
 *          head %= UART_RX_RING_SIZE;
 *          produced = uart_rx_wraps * UART_RX_RING_SIZE + head;
 *          @endcode
 *          The remaining-count register counts down from @ref UART_RX_RING_SIZE
 *          to 0 and reloads on completion, so the subtraction yields the current
 *          write offset; the modulo maps both "counter full" and "counter just
 *          reloaded" to offset 0.  @ref uart_rx_wraps adds one ring per completed
 *          transfer, so @c produced is a monotonic byte counter in the same
 *          domain as @ref uart_rx_consumed and the unread backlog is simply their
 *          difference - no head/tail ambiguity, and a completely full ring is
 *          still describable.
 *
 *          Up to @p max_len bytes are copied oldest first in a plain byte loop
 *          that wraps at the end of the ring, and @ref uart_rx_read and
 *          @ref uart_rx_consumed then advance by the number copied.  Bytes are
 *          not erased, they are only overwritten by later traffic.
 *
 *          If the backlog exceeds @ref UART_RX_RING_SIZE the DMA has lapped the
 *          reader and the oldest unread bytes are already gone.  The function
 *          then resynchronises on the current write position - read index set to
 *          @c head, consumed set to @c produced - drops the whole backlog and
 *          returns 0 instead of returning out-of-order data.
 *
 *          A completion can reload the DMA counter before
 *          @ref HAL_UART_RxCpltCallback publishes the matching wrap increment.
 *          In that short window @c produced has already jumped to the next ring
 *          while @c uart_rx_wraps is still old, so the unsigned difference looks
 *          like a near-4 GiB backlog rather than a lap.  Such a backwards delta
 *          (within @ref UART_RX_RING_SIZE of @c UINT32_MAX) is treated as a
 *          deferred wrap: the function returns 0 without consuming or
 *          resynchronising, and the next call observes the published counter
 *          and wrap together.
 *
 * @param[out] dst     Destination buffer, written with at most @p max_len bytes.
 * @param[in]  max_len Capacity of @p dst in bytes; 0 makes the call a no-op.
 *
 * @return Number of bytes copied into @p dst.
 * @retval 0 Nothing was unread, @p max_len was 0, the wrap increment was still
 *           in flight, or the reader was lapped and the ring was resynchronised.
 *
 * @pre @p dst addresses at least @p max_len writable bytes.  The pointer is not
 *      validated, so @c NULL with a non-zero @p max_len faults.
 * @pre Called from the single consumer task (Comms), never concurrently with
 *      @ref app_uart3_rx_restart, which rewrites the same indices.
 * @post The unread backlog shrank by the returned count, or was dropped entirely
 *       on the lapped path.
 *
 * @note Non-blocking, allocation-free and lock-free.  The DMA keeps filling the
 *       ring during the copy, so the result is a snapshot of the bytes that had
 *       arrived when @c produced was computed.
 * @note The wrap counter is sampled before and after the DMA remaining-count
 *       register.  If a full-ring completion lands between the two reads, the
 *       pair is retried, so @c produced can never combine a new wrap counter
 *       with an old ring offset and jump one ring ahead.  If instead the counter
 *       reload happens before @ref HAL_UART_RxCpltCallback increments the wrap
 *       counter, the deferred-wrap check returns 0 until both agree.
 * @warning Not reentrant and not ISR-safe.  Two concurrent readers would return
 *          the same bytes and corrupt @ref uart_rx_read.
 * @see app_uart3_rx_running, app_uart3_rx_restart, comms_task.h
 */
uint32_t app_uart3_rx_read(uint8_t *dst, uint32_t max_len)
{
    /* Sample the wrap counter on both sides of the DMA position.  A full-ring
     * completion between the two reads increments uart_rx_wraps and reloads the
     * counter; retrying then keeps the pair consistent instead of advancing
     * "produced" by a whole ring. */
    uint32_t wraps = 0U;
    uint32_t counter = 0U;
    do {
        wraps = uart_rx_wraps;
        counter = __HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
    } while (wraps != uart_rx_wraps);

    uint32_t head = (UART_RX_RING_SIZE - counter) % UART_RX_RING_SIZE;
    uint32_t produced = (wraps * UART_RX_RING_SIZE) + head;

    uint32_t delta = produced - uart_rx_consumed;
    if (delta == 0U || max_len == 0U) {
        return 0U;
    }
    if (delta > (0xFFFFFFFFUL - UART_RX_RING_SIZE)) {
        /* The DMA counter reloaded before the completion callback published the
         * wrap increment.  Defer to the next call instead of mistaking the
         * backwards difference for a lap and discarding the ring. */
        return 0U;
    }
    if (delta > UART_RX_RING_SIZE) {
        /* The DMA lapped the reader: bytes were overwritten.  Resync at the
         * write position instead of returning out-of-order data. */
        uart_rx_read = head;
        uart_rx_consumed = produced;
        return 0U;
    }

    uint32_t count = delta;
    if (count > max_len) {
        count = max_len;
        produced = uart_rx_consumed + count;
    }
    for (uint32_t i = 0U; i < count; i++) {
        dst[i] = uart_rx_ring[(uart_rx_read + i) % UART_RX_RING_SIZE];
    }
    uart_rx_read = (uart_rx_read + count) % UART_RX_RING_SIZE;
    uart_rx_consumed = produced;
    return count;
}

/**
 * @brief Report whether the DMA receiver is armed and the ring is filling.
 * @details Requires three independent facts at once: the HAL reports the USART3
 *          handle busy in receive, the receive DMA stream reports busy, and the
 *          receiver enable bit is set in @c USART3->CR1.  Each of them can
 *          outlive a failure of the others, so only the conjunction is trusted by
 *          the Comms recovery path and by the health check (HLT-001).
 *
 * @return true when USART3 is receiving into the ring.
 * @retval false The peripheral was aborted, overrun, de-initialised or never
 *               armed; @ref app_uart3_rx_restart is then the recovery path.
 *
 * @note Read-only, lock-free and callable from any context, but the reads are
 *       not atomic with respect to the DMA and USART3 interrupts, so the answer
 *       is a snapshot that a restart or a receive error may already have
 *       invalidated.
 * @see app_uart3_rx_restart, app_uart3_init
 */
bool app_uart3_rx_running(void)
{
    return huart3.RxState == HAL_UART_STATE_BUSY_RX &&
           hdma_usart3_rx.State == HAL_DMA_STATE_BUSY &&
           (USART3->CR1 & USART_CR1_RE) != 0U;
}

/**
 * @brief Clear the receive error flags, unwind what is left and re-arm the ring.
 * @details A no-op while @ref app_uart3_rx_running reports true.  Otherwise it
 *          writes the overrun, framing, noise, parity and IDLE clear flags to
 *          @c USART3->ICR, calls @c HAL_UART_AbortReceive to unwind any
 *          half-finished reception the HAL still tracks, and arms the ring again
 *          with @ref uart_start_receiver, which resets the three ring counters
 *          and therefore discards unread bytes.
 *
 *          This is the recovery path that keeps HWLINK alive without a board
 *          reset: the Comms task calls it whenever it observes a stopped
 *          receiver, and the health check reports the receiver state while a
 *          hand-over slot is pending (COM-002, HLT-001).
 *
 * @return true when the receiver is armed after the call.
 * @retval true  It was already running, or the restart succeeded.
 * @retval false Re-arming the receive DMA failed; the link stays down until the
 *               next attempt.
 *
 * @pre Called from the Comms task, the only consumer of the ring.
 * @post On success @ref app_uart3_rx_running reports true and the ring is empty.
 *
 * @note The flags are cleared here and in @ref USART3_IRQHandler rather than in
 *       the HAL handler, because @c HAL_UART_IRQHandler aborts the circular
 *       receiver on an overrun and never re-arms it.  Clearing through
 *       @c USART3->ICR also avoids reading @c RDR, which would steal a valid
 *       byte from the ring (RM0410 30.6.9).
 * @warning Not reentrant and not ISR-safe; it must not race with
 *          @ref app_uart3_rx_read, which advances the same indices.
 * @see app_uart3_rx_running, app_uart3_init
 */
bool app_uart3_rx_restart(void)
{
    if (app_uart3_rx_running()) {
        return true;
    }
    /* A DMA transfer error or an overrun can leave the HAL receive state
     * and both DMA streams idle.  Clear the flags, abort what is left and
     * arm the ring again so HWLINK comes back without a reset. */
    USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF |
                  USART_ICR_PECF | USART_ICR_IDLECF;
    (void)HAL_UART_AbortReceive(&huart3);
    return uart_start_receiver();
}

/**
 * @brief Transmit one frame with DMA and wait until the last stop bit is out.
 * @details Publishes the calling task in @ref uart_tx_task, drops one pending
 *          notification left over from an earlier transfer so a late wake-up
 *          cannot be mistaken for this frame, and starts a normal-mode DMA
 *          transfer from @p data.  It then blocks in @c ulTaskNotifyTake for at
 *          most @p timeout_ms; the notification is posted by
 *          @ref HAL_UART_TxCpltCallback once the HAL has ended the transmit.
 *
 *          After the notification the function waits up to 10 ms more for
 *          @c USART_ISR_TC, so the last character has physically left the shift
 *          register, and then clears @c TCCF.  Waiting only for the DMA would
 *          let the caller switch the rate or reset the board while the final
 *          stop bit is still being shifted (DBT-005), which is exactly what
 *          @c EnterBootloader and @c Reset depend on.
 *
 *          On the timeout path the transfer is aborted with
 *          @c HAL_UART_AbortTransmit and the task handle is cleared, so the line
 *          is left idle and a later completion cannot notify a task that is no
 *          longer waiting.
 *
 * @param[in] data       Frame bytes.  Not copied, so the buffer must stay valid
 *                       and unmodified until the function returns.
 * @param[in] len        Frame length in bytes, at most @c UINT16_MAX by type.
 * @param[in] timeout_ms Maximum wait for the DMA completion, in milliseconds;
 *                       converted with @c pdMS_TO_TICKS, so a value below one
 *                       tick does not block at all.
 *
 * @return true when the frame was transmitted or there was nothing to send.
 * @retval true  @p len is 0 or @p data is @c NULL (nothing transmitted), or the
 *               transfer completed and @c TC was seen within 10 ms.
 * @retval false @c HAL_UART_Transmit_DMA could not start, or no completion
 *               arrived within @p timeout_ms and the transmit was aborted.
 *
 * @pre Task context with the scheduler running: the function uses a task
 *      notification and the millisecond tick, so it must never be called from an
 *      ISR.  The peripheral must be initialised and no other task may be sending.
 * @post @ref uart_tx_task is @c NULL on every path; on failure the transmit DMA
 *       is aborted and the line is idle.
 *
 * @note Only the calling task blocks; the LED, button, health and HWLINK parsing
 *       paths keep running (COM-002).
 * @note A missing @c TC flag is not reported as an error: the extra wait is
 *       bounded to 10 ms and the function still returns true, so a marginal
 *       timing miss degrades to the DMA-only behaviour.
 * @warning Single sender.  The module keeps one transmit handle, so a second
 *          task calling this concurrently would clear or steal the first
 *          transfer's completion.  Only the Comms task sends.
 * @warning A @c NULL @p data with a non-zero @p len is reported as success
 *          although nothing was transmitted; the caller cannot detect that
 *          mistake from the return value.
 * @see comms_task.h, app_uart3_set_baud, DBT-005
 */
bool app_uart3_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    if (len == 0U || data == NULL) {
        return true;
    }

    uart_tx_task = xTaskGetCurrentTaskHandle();
    (void)ulTaskNotifyTake(pdTRUE, 0U);   /* drop a stale completion */

    if (HAL_UART_Transmit_DMA(&huart3, (uint8_t *)data, len) != HAL_OK) {
        uart_tx_task = NULL;
        return false;
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) == 0U) {
        (void)HAL_UART_AbortTransmit(&huart3);
        uart_tx_task = NULL;
        return false;
    }

    /* DBT-005: wait until the last character has left the shift register,
     * not just until the DMA has written it to USART3->DR. */
    uint32_t start = HAL_GetTick();
    while ((USART3->ISR & USART_ISR_TC) == 0U) {
        if ((HAL_GetTick() - start) > 10U) {
            break;
        }
    }
    USART3->ICR = USART_ICR_TCCF;

    uart_tx_task = NULL;
    return true;
}

/**
 * @brief HAL callback: a full DMA receive transfer has completed.
 * @details Interrupt context, reached from @ref DMA1_Stream1_IRQHandler through
 *          @c HAL_DMA_IRQHandler.  For USART3 it counts one ring wrap in
 *          @ref uart_rx_wraps - the finished transfer is exactly
 *          @ref UART_RX_RING_SIZE bytes - and posts the "RX may be ready" hint to
 *          the Comms task with @ref comms_post_rx_from_isr.  No byte is copied
 *          here.
 *
 * @param[in] huart Handle that completed.  Other instances are ignored.
 *
 * @note The wrap counter is incremented before the hint is posted, so a task
 *       woken by the hint always observes the new value.
 * @note Called only while the circular receiver stays armed; an aborted or
 *       overrun receiver never gets here, which is why the consumer polls
 *       @ref app_uart3_rx_running and calls @ref app_uart3_rx_restart.
 * @see app_uart3_rx_read, comms_post_rx_from_isr
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        uart_rx_wraps++;
        comms_post_rx_from_isr();
    }
}

/**
 * @brief HAL callback: the DMA receive ring is half full.
 * @details Interrupt context, from @ref DMA1_Stream1_IRQHandler.  For USART3 it
 *          only posts the "RX may be ready" hint, so the consumer is woken long
 *          before a full ring could be lapped.  The half-way mark needs no
 *          bookkeeping of its own, because the ring position is derived from the
 *          DMA counter in @ref app_uart3_rx_read.
 *
 * @param[in] huart Handle that reached the half-transfer mark.  Other instances
 *                  are ignored.
 *
 * @note Hints coalesce in the event queue, so a missed half-transfer event costs
 *       latency, never data.
 * @see app_uart3_rx_read, HAL_UART_RxCpltCallback
 */
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        comms_post_rx_from_isr();
    }
}

/**
 * @brief HAL callback: the DMA transmit has finished.
 * @details Interrupt context, reached from @ref USART3_IRQHandler after
 *          @c HAL_UART_IRQHandler has ended the transfer.  If the handle belongs
 *          to USART3 and a task is registered in @ref uart_tx_task, it posts one
 *          task notification to that task and yields if the woken task is now the
 *          highest priority ready task, which unblocks @ref app_uart3_send.
 *
 * @param[in] huart Handle that completed.  Other instances are ignored.
 *
 * @note Both the instance and the registered task are checked, so a spurious
 *       callback cannot notify a task that never started a transfer.
 * @note This is the only wake-up for a DMA transmit; if it were skipped the
 *       sender would block until its timeout and then abort a frame that was
 *       already on the wire.
 * @warning Must stay at or below @c configMAX_SYSCALL_INTERRUPT_PRIORITY,
 *          because it calls @c vTaskNotifyGiveFromISR (DRT-003).
 * @see app_uart3_send, USART3_IRQHandler
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART3 || uart_tx_task == NULL) {
        return;
    }

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(uart_tx_task, &woken);
    portYIELD_FROM_ISR(woken);
}

/**
 * @brief HAL callback: the HAL observed a UART or DMA error.
 * @details Interrupt context.  For USART3 it only posts the "RX may be ready"
 *          hint, so the consumer drains whatever arrived before the error and
 *          notices a receiver the HAL has aborted.  It does not clear any flag and
 *          does not restart the receiver: @ref USART3_IRQHandler clears the
 *          receive error flags before the HAL can see them, and
 *          @ref app_uart3_rx_restart does the recovery.
 *
 * @param[in] huart Handle that reported the error.  Other instances are ignored.
 *
 * @note The paths that reach this callback are the DMA transfer errors reported
 *       by @ref DMA1_Stream1_IRQHandler and @ref DMA1_Stream3_IRQHandler;
 *       overrun, framing, noise and parity conditions are handled before the HAL
 *       is involved.
 * @see USART3_IRQHandler, app_uart3_rx_restart
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        comms_post_rx_from_isr();
    }
}

/**
 * @brief USART3 interrupt handler: receive errors, transmit complete and IDLE.
 * @details Takes one snapshot of @c USART3->ISR into a local variable and then:
 *          -# clears overrun, framing, noise and parity errors through
 *             @c USART3->ICR only, and marks that a wake-up is needed.  The HAL
 *             is deliberately not involved: @c HAL_UART_IRQHandler aborts the
 *             circular DMA receiver on an overrun and never re-arms it, and
 *             reading @c RDR to clear the flags would steal a valid byte from
 *             the ring (RM0410 30.6.9);
 *          -# calls @c HAL_UART_IRQHandler only when @c TC is set **and** the
 *             transmit-complete interrupt is enabled, which is precisely the
 *             moment the HAL must end a DMA transmit, return the handle to ready
 *             and call @ref HAL_UART_TxCpltCallback.  Running it after the error
 *             flags were cleared keeps the HAL off its error path, and leaving
 *             @c TCCF set lets @ref app_uart3_send wait for the last stop bit
 *             (DBT-005);
 *          -# tests @c IDLE in a fresh read of @c USART3->ISR - the flag can be
 *             raised while the code above runs - clears it and marks a wake-up
 *             as well;
 *          -# posts a single @ref comms_post_rx_from_isr hint if anything
 *             happened, so a burst of conditions costs one event.
 *
 * @note Interrupt context at pre-emption priority 6, which allows the FromISR
 *       API (DRT-003).  The handler never blocks, copies no data and touches no
 *       ring index: the position is derived by the consumer.
 * @note Only the transmit-complete path is delegated to the HAL.  IDLE and the
 *       receive errors are handled here in full because the HAL would abort the
 *       receiver instead.
 * @warning The @c USART3->ICR writes are whole-register stores of
 *          write-1-to-clear bits; they must never become read-modify-write
 *          accesses, which would clear flags that were set in between.
 * @see HAL_UART_TxCpltCallback, HAL_UART_ErrorCallback, app_uart3_rx_restart
 */
void USART3_IRQHandler(void)
{
    uint32_t sr = USART3->ISR;
    bool wake = false;

    /* Receive errors are cleared here instead of through the HAL, because
     * HAL_UART_IRQHandler() aborts the circular DMA receiver on an overrun.
     * Clear through ICR only: reading RDR while the DMA receiver is active
     * would steal a valid byte from the ring (RM0410 30.6.9). */
    if ((sr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0U) {
        USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF |
                      USART_ICR_NCF | USART_ICR_PECF;
        wake = true;
    }

    /* Transmit complete: the HAL disables TCIE, returns the handle to READY
     * and calls HAL_UART_TxCpltCallback(), which wakes the Comms task.  This
     * is the only path that ends a DMA transmit, so it must run.  It is safe
     * to call the HAL now because the error flags above are already cleared;
     * leaving the TCC flag set lets app_uart3_send() wait for the last stop
     * bit (DBT-005). */
    if (((sr & USART_ISR_TC) != 0U) &&
        ((USART3->CR1 & USART_CR1_TCIE) != 0U)) {
        HAL_UART_IRQHandler(&huart3);
    }

    if ((USART3->ISR & USART_ISR_IDLE) != 0U) {
        USART3->ICR = USART_ICR_IDLECF;
        wake = true;
    }
    if (wake) {
        comms_post_rx_from_isr();
    }
}

/**
 * @brief DMA1 Stream1 interrupt: receive stream events.
 * @details Delegates to @c HAL_DMA_IRQHandler, which resolves the stream from the
 *          handle and calls the matching HAL UART DMA callbacks: the half-transfer
 *          and full-transfer receive callbacks on completion, and
 *          @ref HAL_UART_ErrorCallback on a transfer error.
 *
 * @note Interrupt context at pre-emption priority 6, the priority
 *       @ref uart_dma_init programs (DRT-003).
 * @see HAL_UART_RxCpltCallback, HAL_UART_RxHalfCpltCallback, uart_dma_init
 */
void DMA1_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart3_rx);
}

/**
 * @brief DMA1 Stream3 interrupt: transmit stream events.
 * @details Delegates to @c HAL_DMA_IRQHandler for the transmit handle, which
 *          continues the HAL transmit sequence or reports a transfer error through
 *          @ref HAL_UART_ErrorCallback.  It does not wake the sending task: that
 *          wake-up comes from @ref HAL_UART_TxCpltCallback after the USART3
 *          handler has seen the last stop bit.
 *
 * @note Interrupt context at pre-emption priority 6 (DRT-003).
 * @see app_uart3_send, HAL_UART_TxCpltCallback, uart_dma_init
 */
void DMA1_Stream3_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart3_tx);
}
