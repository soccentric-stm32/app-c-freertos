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
#   make flash          build, then upload demo.img through BOOT (bootctl)
#   make erase-all      erase the whole flash (asks for confirmation)
#   make reset          reset the board
#   make debug          build, flash, then GDB halted at reset
#   make gdbserver      OpenOCD GDB server on :3333
#   make serial         serial console on the ST-LINK VCP (115200 8N1)
#   make lint           clang-tidy static analysis
#   make lint-strict    clang-tidy, failing on any finding
#   make format         format src/ and board/ with clang-format
#   make probe          detect the ST-LINK and the target chip
#   make size           show the firmware size
#   make docs           build the Doxygen API documentation
#   make image          build the stm32-dev Docker image
#   make help           show this help
#
# Variables (override on the command line, e.g. make BUILD_TYPE=Debug):
#   BUILD_TYPE   Release | Debug                       (Release, as build.sh)
#   NATIVE       1 = flash/debug/serial with host tools (0)
#   BOOTCTL      bootctl.py used by make flash
#                (default ../bootloader/tools/bootctl.py)
#   PORT         serial port for make flash (default: bootctl auto-detect)
#   CLANG_FORMAT clang-format binary used by make format (clang-format)
#

BUILD_TYPE ?= Release
NATIVE     ?= 0

BUILD_DIR  := build
ELF        := $(BUILD_DIR)/app.elf
IMAGE      := $(BUILD_DIR)/demo.img
DEV        := scripts/docker-dev.sh
OPENOCD    := openocd -f board/st_nucleo_f7.cfg
BOOTCTL    ?= ../bootloader/tools/bootctl.py
PORT       ?=
PYTHON     ?= python3
CLANG_FORMAT ?= clang-format

# Hand-written sources only; generated headers and third_party are not
# formatted here.
FORMAT_SOURCES := $(shell find src board -type f \( -name '*.c' -o -name '*.h' \) -not -path 'src/generated/*')

.DEFAULT_GOAL := help

.PHONY: all build configure clean rebuild lint lint-strict format flash \
        erase-all reset debug gdbserver serial probe size docs image help

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

# clang-tidy needs the compile commands from a configured build tree.  Set the
# strict flag explicitly so a previous `make lint-strict` does not leak into a
# later, non-strict run.
lint: configure
	$(DEV) run cmake -S /work -B /work/$(BUILD_DIR) -G Ninja -DAPP_LINT_STRICT=OFF
	$(DEV) run cmake --build /work/$(BUILD_DIR) --target clang-tidy

lint-strict:
	$(DEV) run cmake -S /work -B /work/$(BUILD_DIR) -G Ninja -DAPP_LINT_STRICT=ON
	$(DEV) run cmake --build /work/$(BUILD_DIR) --target clang-tidy

# clang-format is a host tool (scripts/install-tools.sh), like doxygen.
format:
	@command -v $(CLANG_FORMAT) >/dev/null 2>&1 || { \
		echo "error: clang-format not installed"; exit 1; }
	$(CLANG_FORMAT) -i $(FORMAT_SOURCES)
	@echo "format: $(FORMAT_SOURCES)"

# A-6: never program the raw ELF into a slot. BOOT only starts an image
# that has its 256-byte header, metadata and records, which bootctl writes.
flash: build
	@test -f $(IMAGE) || { echo "error: $(IMAGE) not found, run make build"; exit 1; }
	@test -f $(BOOTCTL) || { echo "error: $(BOOTCTL) not found; set BOOTCTL=/path/to/bootctl.py"; exit 1; }
	$(PYTHON) $(BOOTCTL) $(if $(PORT),--port $(PORT),) update $(IMAGE) --activate

ERASE_CMD = $(if $(filter 1,$(NATIVE)),$(OPENOCD) -c "init; reset halt; flash erase_sector 0 0 last; exit",$(DEV) erase)

erase-all:
	@echo "WARNING: this erases the whole flash: BOOT, both metadata sectors"
	@echo "         and both application slots."
	@printf "Type 'yes' to continue: " && read answer && [ "$$answer" = "yes" ]
	$(ERASE_CMD)

size:
	@test -f $(ELF) || { echo "error: $(ELF) not found, run make build"; exit 1; }
	$(DEV) run arm-none-eabi-size /work/$(ELF)

# Doxygen is a host tool (scripts/install-tools.sh or the stm32-dev image).
docs:
	@command -v doxygen >/dev/null 2>&1 || { echo "error: doxygen not installed"; exit 1; }
	doxygen Doxyfile
	@echo "docs: docs/doxygen/html/index.html"

image:
	$(DEV) build

ifeq ($(NATIVE),1)

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
