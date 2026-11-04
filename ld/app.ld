/*
 * ld/app.ld - demo application linker script (runs under BOOT)
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash, DTCM 128 KB, SRAM1 368 KB,
 *          SRAM2 16 KB
 * SRS:     DCON-003, DBT-001; bootloader LAY-002 / APP-001
 *
 * The bootloader slot starts at bank sector 3. Because BOOT maps the
 * active bank at 0x08000000 (SWP_FB), the same image runs from slot A
 * and slot B, always linked at 0x0800C000.
 */

ENTRY(Reset_Handler)

/* Main/exception stack top: top of DTCM RAM. */
_estack = ORIGIN(DTCMRAM) + LENGTH(DTCMRAM);

MEMORY
{
    /* Slot: bank sectors 3-11, 976 KB (DBT-001). */
    FLASH   (rx)  : ORIGIN = 0x0800C000, LENGTH = 976K
    /* Top 1 KB of DTCM is the reserved no-init window (APP_NOINIT_*). */
    DTCMRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 127K
    NOINIT  (rw)  : ORIGIN = 0x2001FC00, LENGTH =   1K
    SRAM1   (rwx) : ORIGIN = 0x20020000, LENGTH = 368K
    SRAM2   (rwx) : ORIGIN = 0x2007C000, LENGTH =  16K
}

/* No libc heap: FreeRTOS heap_4 is static (DRT-001). */
_Min_Heap_Size  = 0;
/* MSP stack: start-up code and interrupts once the scheduler runs. */
_Min_Stack_Size = 0x1000;

SECTIONS
{
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text .text.*)
        *(.rodata .rodata.*)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > FLASH

    .ARM.exidx :
    {
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > DTCMRAM AT > FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DTCMRAM

    /* Fault record survives a system reset (DGEN-004).  Start-up code must
     * not clear this section; only .bss is cleared. */
    .noinit (NOLOAD) :
    {
        . = ALIGN(4);
        _snoinit = .;
        KEEP(*(.noinit .noinit.*))
        . = ALIGN(4);
        _enoinit = .;
    } > NOINIT

    /* DMA buffers are in SRAM2 and covered by the non-cacheable MPU region
     * set up in cache_mpu.c (COM-002). */
    .dma_buffer (NOLOAD) :
    {
        . = ALIGN(32);
        _sdma_buffer = .;
        KEEP(*(.dma_buffer .dma_buffer.*))
        . = ALIGN(32);
        _edma_buffer = .;
    } > SRAM2

    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > DTCMRAM
}

/* DBT-001: the whole image (including the .data load image) stays inside
 * the 976 KB slot. The build fails otherwise. */
__app_image_end = LOADADDR(.data) + SIZEOF(.data);
ASSERT(ORIGIN(FLASH) == 0x0800C000,
       "DBT-001: the vector table must be at the slot start 0x0800C000")
ASSERT(__app_image_end <= 0x08100000,
       "DBT-001: image exceeds the 976 KB bootloader slot")
ASSERT(ORIGIN(NOINIT) == 0x2001FC00 && LENGTH(NOINIT) == 1K,
       "A-9: .noinit window must match APP_NOINIT_ADDR/SIZE")
ASSERT(ORIGIN(NOINIT) >= 0x2000F000,
       "A-9: .noinit overlaps BOOT's 60 KB DTCM budget")
ASSERT(_enoinit <= ORIGIN(NOINIT) + LENGTH(NOINIT),
       "A-9: .noinit exceeds the reserved window")
ASSERT(SIZEOF(.dma_buffer) <= LENGTH(SRAM2),
       "COM-002: DMA buffers do not fit in SRAM2")
