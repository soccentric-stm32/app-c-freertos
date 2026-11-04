/*
 * boot_contract.h - flash layout, hand-over and metadata contract with BOOT
 *
 * Author:  Sandesh Ghimire <sandesh@soccentric.com>
 * Date:    2026-11-03
 * Target:  STM32F767ZIT6, dual-bank flash (nDBANK = 0)
 * SRS:     DBT-001, DBT-004, DBT-005, DBT-006, HLT-002, DSAFE-002
 *
 * The values in this file mirror SRS-BOOT-F767 LAY-001..LAY-005, HND-003 and
 * Appendix B.  DEMO only ever reads the slot metadata and programs the one
 * CONFIRMED record.
 */

#ifndef BOOT_CONTRACT_H
#define BOOT_CONTRACT_H

#include <stdint.h>

/* LAY-002: every application is linked at the slot offset in bank 1.  BOOT
 * maps the active bank at 0x08000000 with SYSCFG_MEMRMP.SWP_FB, so the same
 * image runs from slot A and slot B. */
#define APP_SLOT_BASE           0x0800C000UL
#define APP_SLOT_SIZE           0x000F4000UL   /* 976 KB */

#define APP_BANK_SIZE           0x00100000UL
#define APP_META_OFFSET         0x00008000UL
#define APP_META_SIZE           0x00004000UL

/* Physical addresses without bank swap (what BOOT uses while it runs). */
#define APP_META_A_ADDR         0x08008000UL
#define APP_META_B_ADDR         0x08108000UL

/* While an application runs, SYSCFG_MEMRMP.SWP_FB maps its own bank to
 * 0x08000000, so its metadata sector is always visible at this alias. */
#define APP_RUNNING_META_ADDR   0x08008000UL

/* Appendix B record offsets and magic words (little-endian). */
#define APP_META_HEADER_OFFSET    0x000U
#define APP_META_VERIFIED_OFFSET  0x100U
#define APP_META_ACTIVATED_OFFSET 0x120U
#define APP_META_CONFIRMED_OFFSET 0x140U
#define APP_META_REJECTED_OFFSET  0x160U
#define APP_META_RECORD_SIZE      32U

#define APP_META_VERIFIED_MAGIC   0x56455249UL  /* "VERI" */
#define APP_META_ACTIVATED_MAGIC  0x41435456UL  /* "ACTV" */
#define APP_META_CONFIRMED_MAGIC  0x434F4E46UL  /* "CONF" */
#define APP_META_REJECTED_MAGIC   0x52454A45UL  /* "REJE" */

#define APP_CONFIRMED_ADDR \
    (APP_RUNNING_META_ADDR + APP_META_CONFIRMED_OFFSET)

/* Hand-over record in RTC_BKP2R (HND-003).  The low 24 bits are the magic
 * and the upper byte carries the slot and trial state:
 *
 *   bits  0..23  magic 0x444E48 (the bytes "HND")
 *   bits 24..25  slot: 1 = A, 2 = B, 0 = unknown
 *   bit  26      trial pending
 *   bits 27..28  trial counter (0..3)
 *
 * The exact packing is private to the BOOT/DEMO pair; BOOT writes the same
 * word.  A missing record (no BOOT) reads as erased/zero and is reported as
 * slot UNKNOWN (DBT-004). */
#define APP_HANDOVER_MAGIC        0x00444E48UL  /* bytes "HND", 0 */
#define APP_HANDOVER_MAGIC_MASK   0x00FFFFFFUL
#define APP_HANDOVER_SLOT_SHIFT   24U
#define APP_HANDOVER_SLOT_MASK    0x3UL
#define APP_HANDOVER_PENDING_BIT  26U
#define APP_HANDOVER_TRIAL_SHIFT  27U
#define APP_HANDOVER_TRIAL_MASK   0x3UL

/* RTC_BKP1R "stay in BOOT" request (ENT-002). */
#define APP_STAY_IN_BOOT_MAGIC    0x59415453UL  /* bytes "STAY" */

/* Reserved no-init window (DGEN-004, A-9).  BOOT uses DTCM
 * 0x20000000..0x2000EFFF (its 60 KB budget, including its stack), so an
 * application fault record placed after .bss could be cleared by BOOT.
 * The top 1 KB of DTCM is reserved instead: BOOT never writes it, the
 * application links .noinit there and its main stack starts just below
 * it, so the record survives every reset and cannot be overwritten by
 * the stack.  The linker script and app_fault.c assert these values. */
#define APP_NOINIT_ADDR           0x2001FC00UL
#define APP_NOINIT_SIZE           0x00000400UL
#define APP_NOINIT_END            0x20020000UL

/* Slot identifiers shared with Status. */
#define APP_SLOT_UNKNOWN          0U
#define APP_SLOT_A                1U
#define APP_SLOT_B                2U

#endif /* BOOT_CONTRACT_H */
