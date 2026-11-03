# soccentric-stm32-application

FreeRTOS demo application for the NUCLEO-F767ZI that runs under the A/B bootloader. It blinks the on-board LEDs in selectable patterns, reads the user button, speaks HWLINK over UART for the GUI, and confirms itself to the bootloader after a health check. It is the template for new applications.

| | |
|---|---|
| Requirements | [SRS.md](SRS.md) — SRS-DEMO-F767 (v0.1 draft) |
| Board | NUCLEO-F767ZI — see [Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md) |
| Status | Skeleton: 216 MHz clock, TIM6 HAL tick, FreeRTOS with the LED task (HEARTBEAT on LD1). Comms, Button, Health and the bootloader image file still to come. |
| Build environment | [docker/README.md](docker/README.md): same `stm32-dev` image as the validation firmware (STM32CubeF7 v1.17.4, arm-none-eabi-gcc 13.2) |

## Build and flash

Everything runs in the `stm32-dev` container through `scripts/docker-dev.sh`:

```bash
scripts/build.sh            # cmake configure + ninja build (in the container)
scripts/build.sh clean      # from scratch
make flash                  # flash build/app.elf over SWD, verify, reset
make help                   # all targets (lint, debug, serial, ...)
```

Outputs in `build/`: `app.elf`, `app.bin`, `app.hex`, `app.map`. `make flash` programs `app.elf` at `0x0800C000` (the bootloader slot). It starts only through the bootloader, which must be installed first.

## Related projects

| Repository | Role |
|---|---|
| [soccentric-stm32-validation-firmware](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware) | HWVAL hardware validation firmware |
| [soccentric-stm32-bootloader](https://github.com/soccentric-stm32/soccentric-stm32-bootloader) | A/B bootloader (UART update, CRC32, trial boot and rollback) |
| [soccentric-stm32-application](https://github.com/soccentric-stm32/soccentric-stm32-application) | FreeRTOS demo application in C (LEDs, button, HWLINK) |
| [soccentric-stm32-rust-application](https://github.com/soccentric-stm32/soccentric-stm32-rust-application) | The same demo in Rust (Embassy) |
| [soccentric-stm32-zephyr-application](https://github.com/soccentric-stm32/soccentric-stm32-zephyr-application) | The same demo on Zephyr RTOS |
| [soccentric-stm32-validation-gui](https://github.com/soccentric-stm32/soccentric-stm32-validation-gui) | PySide6 GUI: runs HWVAL tests, updates and flashes firmware |
| [soccentric-stm32-factory-provisioning](https://github.com/soccentric-stm32/soccentric-stm32-factory-provisioning) | HWVAL Factory: end-of-line test and provisioning station |

## License

[MIT](LICENSE) © 2026 Sandesh Ghimire. Use it as a starting point for your own
products. Third-party code (STM32CubeF7, FreeRTOS, LwIP, Embassy, Zephyr and
others) is fetched at build time and keeps its own licence.

Author: Sandesh Ghimire <sandesh@soccentric.com>
