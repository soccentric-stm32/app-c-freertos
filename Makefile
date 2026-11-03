#
# Makefile - Top-level build, clean and flash for the FreeRTOS demo application
#
# Author:  Sandesh Ghimire <sandesh@soccentric.com>
# Target:  NUCLEO-F767ZI (STM32F767ZI)
#
# The firmware is built with CMake + Ninja inside the stm32-dev container
# (DCON-001), the same way as scripts/build.sh. Flashing and debugging also
# run in the container by default; set NATIVE=1 to use the host's OpenOCD,
# GDB and picocom instead (scripts/install-tools.sh).
#
#   make                build the firmware (same as make build)
#   make build          configure (if needed) and build
#   make clean          remove the build directory
#   make rebuild        clean, then build
#   make flash          build, then flash app.elf, verify and reset
#   make erase          erase the whole flash
#   make reset          reset the board
#   make debug          build, flash, then GDB halted at reset
#   make gdbserver      OpenOCD GDB server on :3333
#   make serial         serial console on the ST-LINK VCP (115200 8N1)
#   make lint           clang-tidy static analysis
#   make lint-strict    clang-tidy, failing on any finding
#   make probe          detect the ST-LINK and the target chip
#   make size           show the firmware size
#   make image          build the stm32-dev Docker image
#   make help           show this help
#
# Variables (override on the command line, e.g. make BUILD_TYPE=Debug):
#   BUILD_TYPE   Release | Debug                       (Release, as build.sh)
#   NATIVE       1 = flash/debug/serial with host tools (0)
#

BUILD_TYPE ?= Release
NATIVE     ?= 0

BUILD_DIR  := build
ELF        := $(BUILD_DIR)/app.elf
DEV        := scripts/docker-dev.sh
OPENOCD    := openocd -f board/st_nucleo_f7.cfg

.DEFAULT_GOAL := build

.PHONY: all build configure clean rebuild lint lint-strict flash erase reset \
        debug gdbserver serial probe size image help

all: build

# Reconfigure when the build directory is new or the build type changed;
# otherwise let Ninja's own dependency tracking decide what to rebuild.
configure:
	@if ! grep -qx "CMAKE_BUILD_TYPE:STRING=$(BUILD_TYPE)" $(BUILD_DIR)/CMakeCache.txt 2>/dev/null; then \
		echo "Configuring $(BUILD_TYPE) build in $(BUILD_DIR)/"; \
		$(DEV) run cmake -S /work -B /work/$(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE); \
	fi

build: configure
	$(DEV) run cmake --build /work/$(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean
	@$(MAKE) --no-print-directory build

# clang-tidy needs the compile commands from a configured build tree.
lint: configure
	$(DEV) run cmake --build /work/$(BUILD_DIR) --target clang-tidy

lint-strict:
	$(DEV) run cmake -S /work -B /work/$(BUILD_DIR) -G Ninja -DAPP_LINT_STRICT=ON
	$(DEV) run cmake --build /work/$(BUILD_DIR) --target clang-tidy

size:
	@test -f $(ELF) || { echo "error: $(ELF) not found, run make build"; exit 1; }
	$(DEV) run arm-none-eabi-size /work/$(ELF)

image:
	$(DEV) build

ifeq ($(NATIVE),1)

flash: build
	$(OPENOCD) -c "program $(ELF) verify reset exit"

erase:
	$(OPENOCD) -c "init; reset halt; flash erase_sector 0 0 last; exit"

reset:
	$(OPENOCD) -c "init; reset run; exit"

debug: build
	$(OPENOCD) -l $(BUILD_DIR)/openocd.log & ocd=$$!; \
	trap 'kill $$ocd 2>/dev/null' EXIT; sleep 1; \
	gdb-multiarch -q $(ELF) -ex "target extended-remote :3333" -ex "monitor reset halt" -ex load

gdbserver:
	$(OPENOCD)

serial:
	scripts/serial.sh

probe:
	st-info --probe

else

flash: build
	$(DEV) flash $(ELF)

erase:
	$(DEV) erase

reset:
	$(DEV) reset

debug: build
	$(DEV) debug $(ELF) --load

gdbserver:
	$(DEV) gdbserver

serial:
	$(DEV) serial

probe:
	$(DEV) probe

endif

help:
	@sed -n '/^#   make/p; /^# Variables/,/^#$$/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'
