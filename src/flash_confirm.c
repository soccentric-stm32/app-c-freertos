/*
 * flash_confirm.c - program the single CONFIRMED metadata record
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash
 * SRS:     HLT-002, HLT-005, DBT-006, DSAFE-002
 *
 * Appendix B defines a 32-byte record area.  We use the first 28 bytes for
 * the record (magic, zeros) and the last word for CRC-32/ISO-HDLC over those
 * 28 bytes.  A missing record is all 0xFF; a torn record is any other value
 * without a valid magic/CRC and is never overwritten.
 */

#include "stm32f7xx_hal.h"
#include "boot_contract.h"
#include "crc32.h"
#include "flash_confirm.h"

#define CONFIRMED_WORDS (APP_META_RECORD_SIZE / 4U)

static bool record_valid(const volatile uint32_t *record)
{
    if (record[0] != APP_META_CONFIRMED_MAGIC) {
        return false;
    }
    return app_crc32((const void *)record, APP_META_RECORD_SIZE - 4U) ==
           record[CONFIRMED_WORDS - 1U];
}

static bool record_erased(const volatile uint32_t *record)
{
    for (uint32_t i = 0U; i < CONFIRMED_WORDS; i++) {
        if (record[i] != 0xFFFFFFFFUL) {
            return false;
        }
    }
    return true;
}

bool app_flash_confirm_running_slot(void)
{
    /* DSAFE-002: the helper has no caller-supplied address.  The address is
     * the CONFIRMED record of the running slot under the BOOT bank swap. */
    const uint32_t address = APP_CONFIRMED_ADDR;
    volatile uint32_t *record = (volatile uint32_t *)address;

    /* The metadata sector may be cached by the D-cache; make sure reads see
     * what is actually in flash. */
    SCB_InvalidateDCache_by_Addr((uint32_t *)address, APP_META_RECORD_SIZE);

    if (record_valid(record)) {
        return true;
    }
    if (!record_erased(record)) {
        return false;
    }

    uint32_t words[CONFIRMED_WORDS];
    for (uint32_t i = 0U; i < CONFIRMED_WORDS; i++) {
        words[i] = 0U;
    }
    words[0] = APP_META_CONFIRMED_MAGIC;
    words[CONFIRMED_WORDS - 1U] =
        app_crc32((const void *)words, APP_META_RECORD_SIZE - 4U);

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_ALL_ERRORS);

    bool ok = true;
    for (uint32_t i = 0U; i < CONFIRMED_WORDS; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + (i * 4U),
                              words[i]) != HAL_OK) {
            ok = false;
            break;
        }
        SCB_InvalidateDCache_by_Addr((uint32_t *)address,
                                     APP_META_RECORD_SIZE);
        if (*(volatile uint32_t *)(address + (i * 4U)) != words[i]) {
            ok = false;
            break;
        }
    }

    HAL_FLASH_Lock();
    SCB_InvalidateDCache_by_Addr((uint32_t *)address, APP_META_RECORD_SIZE);
    return ok && record_valid(record);
}
