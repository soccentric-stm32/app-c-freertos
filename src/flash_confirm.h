/*
 * flash_confirm.h - program the single CONFIRMED metadata record
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash
 * SRS:     HLT-002, HLT-005, DBT-006, DSAFE-002
 */

#ifndef FLASH_CONFIRM_H
#define FLASH_CONFIRM_H

#include <stdbool.h>

/* Program the CONFIRMED record for the running slot if it is absent.
 * Returns true when the record is present after the call.  This is the only
 * flash write DEMO performs; the address is fixed by boot_contract.h and no
 * erase is ever issued. */
bool app_flash_confirm_running_slot(void);

#endif /* FLASH_CONFIRM_H */
