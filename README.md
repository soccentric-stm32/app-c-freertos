# soccentric-stm32-application

FreeRTOS demo application for the NUCLEO-F767ZI that runs under the A/B
bootloader. It blinks the on-board LEDs in selectable patterns, reads the
user button, speaks HWLINK over UART for the GUI, supervises its tasks, and
confirms itself to the bootloader after a health check. It is the template
for new applications.

| | |
|---|---|
| Requirements | [SRS.md](SRS.md) — SRS-DEMO-F767 (v0.1 draft) |
| Board | NUCLEO-F767ZI — see [Hardware.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/Hardware.md) |
| Status | Feature complete for SRS-DEMO-F767: 216 MHz clock, TIM6 HAL tick, FreeRTOS Health/Comms/Button/LED tasks, HWLINK on USART3, button, IWDG supervision, CONFIRMED record, fault capture and the `demo.img` image file. |
| Build environment | [docker/README.md](docker/README.md): same `stm32-dev` image as the validation firmware (STM32CubeF7 v1.17.4, arm-none-eabi-gcc 13.2) |

## Features

- **LEDs:** `OFF`, `ON`, `BLINK`, `HEARTBEAT`, `FAST`, `CHASE` and
  `MORSE_SOS` on LD1/LD2, 50–5000 ms in 10 ms steps. LD3 is reserved for
  errors: solid after a fault or watchdog reset until `ClearFault`, fast
  blink when a pending slot fails its health check.
- **Button B1:** EXTI13 with 30 ms debounce; short press selects the next
  pattern, long press restores `HEARTBEAT` on LD1 and off on LD2.
- **HWLINK:** COBS framing, CRC-32, FlatBuffers verifier, sequence-number
  replay of the last response, `Hello`, `Ping`, `GetStatus`, `SetLed`,
  `SetLedAll`, `ClearFault`, `EnterBootloader`, `Reset`, `SetBaud` and
  unsolicited `ButtonEvent`.
- **Health:** each task checks in with the highest-priority Health task;
  the IWDG (4 s, started by BOOT) is refreshed only while all check-ins and
  the pending-slot health check pass. `CONFIRMED` is programmed once into
  the running slot's metadata sector; the only other flash write is none.
- **Diagnostics:** reset cause from `RCC_CSR`, a no-init fault record with
  CFSR/HFSR/MMFAR/BFAR/PC/LR, task stack high-water marks, and free /
  minimum-ever-free heap in `Status`.
- **Safety:** only PB0/PB7/PB14 (LEDs), PC13 (button) and PD8/PD9 (USART3)
  are used; SWD, HSE, option bytes and all slot sectors are untouched.

## Build and flash

Everything runs in the `stm32-dev` container through `scripts/docker-dev.sh`:

```bash
scripts/build.sh            # cmake configure + ninja build (in the container)
scripts/build.sh clean      # from scratch
make flash                  # upload build/demo.img through BOOT (bootctl)
make erase-all              # erase BOOT + slots (asks for confirmation)
make help                   # all targets (lint, debug, serial, ...)
```

Outputs in `build/`:

| File | Purpose |
|---|---|
| `app.elf`, `app.bin`, `app.hex`, `app.map` | SWD/debug build |
| `demo.elf`, `demo.bin`, `demo.hex`, `demo.map` | Same image under the SRS names (DGEN-001) |
| `demo.img` | BOOT image: 256-byte header + `demo.bin` (`scripts/mkimage.py`) |

`make flash` builds `demo.img` and uploads it through the bootloader with
`bootctl.py update --activate` over the ST-LINK VCP. BOOT must be installed
first. Never program `app.elf` or `app.bin` into a slot over SWD: BOOT only
starts an image that has its 256-byte header, metadata and records, which
the `.img` carries. Point `BOOTCTL` at `bootctl.py` or `PORT` at the serial
device if the defaults do not fit:

```bash
make flash PORT=/dev/ttyACM0
make flash BOOTCTL=/path/to/bootctl.py
```

The build prints the flash/RAM footprint and fails if it exceeds 128 KB /
64 KB (DNFR-001). `-DDEMO_FAIL_HEALTH=ON` forces the pending-slot health
check to fail so rollback can be tested (HLT-004).

## HWLINK protocol

[`protocol/hwlink.fbs`](protocol/hwlink.fbs) is the single schema
(SRS-GUI-F767 Appendix A). The generated C headers are in `src/generated`
and were made with flatcc 0.6.1; the flatcc runtime is vendored under
`third_party/flatcc` with its original licence. When `flatcc` is on `PATH`,
CMake regenerates the headers at build time. See
[`protocol/README.md`](protocol/README.md).

The hand-over record in `RTC_BKP2R`, the metadata record offsets and the
`CONFIRMED` address are defined in [`src/boot_contract.h`](src/boot_contract.h);
see also SRS-BOOT-F767 Appendix B. The running bank is mapped at
`0x08000000`, so the `CONFIRMED` record is always at `0x08008140`.

## Making a new application from DEMO

1. Copy this repository and change `APP_FIRMWARE_NAME`, `APP_VERSION` and
   the linker `--cref`/image name in `CMakeLists.txt`.
2. Keep the bootloader contract: link at `0x0800C000`, set `SCB->VTOR`,
   refresh the IWDG immediately, read `RTC_BKP2R`, confirm a pending slot
   through one audited flash helper, and implement `Hello` /
   `EnterBootloader` with role `DEMO` or a new reserved role.
3. Keep the four tasks (Health highest, then Comms, Button, LED) or add new
   tasks with a Health check-in at least once per second. All queues, tasks
   and buffers must exist before `vTaskStartScheduler()`; no task allocates
   memory at run time.
4. Add new HWLINK messages at the end of the `Body` union in
   `protocol/hwlink.fbs` and raise the protocol minor version.
5. Keep the only flash write the `CONFIRMED` record of the running slot.

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
products. Third-party code (STM32CubeF7, FreeRTOS, flatcc, LwIP, Embassy,
Zephyr and others) is fetched at build time or vendored under its own licence.

Author: Sandesh Ghimire <sandesh@soccentric.com>
