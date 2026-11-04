/*
 * app_fault.c - no-init fault record and fatal paths
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6
 * SRS:     DGEN-004, DRT-002, LED-005
 *
 * The fault record is written before resetting and is reported in Status
 * after the reset.  The fatal path works before the HAL, before the GPIO
 * driver and with interrupts disabled, so it programs the LED port with
 * register accesses only.
 */

#include "stm32f7xx_hal.h"
#include "app_board.h"
#include "app_fault.h"
#include "boot_contract.h"

app_fault_record_t g_app_fault __attribute__((section(".noinit")));
static uint32_t app_boot_id __attribute__((section(".noinit")));

/* A-9: the record and the boot id must fit the window that BOOT leaves
 * alone.  The linker script places .noinit at APP_NOINIT_ADDR. */
_Static_assert(sizeof(app_fault_record_t) + sizeof(uint32_t) <= APP_NOINIT_SIZE,
               "A-9: fault record exceeds the reserved no-init window");

static const char *const fault_reason_names[] = {
    "none",
    "hard_fault",
    "mem_manage",
    "bus_fault",
    "usage_fault",
    "assert",
    "stack_overflow",
    "malloc_failed",
    "startup"
};

static void fault_copy(char *dst, uint32_t dst_size, const char *src)
{
    uint32_t i = 0U;

    if (dst_size == 0U) {
        return;
    }
    if (src != NULL) {
        /* __FILE__ may be an absolute build path.  Keep the basename so the
         * 31-character field identifies the file (DGEN-004). */
        const char *base = src;
        for (const char *p = src; *p != '\0'; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        while ((i + 1U) < dst_size && base[i] != '\0') {
            dst[i] = base[i];
            i++;
        }
    }
    dst[i] = '\0';
}

void app_fault_boot_start(void)
{
    app_boot_id++;
    if (app_boot_id == 0U) {
        app_boot_id = 1U;
    }
}

bool app_fault_is_valid(void)
{
    return g_app_fault.magic == APP_FAULT_MAGIC &&
           g_app_fault.reason <= (uint32_t)APP_FAULT_STARTUP;
}

bool app_fault_is_current_boot(void)
{
    return app_fault_is_valid() && g_app_fault.boot_id == app_boot_id;
}

bool app_fault_is_previous_boot(void)
{
    uint32_t previous = app_boot_id - 1U;
    if (previous == 0U) {
        previous = 0xFFFFFFFFUL;
    }
    return app_fault_is_valid() && g_app_fault.boot_id == previous;
}

const char *app_fault_reason_text(app_fault_reason_t reason)
{
    if ((uint32_t)reason >= (sizeof(fault_reason_names) /
                             sizeof(fault_reason_names[0]))) {
        return "unknown";
    }
    return fault_reason_names[reason];
}

const char *app_fault_task(void)
{
    return g_app_fault.task;
}

const char *app_fault_file(void)
{
    return g_app_fault.file;
}

void app_fault_save(app_fault_reason_t reason, uint32_t cfsr, uint32_t hfsr,
                    uint32_t mmfar, uint32_t bfar, uint32_t pc, uint32_t lr,
                    uint32_t psr, const char *task, const char *file,
                    uint32_t line)
{
    /* Invalidate first so a reset in the middle of the copy is not mistaken
     * for a complete record. */
    g_app_fault.magic = 0U;
    __DMB();

    g_app_fault.reason = (uint32_t)reason;
    g_app_fault.cfsr = cfsr;
    g_app_fault.hfsr = hfsr;
    g_app_fault.mmfar = mmfar;
    g_app_fault.bfar = bfar;
    g_app_fault.pc = pc;
    g_app_fault.lr = lr;
    g_app_fault.psr = psr;
    g_app_fault.line = line;
    g_app_fault.boot_id = app_boot_id;
    fault_copy(g_app_fault.task, sizeof(g_app_fault.task), task);
    fault_copy(g_app_fault.file, sizeof(g_app_fault.file), file);

    __DMB();
    g_app_fault.magic = APP_FAULT_MAGIC;
    __DSB();
}

void app_fault_clear(void)
{
    g_app_fault.magic = 0U;
    __DMB();
}

static void fault_delay_ms(uint32_t ms)
{
    /* Use the DWT cycle counter when it is available; this works before the
     * HAL and gives the same delay at 16 MHz and 216 MHz.  Fall back to a
     * calibrated loop if the trace unit is locked. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* If the trace unit is locked, CYCCNT never advances; detect that once
     * instead of spinning for cycles*4 iterations (the IWDG would fire
     * first). */
    uint32_t probe = DWT->CYCCNT;
    for (volatile uint32_t i = 0U; i < 100U; i++) {
        __NOP();
    }
    if (DWT->CYCCNT == probe) {
        for (volatile uint32_t i = 0U; i < (ms * 20000U); i++) {
            __NOP();
        }
        return;
    }

    uint32_t cycles = (SystemCoreClock / 1000U) * ms;
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles) {
        __NOP();
    }
}

static void fault_led3(bool on)
{
    GPIOB->BSRR = on ? APP_LED3_PIN : ((uint32_t)APP_LED3_PIN << 16U);
}

void app_fault_show_and_reset(void)
{
    __disable_irq();

    /* Register-level LED set-up: DGEN-004 must work from any context. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    __DSB();
    uint32_t moder = GPIOB->MODER;
    moder &= ~(3UL << (14U * 2U));
    moder |= (1UL << (14U * 2U));
    GPIOB->MODER = moder;
    __DSB();

    /* Three short flashes, a pause, repeat; roughly 2 s in total. */
    for (uint32_t cycle = 0U; cycle < 2U; cycle++) {
        for (uint32_t flash = 0U; flash < 3U; flash++) {
            fault_led3(true);
            fault_delay_ms(100U);
            fault_led3(false);
            fault_delay_ms(100U);
        }
        fault_delay_ms(500U);
    }

    NVIC_SystemReset();
    for (;;) {
        __NOP();
    }
}

void app_fault_fatal(app_fault_reason_t reason, const char *task,
                     const char *file, uint32_t line)
{
    app_fault_save(reason, 0U, 0U, 0U, 0U, 0U, 0U, 0U, task, file, line);
    app_fault_show_and_reset();
}

/* Called from the naked exception wrappers with r0 = stacked frame and
 * r1 = app_fault_reason_t. */
__attribute__((used, noreturn))
void app_fault_handler_c(uint32_t *frame, uint32_t reason)
{
    /* A stack-overflow fault can leave MSP/PSP outside RAM; dereferencing
     * such a frame double-faults into lockup.  Only read the stacked PC/LR
     * when the whole exception frame lies in internal RAM. */
    uint32_t pc = 0U, lr = 0U, psr = 0U;
    uintptr_t base = (uintptr_t)frame;
    if (base >= 0x20000000UL && base <= 0x20080000UL &&
        (base & 3U) == 0U && (base + (8U * 4U)) <= 0x20080000UL) {
        pc  = frame[6];
        lr  = frame[5];
        psr = frame[7];
    }
    app_fault_save((app_fault_reason_t)reason,
                   SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR,
                   pc, lr, psr, NULL, NULL, 0UL);
    app_fault_show_and_reset();
}

#define APP_DEFINE_FAULT_HANDLER(name, reason)                              \
    __attribute__((naked, noreturn)) void name(void)                        \
    {                                                                       \
        __asm volatile (                                                    \
            "tst lr, #4\n\t"                                                \
            "ite eq\n\t"                                                    \
            "mrseq r0, msp\n\t"                                             \
            "mrsne r0, psp\n\t"                                             \
            "movs r1, %0\n\t"                                               \
            "b app_fault_handler_c\n\t"                                     \
            : : "I"(reason) : "r0", "r1", "memory");                        \
    }

APP_DEFINE_FAULT_HANDLER(HardFault_Handler,  APP_FAULT_HARDFAULT)
APP_DEFINE_FAULT_HANDLER(MemManage_Handler,  APP_FAULT_MEMMANAGE)
APP_DEFINE_FAULT_HANDLER(BusFault_Handler,   APP_FAULT_BUSFAULT)
APP_DEFINE_FAULT_HANDLER(UsageFault_Handler, APP_FAULT_USAGEFAULT)
