# vanmoof-s3-decomp — build for the main controller application
# Target firmware: mainware (mainware_1.07.06.bin, STM32F413VGT6,
# Cortex-M4F). 1 MB flash, 320 KB SRAM (256+64) contiguous.

PROJECT      := mainware
MCU          := cortex-m4
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
LD_SCRIPT    := linker_stm32f413.ld

OEM_IMAGE    ?= mainware_1.07.06.bin

C_SRCS       := $(wildcard $(SRC_DIR)/*.c)
S_SRCS       := $(wildcard $(SRC_DIR)/*.S)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
                $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(S_SRCS))
DEPS         := $(OBJS:.o=.d)

comma        := ,

ARCH_FLAGS   := -mcpu=$(MCU) -mthumb -mfloat-abi=soft

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

# --- Renode test image -------------------------------------------------------
# The normal --gc-sections build drops every function main() cannot reach (main
# is still the startup spin-stub until the real super-loop's callee closure is
# rooted), so the test ELF re-roots the pure leaves the Renode suite calls by
# forcing each one `undefined` at link time. All listed symbols are self-contained
# integer routines (Cortex-M4 hardware MUL/UDIV — no libgcc helper is pulled in
# under -nostdlib), so the closure links cleanly.
TEST_KEEP    := crc16 crc16_modbus_update bcd_to_bin bin_to_bcd \
                ringbuf_push_byte ringbuf_get_byte ringbuf_free_space \
                telemetry_map_clamp
TEST_ELF     := $(BUILD_DIR)/$(PROJECT)-test.elf

.PHONY: all clean compare size disasm flash help renode-test
.DEFAULT_GOAL := all

all: $(BUILD_DIR)/$(PROJECT).bin size

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S | $(BUILD_DIR)
	$(AS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PROJECT).elf: $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

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
		echo "Set OEM_IMAGE=path/to/mainware_X.YY.ZZ.bin"; exit 1; \
	fi
	@echo "OEM:  $$(stat -c%s $(OEM_IMAGE) 2>/dev/null || stat -f%z $(OEM_IMAGE)) bytes  $(OEM_IMAGE)"
	@echo "OURS: $$(stat -c%s $(BUILD_DIR)/$(PROJECT).bin 2>/dev/null || stat -f%z $(BUILD_DIR)/$(PROJECT).bin) bytes  $(BUILD_DIR)/$(PROJECT).bin"
	@diff_bytes=$$(cmp -l $(OEM_IMAGE) $(BUILD_DIR)/$(PROJECT).bin 2>/dev/null | wc -l); \
	echo "differing bytes: $$diff_bytes"
	@cmp $(OEM_IMAGE) $(BUILD_DIR)/$(PROJECT).bin && echo "BYTE-EQUIVALENT" || true

# Test image: same objects as the normal build, but re-root the leaves the
# Renode suite exercises so --gc-sections keeps them.
$(TEST_ELF): $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(addprefix -Wl$(comma)--undefined=,$(TEST_KEEP)) $(OBJS) -o $@

# Renode emulation smoke tests. Runs from the repo root so the suite's
# @mainware/... paths resolve. Requires `renode-test` on PATH.
renode-test: $(TEST_ELF)
	@command -v renode-test >/dev/null 2>&1 || { \
		echo "renode-test not found on PATH — install Renode (https://renode.io)"; exit 1; }
	cd .. && renode-test mainware/tests/$(PROJECT).robot

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets:"
	@echo "  all     — build $(BUILD_DIR)/$(PROJECT).bin (default)"
	@echo "  size    — show flash/SRAM usage"
	@echo "  disasm  — emit interleaved C+asm listing"
	@echo "  compare — diff our build against OEM_IMAGE"
	@echo "  renode-test — run the Renode smoke-test suite"
	@echo "  clean   — remove build/"
	@echo "Variables:"
	@echo "  OEM_IMAGE=$(OEM_IMAGE)"

-include $(DEPS)
