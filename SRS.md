# Software Requirements Specification — NUCLEO-F767ZI FreeRTOS Demo Application

| | |
|---|---|
| **Document** | SRS-DEMO-F767 |
| **Version** | 0.1 (draft) |
| **Author** | Sandesh Ghimire <sandesh@soccentric.com> |
| **Date** | 2026-11-03 |
| **Status** | Draft for review |
| **Hardware** | NUCLEO-F767ZI, STM32F767ZIT6 rev Z (see [validation-firmware/Hardware.md](https://github.com/soccentric-stm32/validation-firmware/blob/HEAD/Hardware.md)) |
| **Software base** | FreeRTOS V10.2.0 and HAL/LL from STM32CubeF7 v1.17.4 as shipped (same as HWVAL); flatcc runtime for FlatBuffers |

---

## Contents

1. [Introduction](#1-introduction)
2. [Overall description](#2-overall-description)
3. [General requirements](#3-general-requirements)
4. [Bootloader contract](#4-bootloader-contract)
5. [FreeRTOS requirements](#5-freertos-requirements)
6. [LED requirements](#6-led-requirements)
7. [Button requirements](#7-button-requirements)
8. [Communication requirements](#8-communication-requirements)
9. [Health and confirmation](#9-health-and-confirmation)
10. [Safety requirements](#10-safety-requirements)
11. [Non-functional requirements](#11-non-functional-requirements)
12. [Verification matrix](#12-verification-matrix)
- [Appendix A — LED patterns](#appendix-a--led-patterns)
- [Appendix B — Open items](#appendix-b--open-items)

---

## 1. Introduction

### 1.1 Purpose

This document specifies **DEMO**, a small FreeRTOS application for the
NUCLEO-F767ZI. DEMO blinks the on-board LEDs and is controlled from the host
GUI over UART. Its purpose is to be a reference "standard application" that
proves the whole chain works: SWD provisioning → bootloader → A/B update →
application → GUI.

### 1.2 Scope

DEMO shall:

- run under the A/B bootloader (SRS-BOOT-F767) from either slot;
- blink LD1/LD2/LD3 in selectable patterns;
- let the user button B1 cycle through patterns;
- speak HWLINK over the ST-LINK VCP so the GUI can read status, control the
  LEDs and send the board to the bootloader;
- confirm itself to the bootloader after a health check.

DEMO is a template, not a product: it has no networking, storage, or
peripherals other than LEDs, button, UART, IWDG, RTC backup registers and
flash (metadata confirmation only).

### 1.3 Definitions

| Term | Meaning |
|---|---|
| **DEMO** | The application specified here |
| **BOOT** | The A/B bootloader, SRS-BOOT-F767 |
| **HWLINK** | UART framing and FlatBuffers protocol ([LINK]) |
| **Pattern** | A named LED behaviour (Appendix A) |
| **Pending / Confirmed** | Bootloader trial states ([BOOT] §7) |
| **Hand-over record** | `RTC_BKP2R` written by BOOT ([BOOT] HND-003) |

### 1.4 Requirement conventions

As in [BOOT] §1.4: **shall / should / may**, IDs `<AREA>-<NNN>`, and methods
**T** (test), **D** (demonstration), **I** (inspection), **A** (analysis).

### 1.5 References

| ID | Document |
|---|---|
| [HW] | [validation-firmware/Hardware.md](https://github.com/soccentric-stm32/validation-firmware/blob/HEAD/Hardware.md) — board reference (LEDs §9, button) |
| [HWVAL] | [validation-firmware/SRS.md](https://github.com/soccentric-stm32/validation-firmware/blob/HEAD/SRS.md) — HWVAL, for shared build and board conventions |
| [BOOT] | [bootloader/SRS.md](https://github.com/soccentric-stm32/bootloader/blob/HEAD/SRS.md) — SRS-BOOT-F767 |
| [GUI] | [validation-gui/SRS.md](https://github.com/soccentric-stm32/validation-gui/blob/HEAD/SRS.md) — SRS-GUI-F767 |
| [LINK] | [validation-gui/SRS.md, Appendix A](https://github.com/soccentric-stm32/validation-gui/blob/HEAD/SRS.md#appendix-a--hwlink-protocol) — HWLINK protocol (normative) |
| [FRTOS] | FreeRTOS V10.2.0 reference manual |
| [RM0410] | ST, STM32F76xxx/F77xxx reference manual |
| [UM1974] | ST, STM32 Nucleo-144 boards user manual |

---

## 2. Overall description

### 2.1 Product perspective

```
 Host PC                                  NUCLEO-F767ZI
+-----------------+   ST-LINK VCP    +--------------------------------------+
| HWVAL GUI       |<================>| USART3 ─> Comms task ──> LED task    |
|  demo page:     |  HWLINK frames   |                 │          │        |
|  status, LEDs,  |                  |                 v          v        |
|  "update" ──────|──> bootloader    |            Health task   LD1 LD2 LD3 |
+-----------------+                  |                 │                    |
                                     |   B1 ──> Button task ──> LED task    |
                                     |   IWDG <── Health task               |
                                     |   (running from slot A or B, BOOT)   |
                                     +--------------------------------------+
```

### 2.2 Users

| User | Use |
|---|---|
| Firmware developer | Starting point for new applications; A/B update and rollback testing |
| Test technician | Visible check that a board runs a confirmed application |
| GUI | Controls LEDs, reads status, triggers updates |

### 2.3 Constraints

| ID | Constraint |
|---|---|
| DCON-001 | DEMO shall be built with the arm-none-eabi GCC toolchain in the project Docker image with one CMake command, like HWVAL. |
| DCON-002 | DEMO shall use FreeRTOS V10.2.0 and HAL/LL from STM32CubeF7 v1.17.4 unmodified (same versions as HWVAL); flatcc as released. |
| DCON-003 | DEMO shall run only under BOOT, linked at `0x0800 C000` ([BOOT] LAY-002). It is not intended to be flashed alone at `0x0800 0000`. |
| DCON-004 | DEMO shall use USART3 (PD8/PD9) on the ST-LINK VCP for HWLINK. |
| DCON-005 | DEMO shall never reconfigure the SWD pins (PA13, PA14) or HSE input (PH0, PH1). |

### 2.4 Assumptions

| ID | Assumption |
|---|---|
| DASM-001 | The board is provisioned for BOOT ([BOOT] BASM-001) and BOOT is installed. |
| DASM-002 | LD1 is on PB0 (default solder bridge), LD2 on PB7, LD3 on PB14; all high = on. B1 is on PC13, high when pressed [HW §9]. |
| DASM-003 | The ST-LINK VCP works in both directions ([BOOT] BASM-004). |

---

## 3. General requirements

| ID | Requirement | Method |
|---|---|---|
| DGEN-001 | The build shall produce `demo.elf`, `demo.bin`, `demo.hex`, `demo.map` and the bootloader image file `demo.img` made by `mkimage.py` ([BOOT] BHOST-001). | I |
| DGEN-002 | DEMO shall embed a semantic version, git commit and build date; the same values shall go into the image header. | I, T |
| DGEN-003 | DEMO shall run the core at 216 MHz from the PLL fed by the 8 MHz HSE bypass clock, with voltage scale 1, over-drive, 7 flash wait states, ART, prefetch, I-cache and D-cache on (same clock configuration as HWVAL GEN-004). | T |
| DGEN-004 | DEMO shall install HardFault, MemManage, BusFault and UsageFault handlers that save CFSR, HFSR, MMFAR, BFAR and the stacked PC/LR to a no-init RAM area, show the fault pattern on LD3 for 2 s, and reset. The saved fault shall be reported in `Status` after the reset. | T |
| DGEN-005 | DEMO shall read the reset cause from `RCC_CSR` at start-up, keep it for `Status`, and then clear the flags. | T |
| DGEN-006 | The HAL time base shall use a hardware timer (not SysTick, which FreeRTOS owns). | I |
| DGEN-007 | DEMO should share its board, clock and fault-handler code with HWVAL through a common source directory instead of copying it. | I |
| DGEN-008 | All source files shall carry the author header of the project. | I |

---

## 4. Bootloader contract

DEMO shall meet [BOOT] §11 APP-001…APP-007. In DEMO terms:

| ID | Requirement | Method |
|---|---|---|
| DBT-001 | The linker script shall place the vector table at `0x0800 C000` and fail the build if the image exceeds 976 KB. | I |
| DBT-002 | The first instructions of `main` shall set `SCB->VTOR` to `0x0800 C000` (BOOT also sets it) and refresh the IWDG. | I, T |
| DBT-003 | DEMO shall configure its clocks from the reset state left by BOOT ([BOOT] HND-001). | T |
| DBT-004 | DEMO shall read the hand-over record from `RTC_BKP2R` and report the running slot (A/B) and trial state in `Status`. If the record is missing (no BOOT), DEMO shall report slot `UNKNOWN` and skip confirmation. | T |
| DBT-005 | `EnterBootloader` shall write the "stay in BOOT" magic to `RTC_BKP1R`, send `Ack`, wait until the UART has sent it, and reset ([BOOT] ENT-002). | T |
| DBT-006 | DEMO shall never write option bytes, never write bank 1 sectors 0–1, the other bank, or any slot sector. The only flash write DEMO performs is the `CONFIRMED` record (§9). | I |

---

## 5. FreeRTOS requirements

### 5.1 Configuration

| ID | Requirement | Method |
|---|---|---|
| DRT-001 | DEMO shall use the FreeRTOS `ARM_CM7` GCC port with the native API, a 1 kHz tick from SysTick, and `heap_4`. | I |
| DRT-002 | `configASSERT`, stack overflow checking (method 2) and the malloc-failed hook shall be enabled. Each shall record the reason (file/line, task name or free heap) in the no-init fault area and reset, as in DGEN-004. | T |
| DRT-003 | Interrupt priorities shall respect `configMAX_SYSCALL_INTERRUPT_PRIORITY`. | I |
| DRT-004 | All tasks, queues and timers shall be created before the scheduler starts; DEMO shall not allocate memory after start-up. | I |

### 5.2 Tasks

| Task | Priority | Job |
|---|---|---|
| **Health** | highest | Refreshes the IWDG when all other tasks have checked in; runs the health check and writes `CONFIRMED` (§9) |
| **Comms** | high | Receives HWLINK frames, executes commands, sends replies and events |
| **Button** | normal | Debounces B1 and sends events to the LED task |
| **LED** | normal | Runs the current pattern on LD1/LD2/LD3 |

| ID | Requirement | Method |
|---|---|---|
| DRT-010 | DEMO shall have exactly the four tasks above plus the FreeRTOS idle and timer tasks. | I |
| DRT-011 | Tasks shall communicate through queues or task notifications only; no task shall write another task's state directly. | I |
| DRT-012 | Every task except Health shall check in with Health at least once per second. Health shall stop refreshing the IWDG when any task misses its check-in for 2 s, so that the board resets ([BOOT] TRY-004). | T |
| DRT-013 | `Status` shall report each task's stack high-water mark and the free and minimum-ever-free heap. | T |

---

## 6. LED requirements

| ID | Requirement | Method |
|---|---|---|
| LED-001 | DEMO shall support the patterns of Appendix A on LD1 (green) and LD2 (blue). | D |
| LED-002 | The default pattern after reset shall be `HEARTBEAT` on LD1. | D |
| LED-003 | The GUI shall be able to set the pattern per LED, and the period from 50 ms to 5000 ms in 10 ms steps, with `SetLed` ([LINK]). Out-of-range values shall be answered with `Nak BAD_ARGUMENT`. | T |
| LED-004 | The GUI shall be able to set all LEDs at once with `SetLedAll` (a pattern and period for LD1+LD2). | T |
| LED-005 | LD3 (red) shall be reserved for errors: on solid when the last reset was caused by a fault or watchdog (until `ClearFault`), and fast blink while the slot is pending and the health check has failed. LD3 shall not be settable by `SetLed` except with the `force` flag. | T, D |
| LED-006 | LED timing shall be generated by FreeRTOS delays or a software timer, with jitter below 2 ms at a 100 ms period. | T |
| LED-007 | The pattern settings shall be kept in RAM only; they return to defaults after reset. | T |

---

## 7. Button requirements

| ID | Requirement | Method |
|---|---|---|
| BTN-001 | B1 shall be read through EXTI line 13 with software debounce of 30 ms. | T |
| BTN-002 | A short press (< 1 s) shall select the next pattern for LD1 and LD2 in the order of Appendix A. | D |
| BTN-003 | A long press (≥ 2 s) shall restore the default patterns (LED-002). | D |
| BTN-004 | Each press shall send an unsolicited `ButtonEvent` (short/long, new pattern) to the GUI when a link is open. | T |

---

## 8. Communication requirements

DEMO implements the HWLINK framing and common messages of [LINK] and the
demo message set below.

### 8.1 Messages

| Request (host → DEMO) | Response / event (DEMO → host) | Purpose |
|---|---|---|
| `Hello` | `HelloReply` (role `DEMO`, versions, UID, protocol version) | Identify the firmware |
| `Ping` | `Pong` | Link check |
| `GetStatus` | `Status` | Version, uptime, slot, trial state, reset cause, last fault, LED patterns, task and heap stats |
| `SetLed` (led, pattern, period, force) | `Ack` / `Nak` | Control one LED |
| `SetLedAll` (pattern, period) | `Ack` / `Nak` | Control LD1+LD2 |
| `ClearFault` | `Ack` | Clear the saved fault and LD3 |
| `EnterBootloader` | `Ack`, then reset | Go to BOOT for an update |
| `Reset` | `Ack`, then reset | Software reset |
| — | `ButtonEvent` | Unsolicited, on B1 press |
| — | `Log` | Unsolicited, optional diagnostic text |

### 8.2 Requirements

| ID | Requirement | Method |
|---|---|---|
| COM-001 | USART3 shall run at 115200 baud 8N1 by default; `SetBaud` ([LINK]) shall allow 921600 with revert to 115200 if no valid frame arrives within 5 s at the new rate. | T |
| COM-002 | Reception shall be interrupt- or DMA-driven into a ring buffer of at least 2 KB; transmission shall be DMA- or interrupt-driven and shall not block other tasks. | T |
| COM-003 | Every request shall be answered within 50 ms (except `Reset`/`EnterBootloader`, which reset after the `Ack`). | T |
| COM-004 | Every received FlatBuffer shall pass the flatcc verifier before it is used; failures shall be answered with `Nak MALFORMED`. | T |
| COM-005 | Unknown message types shall be answered with `Nak UNSUPPORTED`, without state change. | T |
| COM-006 | `EnterBootloader` shall follow DBT-005. | T |
| COM-007 | DEMO shall not print free text on USART3; all output shall be HWLINK frames (the `Log` event carries text). | I |

---

## 9. Health and confirmation

| ID | Requirement | Method |
|---|---|---|
| HLT-001 | When the hand-over record says the slot is pending, Health shall run the health check: all four tasks have checked in at least 5 times, no assert or fault since start, free heap ≥ 4 KB, and the HWLINK receiver is running. | T |
| HLT-002 | The health check shall pass no earlier than 5 s and no later than 10 s after start-up. On pass, Health shall program the `CONFIRMED` record into the metadata sector of the running slot (`0x0800 8000` + record offset, [BOOT] Appendix B) using the shared flash helper ([BOOT] LAY-005). | T |
| HLT-003 | On fail, DEMO shall not confirm, shall show LD3 fast blink, report `HEALTH_FAILED` in `Status`, and stop refreshing the IWDG after 10 s, so that BOOT counts a failed trial. | T |
| HLT-004 | A build option `DEMO_FAIL_HEALTH` shall make the health check always fail, so that rollback can be tested ([BOOT] BNFR-004). | T |
| HLT-005 | When the slot is already confirmed, DEMO shall not write flash. | T |

---

## 10. Safety requirements

| ID | Requirement | Method |
|---|---|---|
| DSAFE-001 | DEMO shall never unlock or write option bytes; code that touches `OPTKEYR` shall not be compiled in. | I |
| DSAFE-002 | The flash helper used by DEMO shall accept only the `CONFIRMED` record address of the running slot and shall reject every other address. | I, T |
| DSAFE-003 | DEMO shall drive only PB0, PB7, PB14 (LEDs), read PC13 (B1), use PD8/PD9 (USART3) and PG14 (USART6 debug TX, DLOG-001). | I |

---

## 11. Non-functional requirements

| ID | Requirement | Method |
|---|---|---|
| DNFR-001 | **Footprint:** DEMO shall be under 128 KB of flash and 64 KB of RAM; the build shall print both. | I |
| DNFR-002 | **Start-up:** the LED pattern shall be visible within 300 ms of hand-over. | T |
| DNFR-003 | **Reliability:** DEMO shall run 24 h with the GUI polling `GetStatus` every 100 ms without a reset or a missed reply. | T |
| DNFR-004 | **Code quality:** `-Wall -Wextra -Werror` and cppcheck with no high-severity findings. | I |
| DNFR-005 | **Template:** a README shall explain how to make a new application from DEMO (rename, add tasks, keep the bootloader contract). | I |

### Debug output (USART6)

USART3 carries HWLINK only.  Debug lines go to USART6 TX on PG14 (Arduino
D1), 115200 8N1, in the format BOOT uses: `[<ms>] <TAG> <E|W|I|D> <text>` (milliseconds since reset, right-aligned to 7 digits, CR LF), tag `C-FRT`.

| ID | Requirement | Method |
|---|---|---|
| DLOG-001 | DEMO shall log start (version, commit, slot, trial state, reset cause), the last fault, the health verdict, button presses and HWLINK-requested resets. | D |
| DLOG-002 | `app_log()` shall never block: it formats into a stack buffer and copies the line into a 1 KB ring drained by the USART6 TXE interrupt, which makes no FreeRTOS call. It shall be safe from tasks, interrupts and before the scheduler starts, and shall not allocate. | I |
| DLOG-003 | A line that does not fit in the ring shall be dropped and counted, never block the caller. | I |
| DLOG-004 | Before a deliberate reset DEMO shall flush the ring by polling, bounded in time, so the last lines are not lost. | I |

---

## 12. Verification matrix

| Area | Requirements | Main method |
|---|---|---|
| General | DGEN-001…008 | T, I |
| Bootloader contract | DBT-001…006 | T, I |
| FreeRTOS | DRT-001…013 | T, I |
| LEDs | LED-001…007 | T, D |
| Button | BTN-001…004 | T, D |
| Communication | COM-001…007 | T |
| Health | HLT-001…005 | T |
| Safety | DSAFE-001…003 | I, T |
| Non-functional | DNFR-001…005 | T, I |

---

## Appendix A — LED patterns

| Pattern | Behaviour (period P) |
|---|---|
| `OFF` | Off |
| `ON` | On |
| `BLINK` | On P/2, off P/2 |
| `HEARTBEAT` | Two 50 ms flashes 200 ms apart, then off for the rest of P (default P = 1000 ms) |
| `FAST` | `BLINK` at P/5 |
| `CHASE` | LD1 → LD2 → LD1… stepping every P/2 (applies to LD1+LD2 together) |
| `MORSE_SOS` | ··· — — — ··· with a 100 ms unit (P ignored) |

Fault pattern on LD3 (DGEN-004, not selectable): three short flashes, pause, repeat.

## Appendix B — Open items

| ID | Item | Status |
|---|---|---|
| OPEN-D01 | Decide whether DEMO should also build without BOOT (linked at `0x0800 0000`) for debugging. | Open |
| OPEN-D02 | Agree the common source directory shared with HWVAL (DGEN-007): which board and clock files move out of `validation-firmware`, and how the other repositories consume them (git submodule or copy). | Open |
| OPEN-D03 | Confirm the LD1 solder bridge (PB0 vs PA5) on this board (HWVAL OPEN-002). | Open bench item |
