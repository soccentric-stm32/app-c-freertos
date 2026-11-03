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
    DTCMRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
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
