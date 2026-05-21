# vanmoof-s3-decomp — build for the BLE-MCU application image
# Target firmware: bleware (bleware_1.4.01.bin, TI CC2642R1F,
# Cortex-M4F + FPv4-SP, BLE 5.2 SoC). 344 KB of usable main flash
# (the last 8 KB belongs to bleboot's BIM), 80 KB SRAM.
#
# Unlike bleboot, bleware ships with an 0x90-byte TI OAD-NVM1 image
# header at the front. The header stays at flash 0x00000000..0x0000008F
# at runtime; bleware's vector table starts at flash 0x00000090 (the
# `prgEntry` field in the header points there). See docs/hardware.md
# for the full header layout.

PROJECT      := bleware
MCU          := cortex-m4
FPU          := fpv4-sp-d16
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
LD_SCRIPT    := linker_cc2642r1.ld

OEM_IMAGE    ?= bleware/bleware_1.4.01.bin

C_SRCS       := $(shell find $(SRC_DIR) -name '*.c' 2>/dev/null)
S_SRCS       := $(shell find $(SRC_DIR) -name '*.S' 2>/dev/null)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
                $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS))
DEPS         := $(OBJS:.o=.d)

# CC2642R1F has hardware FPv4-SP. bleware exercises the FPU in places
# (e.g. audio resampling, OAD CRC). Keep hard-float — Reset_Handler
# enables CP10/CP11 in SCB->CPACR as one of its first acts.
ARCH_FLAGS   := -mcpu=$(MCU) -mthumb -mfpu=$(FPU) -mfloat-abi=hard

CFLAGS       := $(ARCH_FLAGS) \
                -std=c11 \
                -Os -g3 \
                -ffunction-sections -fdata-sections \
                -fno-common -fno-builtin \
                -Wall -Wextra -Wpedantic -Wshadow \
                -Wno-unused-parameter \
                -I$(INC_DIR) \
                -MMD -MP

ASFLAGS      := $(ARCH_FLAGS) -g3 -I$(INC_DIR) -MMD -MP -Wa,--no-warn

LDFLAGS      := $(ARCH_FLAGS) \
                -T$(LD_SCRIPT) \
                -Wl,--gc-sections \
                -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map \
                -nostartfiles -nostdlib

.PHONY: all clean compare size disasm help
.DEFAULT_GOAL := all

all: $(BUILD_DIR)/$(PROJECT).bin size

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PROJECT).elf: $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# Raw flash image — vector table at 0x90, post-OAD-header. The
# `-R .data -R .bss -R ._user_heap_stack` strips SRAM-resident
# sections (their LMA is in FLASH for .data but the section maps to
# SRAM; we don't want objcopy emitting those at SRAM offsets in the
# raw bin).
$(BUILD_DIR)/$(PROJECT).bin: $(BUILD_DIR)/$(PROJECT).elf
	$(OBJCOPY) -O binary --gap-fill=0xFF \
		-R .data -R .bss -R ._user_heap_stack \
		$< $@

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
		echo "Set OEM_IMAGE=path/to/bleware_X.Y.Z.bin"; exit 1; \
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
