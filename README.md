# soccentric-stm32-application

FreeRTOS demo application for the NUCLEO-F767ZI that runs under the A/B bootloader. It blinks the on-board LEDs in selectable patterns, reads the user button, speaks HWLINK over UART for the GUI, and confirms itself to the bootloader after a health check. It is the template for new applications.

| | |
|---|---|
| Requirements | [SRS.md](SRS.md) — SRS-DEMO-F767 (v0.1 draft) |
| Board | NUCLEO-F767ZI — see [Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md) |
| Status | Specification only; no implementation yet |

## Related projects

| Repository | Role |
|---|---|
| [soccentric-stm32-validation-firmware](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware) | HWVAL hardware validation firmware |
| [soccentric-stm32-bootloader](https://github.com/soccentric-stm32/soccentric-stm32-bootloader) | A/B bootloader (UART update, CRC32, trial boot and rollback) |
| [soccentric-stm32-application](https://github.com/soccentric-stm32/soccentric-stm32-application) | FreeRTOS demo application (LEDs, button, HWLINK) |
| [soccentric-stm32-validation-gui](https://github.com/soccentric-stm32/soccentric-stm32-validation-gui) | PySide6 GUI: runs HWVAL tests, updates and flashes firmware |

Author: Sandesh Ghimire <sandesh@soccentric.com>
