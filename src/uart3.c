/*
 * uart3.c - USART3 on the ST-LINK VCP
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  NUCLEO-F767ZI (PD8 TX, PD9 RX)
 * SRS:     COM-001, COM-002, DCON-004
 *
 * Receive is a 4 KB circular DMA ring; the Comms task polls the DMA write
 * index and never loses a byte while it is busy with a reply.  Transmit is
 * DMA-driven and only the Comms task waits for completion.  Both buffers
 * live in the non-cacheable .dma_buffer SRAM2 window (cache_mpu.c).
 */

#include "stm32f7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "uart3.h"

#define UART_RX_RING_SIZE       4096U
#define UART_TX_TIMEOUT_MS      200U

static UART_HandleTypeDef huart3;
static DMA_HandleTypeDef hdma_usart3_rx;
static DMA_HandleTypeDef hdma_usart3_tx;

static uint8_t uart_rx_ring[UART_RX_RING_SIZE]
    __attribute__((section(".dma_buffer"), aligned(32)));
static uint32_t uart_rx_read;
static uint32_t uart_baud = APP_UART_DEFAULT_BAUD;
static volatile TaskHandle_t uart_tx_task;

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

static bool uart_start_receiver(void)
{
    uart_rx_read = 0U;
    if (HAL_UART_Receive_DMA(&huart3, uart_rx_ring, UART_RX_RING_SIZE) !=
        HAL_OK) {
        return false;
    }
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_ERR);
    __HAL_UART_ENABLE_IT(&huart3, UART_IT_IDLE);
    return true;
}

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

uint32_t app_uart3_baud(void)
{
    return uart_baud;
}

uint32_t app_uart3_rx_read(uint8_t *dst, uint32_t max_len)
{
    uint32_t head = UART_RX_RING_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart3_rx);
    head %= UART_RX_RING_SIZE;

    if (head == uart_rx_read || max_len == 0U) {
        return 0U;
    }

    uint32_t count = (head > uart_rx_read) ? (head - uart_rx_read)
                                           : (UART_RX_RING_SIZE - uart_rx_read);
    if (count > max_len) {
        count = max_len;
    }
    for (uint32_t i = 0U; i < count; i++) {
        dst[i] = uart_rx_ring[(uart_rx_read + i) % UART_RX_RING_SIZE];
    }
    uart_rx_read = (uart_rx_read + count) % UART_RX_RING_SIZE;
    return count;
}

bool app_uart3_rx_running(void)
{
    return huart3.RxState == HAL_UART_STATE_BUSY_RX &&
           hdma_usart3_rx.State == HAL_DMA_STATE_BUSY &&
           (USART3->CR1 & USART_CR1_RE) != 0U;
}

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

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        comms_post_rx_from_isr();
    }
}

void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        comms_post_rx_from_isr();
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART3 || uart_tx_task == NULL) {
        return;
    }

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(uart_tx_task, &woken);
    portYIELD_FROM_ISR(woken);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        comms_post_rx_from_isr();
    }
}

void USART3_IRQHandler(void)
{
    uint32_t sr = USART3->ISR;
    bool wake = false;

    /* Receive errors are cleared here instead of through the HAL, because
     * HAL_UART_IRQHandler() aborts the circular DMA receiver on an overrun.
     * Reading ISR and then RDR clears the error flags; ICR is explicit. */
    if ((sr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0U) {
        volatile uint32_t dr = USART3->RDR;
        (void)dr;
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

void DMA1_Stream1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart3_rx);
}

void DMA1_Stream3_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_usart3_tx);
}
