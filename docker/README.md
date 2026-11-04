# Docker Development Environment

| | |
|---|---|
| **Author** | Sandesh Ghimire <sandesh@soccentric.com> |
| **Date** | 2026-11-03 |
| **Board** | NUCLEO-F767ZI |

the FreeRTOS demo application is built, flashed and debugged in the same `stm32-dev:latest` image as
the validation firmware. [Dockerfile](Dockerfile) and
[scripts/docker-dev.sh](../scripts/docker-dev.sh) are identical copies of the
files in
[soccentric-stm32-validation-firmware](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware),
so an image built from any of the repositories serves all of them. Keep the
copies in sync when the image changes.

The full description of the image (tools, STM32CubeF7 v1.17.4 SDK layout,
host requirements, debugging) is in the validation firmware's
[docker/README.md](https://github.com/soccentric-stm32/soccentric-stm32-validation-firmware/blob/HEAD/docker/README.md).

## Quick start

```bash
scripts/docker-dev.sh build      # build the image (automatic on first use)
scripts/build.sh                 # configure + build in the container
make flash                       # upload build/demo.img through BOOT (bootctl)
make serial                      # ST-LINK VCP console, 115200 8N1
```

Run `make help` for all targets and `scripts/docker-dev.sh --help` for the
container commands.
