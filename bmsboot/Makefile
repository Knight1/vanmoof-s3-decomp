# vanmoof-s3-decomp — build for STM32L072CZT6 (ARM Cortex-M0+)
# Target firmware: bmsboot (VanMoof battery-module / BMS bootloader).

PROJECT      := bmsboot
MCU          := cortex-m0plus
TOOLCHAIN    := arm-none-eabi-

CC           := $(TOOLCHAIN)gcc
AS           := $(TOOLCHAIN)gcc
LD           := $(TOOLCHAIN)gcc
OBJCOPY      := $(TOOLCHAIN)objcopy
OBJDUMP      := $(TOOLCHAIN)objdump
SIZE         := $(TOOLCHAIN)size

BUILD_DIR    := build
SRC_DIR      := src
INC_DIR      := include
LD_SCRIPT    := linker_stm32l072.ld

OEM_IMAGE    ?= bmsboot_v007.bin

C_SRCS       := $(wildcard $(SRC_DIR)/*.c)
S_SRCS       := $(wildcard $(SRC_DIR)/*.S)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
                $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS))
DEPS         := $(OBJS:.o=.d)

ARCH_FLAGS   := -mcpu=$(MCU) -mthumb -mfloat-abi=soft

CFLAGS       := $(ARCH_FLAGS) \
                -std=c11 \
                -Os -g3 \
                -ffunction-sections -fdata-sections -fno-common -fno-builtin \
                -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter \
                -I$(INC_DIR) -MMD -MP

ASFLAGS      := $(ARCH_FLAGS) -g3 -I$(INC_DIR) -MMD -MP -Wa,--no-warn

LDFLAGS      := $(ARCH_FLAGS) \
                -T$(LD_SCRIPT) \
                -Wl,--gc-sections \
                -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map \
                -nostartfiles -nostdlib

LDLIBS       := -lgcc

.PHONY: all clean compare size disasm help test
.DEFAULT_GOAL := all

# ---- Renode test image -------------------------------------------------------
# The normal targets can't link standalone (the HAL/CMSIS leaves are extern).
# `make test` links the real firmware objects against tests/stubs.c (vendor-leaf
# stand-ins) into a self-contained ELF + bin for Renode, and generates a
# known-good 6 KB EEPROM fixture (real field layout) the suite loads for its
# real-data tests.
TEST_ELF     := $(BUILD_DIR)/$(PROJECT)_test.elf
TEST_BIN     := $(BUILD_DIR)/$(PROJECT)_test.bin
TEST_EEPROM  := $(BUILD_DIR)/eeprom_test.bin
EEPROM_GEN   := ../batteryware/tools/eeprom_example.py
TEST_OBJS    := $(OBJS) $(BUILD_DIR)/stubs.o

$(BUILD_DIR)/stubs.o: tests/stubs.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_ELF): $(TEST_OBJS) $(LD_SCRIPT) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(TEST_OBJS) $(LDLIBS) -o $@

$(TEST_BIN): $(TEST_ELF)
	$(OBJCOPY) -O binary $< $@

$(TEST_EEPROM): $(EEPROM_GEN) | $(BUILD_DIR)
	python3 $(EEPROM_GEN) --boot-flag 0x55 --esn "VM3SCANNED0001" -o $@

test: $(TEST_BIN) $(TEST_EEPROM)
	@$(SIZE) $(TEST_ELF)
	@echo "built $(TEST_ELF) + $(TEST_BIN) + $(TEST_EEPROM) (Renode test images)"

all: $(BUILD_DIR)/$(PROJECT).bin size

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PROJECT).elf: $(OBJS) $(LD_SCRIPT) | $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(BUILD_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(PROJECT).hex: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O ihex $< $@

size: $(BUILD_DIR)/$(PROJECT).elf
	@$(SIZE) $<

disasm: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJDUMP) -d -S $< > $(BUILD_DIR)/$(PROJECT).lst
	@echo "wrote $(BUILD_DIR)/$(PROJECT).lst"

compare: $(BUILD_DIR)/$(PROJECT).bin
	@if [ ! -f "$(OEM_IMAGE)" ]; then \
		echo "OEM_IMAGE not found at $(OEM_IMAGE)"; \
		echo "Set OEM_IMAGE=path/to/bmsboot_v007.bin"; exit 1; \
	fi
	@echo "OEM:  $$(stat -c%s $(OEM_IMAGE) 2>/dev/null || stat -f%z $(OEM_IMAGE)) bytes  $(OEM_IMAGE)"
	@echo "OURS: $$(stat -c%s $(BUILD_DIR)/$(PROJECT).bin 2>/dev/null || stat -f%z $(BUILD_DIR)/$(PROJECT).bin) bytes  $(BUILD_DIR)/$(PROJECT).bin"
	@diff_bytes=$$(cmp -l $(OEM_IMAGE) $(BUILD_DIR)/$(PROJECT).bin 2>/dev/null | wc -l); \
	echo "differing bytes: $$diff_bytes"
	@cmp $(OEM_IMAGE) $(BUILD_DIR)/$(PROJECT).bin && echo "BYTE-EQUIVALENT" || true

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets:"
	@echo "  all     — build $(BUILD_DIR)/$(PROJECT).bin (default)"
	@echo "  size    — show flash/SRAM usage"
	@echo "  disasm  — emit interleaved C+asm listing"
	@echo "  compare — diff our build against OEM_IMAGE"
	@echo "  clean   — remove build/"
	@echo "Variables:"
	@echo "  OEM_IMAGE=$(OEM_IMAGE)"

-include $(DEPS)
