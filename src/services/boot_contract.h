/**
 * @file    boot_contract.h
 * @brief   Flash layout, hand-over and metadata contract shared with BOOT.
 * @details This header is the single place where the application encodes the
 *          layout it was built for by SRS-BOOT-F767: the slot link address and
 *          size (`LAY-001`..`LAY-005`), the per-bank metadata area with its
 *          Appendix B records, the `RTC_BKP2R` hand-over word (`HND-003`) and
 *          the `RTC_BKP1R` "stay in BOOT" request (`ENT-002`).
 *
 *          BOOT maps the active bank to `0x08000000` with
 *          `SYSCFG_MEMRMP.SWP_FB`, so one image linked at @ref APP_SLOT_BASE
 *          runs unchanged from slot A and from slot B.  The application only
 *          ever reads metadata; the single flash write it performs is the
 *          CONFIRMED record at @ref APP_CONFIRMED_ADDR (`HLT-002`, `DBT-006`,
 *          `DSAFE-002`).
 *
 * @author  Sandesh Ghimire <sandesh@soccentric.com>
 * @date    2026-11-03
 * @target  NUCLEO-F767ZI (STM32F767ZIT6), dual-bank flash (nDBANK = 0)
 * @srs     DBT-001, DBT-004, DBT-005, DBT-006, HLT-002, DSAFE-002
 *
 * @warning Every value here is a binary contract with BOOT.  Changing an
 *          address, a size or a magic word without the matching BOOT change
 *          silently breaks hand-over, trial boot or confirmation.
 * @note    This header defines compile-time constants only.  It allocates no
 *          storage and generates no code.
 * @ingroup boot_contract
 */

#ifndef BOOT_CONTRACT_H
#define BOOT_CONTRACT_H

#include <stdint.h>

/**
 * @defgroup boot_contract BOOT flash and hand-over contract
 * @brief Addresses, sizes and magic words shared with the bootloader.
 * @details The 2 MB flash of the STM32F767ZIT6 is two 1 MB banks.  With
 *          `nDBANK = 0` each bank is four 16 KB sectors, one 64 KB sector and
 *          seven 128 KB sectors (RM0410).  BOOT uses bank sectors 0..1, the
 *          metadata occupies sector 2 and the application slot starts at
 *          sector 3:
 *
 *          0x08000000  +---------------------------+  bank 1 base
 *                      | BOOT, sectors 0..1        |
 *          0x08008000  | metadata, 16 KB (sector 2)|
 *          0x0800C000  | slot A, 976 KB            |
 *          0x08100000  +---------------------------+  bank 2 base
 *                      | same layout, mirror       |
 *          0x08108000  | metadata for slot B       |
 *          0x0810C000  | slot B, 976 KB            |
 *          0x08200000  +---------------------------+
 *
 *          Bank 2 repeats the bank 1 offsets; only the 1 MB bank base
 *          differs.  Keeping the metadata in its own 16 KB sector lets BOOT
 *          erase and program records without touching BOOT or the slot.
 * @see boot_contract.h
 * @see handover.c
 * @see flash_confirm.c
 * @see app_fault.c
 * @{
 */

/**
 * @name Slot geometry and bank layout
 * @brief Link address, slot size and per-bank metadata placement.
 * @{
 */

/**
 * @brief Link address of the application slot in bank 1 (`LAY-002`).
 * @details Absolute address `0x0800C000`, that is 48 KB into the bank,
 *          directly after BOOT (sectors 0..1) and the 16 KB metadata sector.
 *          `main()` copies this value into `SCB->VTOR` (`DBT-002`) and
 *          `ld/app.ld` asserts that the vector table is linked here.
 * @note The same image runs from slot A and slot B because BOOT swaps the
 *       banks, so this is the only link address the application needs.
 * @see APP_SLOT_SIZE
 */
#define APP_SLOT_BASE 0x0800C000UL
/**
 * @brief Size of the application slot in bytes: 976 KB (`DBT-001`).
 * @details `0x000F4000` (999424 bytes), covering `0x0800C000`..`0x080FFFFF`.
 *          It spans bank sectors 3..11 and ends exactly at the 1 MB bank
 *          boundary, so the slot never reaches into bank 2.
 * @note The build fails if the image, including the `.data` load image,
 *       exceeds this size; see `__app_image_end` in `ld/app.ld`.
 */
#define APP_SLOT_SIZE 0x000F4000UL /* 976 KB */

/**
 * @brief Size of one flash bank in bytes: 1 MB.
 * @details `0x00100000`.  The two banks give the 2 MB device its A/B slots.
 *          All metadata and slot offsets below are relative to a bank base,
 *          which is `0x08000000` for bank 1 and `0x08100000` for bank 2.
 * @see APP_META_OFFSET
 * @see APP_SLOT_BASE
 */
#define APP_BANK_SIZE 0x00100000UL
/**
 * @brief Offset of the metadata area from the bank base (`0x8000`).
 * @details 32 KB into the bank, which is the start of the 16 KB metadata
 *          sector (sector 2 in dual-bank mode).  BOOT keeps it outside both
 *          the BOOT region and the slot so records can be erased separately.
 * @see APP_META_SIZE
 */
#define APP_META_OFFSET 0x00008000UL
/**
 * @brief Size of the metadata area in bytes: 16 KB (`0x4000`).
 * @details Exactly one dual-bank flash sector, covering
 *          bank + `0x8000`..bank + `0xBFFF`.  The Appendix B records occupy
 *          only the first `0x180` bytes; the rest stays erased.
 * @note The size matches the hardware sector so BOOT's erase granularity is
 *       one metadata sector.
 */
#define APP_META_SIZE 0x00004000UL

/**
 * @brief Physical metadata address in bank 1: `0x08008000` (`0x08000000` +
 *        @ref APP_META_OFFSET).
 * @details This is the address BOOT uses while it runs, before it applies the
 *          bank swap.  It has the same numeric value as
 *          @ref APP_RUNNING_META_ADDR because bank 1 is normally mapped at
 *          `0x08000000`.
 * @see APP_META_B_ADDR
 */
#define APP_META_A_ADDR 0x08008000UL
/**
 * @brief Physical metadata address in bank 2: `0x08108000` (`0x08100000` +
 *        @ref APP_META_OFFSET).
 * @details Used by BOOT when it inspects or programs the slot-B records
 *          without a bank swap.
 * @see APP_META_A_ADDR
 */
#define APP_META_B_ADDR 0x08108000UL

/**
 * @brief Metadata alias that is valid while the application runs.
 * @details `0x08008000`.  With `SYSCFG_MEMRMP.SWP_FB` set, the running bank
 *          is mapped at `0x08000000`, so the running slot's metadata sector is
 *          always visible at this address, whether the slot is A or B.
 * @warning Do not use this alias to reach the *other* slot's metadata; read
 *          @ref APP_META_A_ADDR or @ref APP_META_B_ADDR instead.
 * @see APP_CONFIRMED_ADDR
 */
#define APP_RUNNING_META_ADDR 0x08008000UL

/** @} */

/**
 * @name Appendix B metadata records
 * @brief Record offsets, record size and magic words in the metadata sector.
 * @details Record offsets are relative to a metadata base
 *          (@ref APP_META_A_ADDR, @ref APP_META_B_ADDR or
 *          @ref APP_RUNNING_META_ADDR).  Each record is a 32-byte slot and the
 *          four state records are spaced `0x20` apart:
 *
 *          offset  state       magic
 *          0x000   header      (BOOT-owned, word 0)
 *          0x100   VERIFIED    0x56455249
 *          0x120   ACTIVATED   0x41435456
 *          0x140   CONFIRMED   0x434F4E46
 *          0x160   REJECTED    0x52454A45
 *
 * @note DEMO writes only the CONFIRMED record; every other record is written
 *       by BOOT and is read-only for the application (`DBT-006`).
 * @{
 */

/**
 * @brief Offset of the metadata header word (`0x000`).
 * @details The header belongs to BOOT; DEMO never reads or writes it and does
 *          not depend on its contents.
 */
#define APP_META_HEADER_OFFSET 0x000U
/**
 * @brief Offset of the VERIFIED record (`0x100`).
 * @details Programmed and consumed by BOOT.  DEMO does not interpret it; it
 *          only relies on BOOT having accepted the image before the jump.
 */
#define APP_META_VERIFIED_OFFSET 0x100U
/**
 * @brief Offset of the ACTIVATED record (`0x120`), `0x100` + one record.
 * @details Part of BOOT's trial/activation state machine; DEMO never writes
 *          it.
 */
#define APP_META_ACTIVATED_OFFSET 0x120U
/**
 * @brief Offset of the CONFIRMED record (`0x140`), `0x120` + one record.
 * @details This is the only record DEMO programs, via
 *          `app_flash_confirm_running_slot()`, after a successful health check
 *          (`HLT-002`).  Its absolute address is @ref APP_CONFIRMED_ADDR.
 * @see APP_META_RECORD_SIZE
 */
#define APP_META_CONFIRMED_OFFSET 0x140U
/**
 * @brief Offset of the REJECTED record (`0x160`), `0x140` + one record.
 * @details Part of BOOT's rollback bookkeeping; DEMO never writes it.
 */
#define APP_META_REJECTED_OFFSET 0x160U
/**
 * @brief Size of one metadata record in bytes: 32 (`0x20`).
 * @details Also the spacing between consecutive record offsets.
 * @note For the CONFIRMED record, `flash_confirm.c` uses word 0 for the magic,
 *       words 1..6 as zeros and word 7 as a CRC-32/ISO-HDLC over the first
 *       28 bytes.  An all-`0xFF` record means "absent"; a torn record is
 *       never overwritten.
 */
#define APP_META_RECORD_SIZE 32U

/**
 * @brief VERIFIED magic: `0x56455249`.
 * @details The ASCII spelling of the constant read from its most significant
 *          byte is "VERI" (`56`='V', `45`='E', `52`='R', `49`='I').
 * @note The word is written little-endian, so the bytes at increasing flash
 *       addresses are `49 52 45 56`, i.e. "IREV".  The contract requires BOOT
 *       to use the same 32-bit constant.
 */
#define APP_META_VERIFIED_MAGIC 0x56455249UL /* "VERI" */
/**
 * @brief ACTIVATED magic: `0x41435456`.
 * @details ASCII spelling of the constant, most significant byte first, is
 *          "ACTV"; in little-endian flash the bytes read "VTCA".
 */
#define APP_META_ACTIVATED_MAGIC 0x41435456UL /* "ACTV" */
/**
 * @brief CONFIRMED magic: `0x434F4E46`.
 * @details ASCII spelling of the constant, most significant byte first, is
 *          "CONF"; in little-endian flash the bytes read "FNOC".  This is the
 *          magic checked by `flash_confirm.c` before it trusts the CRC.
 */
#define APP_META_CONFIRMED_MAGIC 0x434F4E46UL /* "CONF" */
/**
 * @brief REJECTED magic: `0x52454A45`.
 * @details ASCII spelling of the constant, most significant byte first, is
 *          "REJE"; in little-endian flash the bytes read "EJER".
 */
#define APP_META_REJECTED_MAGIC 0x52454A45UL /* "REJE" */

/**
 * @brief Absolute address of the running slot's CONFIRMED record:
 *        `0x08008140` (@ref APP_RUNNING_META_ADDR +
 *        @ref APP_META_CONFIRMED_OFFSET).
 * @details The only address DEMO is allowed to program (`DSAFE-002`,
 *          `DBT-006`).  The helper takes no address argument, so this
 *          constant cannot be redirected at run time.
 * @note The address is 32-byte aligned, as required by
 *       `SCB_InvalidateDCache_by_Addr()`, which `flash_confirm.c` calls
 *       before reading the record because the metadata sector may be cached.
 * @warning Never erase this sector and never program any other metadata
 *          offset from the application; both would destroy BOOT state.
 * @see APP_META_RECORD_SIZE
 */
#define APP_CONFIRMED_ADDR (APP_RUNNING_META_ADDR + APP_META_CONFIRMED_OFFSET)

/** @} */

/**
 * @name Hand-over record (RTC_BKP2R)
 * @brief Bit packing of the word BOOT leaves in backup register 2.
 * @details Written by BOOT before the jump (`HND-003`).  The low 24 bits are
 *          a magic and the upper byte carries the slot and trial state:
 *
 *          bits  0..23  magic `0x444E48` (memory bytes 'H','N','D')
 *          bits 24..25  slot: 1 = A, 2 = B, 0 = unknown
 *          bit  26      trial pending (1 = pending, 0 = confirmed)
 *          bits 27..28  trial counter, 0..3
 *          bits 29..31  not used by DEMO
 *
 *          The packing is private to the BOOT/DEMO pair; BOOT writes exactly
 *          the same word.  `app_handover_read()` decodes it into
 *          `g_handover`.
 * @note An erased or missing record reads as zero, which fails the magic
 *       check and is reported as slot UNKNOWN without confirmation
 *       (`DBT-004`).
 * @note The backup domain is retained by VDD on the NUCLEO-F767ZI (VBAT is
 *       tied to VDD), so the record survives a system reset but not a power
 *       cycle.
 * @warning Do not add fields: the remaining bits are reserved by BOOT.
 * @{
 */

/**
 * @brief Hand-over magic, `0x00444E48`, occupying bits 0..23.
 * @details In little-endian memory the low 24 bits appear as `48 4E 44`,
 *          that is 'H','N','D', with the upper byte reading `0x00`.  Compare
 *          with @ref APP_HANDOVER_MAGIC_MASK, because the upper byte carries
 *          the slot and trial state.
 */
#define APP_HANDOVER_MAGIC 0x00444E48UL /* bytes "HND", 0 */
/**
 * @brief Mask selecting the 24 magic bits of `RTC_BKP2R` (`0x00FFFFFF`).
 * @details The record is considered present only when
 *          `(raw & APP_HANDOVER_MAGIC_MASK) == APP_HANDOVER_MAGIC`.
 */
#define APP_HANDOVER_MAGIC_MASK 0x00FFFFFFUL
/**
 * @brief Right shift of the slot field: 24.
 * @details Combined with @ref APP_HANDOVER_SLOT_MASK to extract bits 24..25.
 */
#define APP_HANDOVER_SLOT_SHIFT 24U
/**
 * @brief Width mask of the slot field: `0x3` (bits 24..25).
 * @details Valid decoded values are @ref APP_SLOT_A and @ref APP_SLOT_B; 0
 *          and 3 are both reported as @ref APP_SLOT_UNKNOWN.
 */
#define APP_HANDOVER_SLOT_MASK 0x3UL
/**
 * @brief Bit position of the "trial pending" flag: 26.
 * @details 1 means BOOT booted the slot on trial; 0 means it is confirmed.
 *          `app_handover_read()` maps 1 to trial state 2 and 0 to state 1.
 */
#define APP_HANDOVER_PENDING_BIT 26U
/**
 * @brief Right shift of the trial counter: 27.
 * @details Combined with @ref APP_HANDOVER_TRIAL_MASK to extract bits 27..28.
 */
#define APP_HANDOVER_TRIAL_SHIFT 27U
/**
 * @brief Width mask of the trial counter: `0x3` (bits 27..28, values 0..3).
 * @details Maintained by BOOT as part of trial boot; DEMO only reports it in
 *          `Status`.
 */
#define APP_HANDOVER_TRIAL_MASK 0x3UL

/** @} */

/**
 * @name Stay-in-BOOT request (RTC_BKP1R)
 * @brief Magic that asks BOOT to stay in its update mode after a reset.
 * @{
 */

/**
 * @brief "Stay in BOOT" magic for `RTC_BKP1R`, `0x59415453` (`ENT-002`).
 * @details In little-endian memory the bytes read `53 54 41 59`, that is
 *          'S','T','A','Y'.  Written by
 *          `app_handover_request_bootloader()` before a reset (`DBT-005`).
 * @note The write needs the backup domain write protection cleared
 *       (`HAL_PWR_EnableBkUpAccess()`), so the helper enables the PWR and RTC
 *       clocks first and then writes the register.
 * @warning Only set this immediately before resetting; leaving the magic in
 *          place may keep the board in BOOT on the next reset as well.
 */
#define APP_STAY_IN_BOOT_MAGIC 0x59415453UL /* bytes "STAY" */

/** @} */

/**
 * @name Reserved no-init window (DTCM)
 * @brief Top 1 KB of DTCM that BOOT never writes and .noinit lives in.
 * @details The window exists because BOOT uses DTCM `0x20000000`..
 *          `0x2000EFFF` (its 60 KB budget, including its stack).  A fault
 *          record placed after `.bss` could therefore be cleared by BOOT on
 *          the next boot.  Instead the top 1 KB of the 128 KB DTCM is
 *          reserved: BOOT never writes it, the application links `.noinit`
 *          there, and its main stack starts just below it, so the record
 *          survives every reset and cannot be overwritten by stack growth.
 * @note `ld/app.ld` asserts that `NOINIT` is `0x2001FC00`, 1 KB long and above
 *       `0x2000F000`; `app_fault.c` statically asserts that the fault record
 *       and the boot id fit in @ref APP_NOINIT_SIZE (`DGEN-004`, A-9).
 * @see app_fault.c
 * @{
 */

/**
 * @brief Start of the reserved no-init window: `0x2001FC00`.
 * @details `0x20000000` + 127 KB, the first byte of the top 1 KB of DTCM.
 *          This is also the initial main stack pointer (`_estack`), so the
 *          stack grows downward, away from the window.
 */
#define APP_NOINIT_ADDR 0x2001FC00UL
/**
 * @brief Size of the reserved no-init window in bytes: 1 KB (`0x400`).
 * @details Holds `g_app_fault` plus the file-scope boot id, both placed in
 *          `.noinit`; the start-up code clears only `.bss`.
 */
#define APP_NOINIT_SIZE 0x00000400UL
/**
 * @brief Exclusive end of the no-init window: `0x20020000`, which is also the
 *        first byte of SRAM1.
 * @details Equals @ref APP_NOINIT_ADDR + @ref APP_NOINIT_SIZE.  The window is
 *          half-open, `[APP_NOINIT_ADDR, APP_NOINIT_END)`.
 */
#define APP_NOINIT_END 0x20020000UL

/** @} */

/**
 * @name Slot identifiers
 * @brief Slot codes shared with the Status reply.
 * @details The values match the hand-over slot field, so the decoded slot can
 *          be reported without translation.  They are also the values used by
 *          `g_handover.slot`.
 * @{
 */

/** No hand-over record, or an unusable slot value (`DBT-004`). */
#define APP_SLOT_UNKNOWN 0U
/** Slot A (hand-over slot field value 1). */
#define APP_SLOT_A 1U
/** Slot B (hand-over slot field value 2). */
#define APP_SLOT_B 2U

/** @} */

/** @} */

#endif /* BOOT_CONTRACT_H */
