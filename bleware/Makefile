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
#
# ---- TI SDK integration ----
#
# This build requires the TI SimpleLink CC13x2/CC26x2 SDK 3.40.00.02.
# Download from TI.com (free account), then either:
#
#   1. Set TI_SDK on the make command line:
#        make TI_SDK=~/ti/simplelink_cc13x2_26x2_sdk_3_40_00_02
#
#   2. Or extract into vendor/ with the helper script:
#        ./vendor/fetch_sdk.sh ~/Downloads/sdk.zip
#        make TI_SDK=vendor/ti-sdk-3.40
#
#   3. Or export once:
#        export TI_SDK=vendor/ti-sdk-3.40
#        make
#
# Without TI_SDK, the build uses weak stubs only (compiles but the
# binary is not functional). See vendor/README.md for details.

PROJECT      := bleware
MCU          := cortex-m4
FPU          := fpv4-sp-d16
TOOLCHAIN    := arm-none-eabi-
TI_SDK       ?=

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
VENDOR_DIR   := vendor

OEM_IMAGE    ?= bleware/bleware_1.4.01.bin

# ---- SDK detection ---------------------------------------------------

ifeq ($(TI_SDK),)
  $(info TI_SDK not set — building with weak stubs only (no BLE stack, no drivers))
  $(info To build a functional image, set TI_SDK=path/to/sdk. See vendor/README.md)
  SDK_CFLAGS := -DBLEWARE_NO_SDK
  SDK_INC   :=
  SDK_SRCS  :=
else ifeq ($(wildcard $(TI_SDK)/source/ti/ble5stack/rom/rom_jt.h),)
  $(error TI_SDK set to '$(TI_SDK)' but rom_jt.h not found. Wrong SDK version? Expected SDK 3.40.00.02)
else
  $(info Using TI SDK: $(TI_SDK))
  SDK_CFLAGS :=
  SDK_INC   := -I$(TI_SDK)/source \
               -I$(TI_SDK)/source/ti/ble5stack \
               -I$(TI_SDK)/source/ti/ble5stack/inc \
               -I$(TI_SDK)/source/ti/ble5stack/icall/inc \
               -I$(TI_SDK)/source/ti/ble5stack/rom \
               -I$(TI_SDK)/source/ti/ble5stack/hal/src/common \
               -I$(TI_SDK)/source/ti/drivers \
               -I$(TI_SDK)/source/ti/devices/cc13x2_cc26x2 \
               -I$(TI_SDK)/source/ti/devices/cc13x2_cc26x2/inc \
               -I$(TI_SDK)/kernel/tirtos/packages
  SDK_SRCS  :=
  # The TI SDK source (.c files) is NOT compiled by this Makefile.
  # The SDK uses TI's XDCtools configuration system and Code Composer
  # Studio project format. All SDK runtime functions called by bleware
  # reside in the CC2642R1F mask ROM or are provided by the TI-RTOS
  # kernel — these are resolved at link time through the ROM thunks
  # in src/hal_stubs.S (thunk_EXT_FUN_1002xxxx entries) when a full
  # TI toolchain build is available.
  #
  # This Makefile compiles ONLY the VanMoof-custom source files (62
  # .c files in src/) and links them with the ROM thunk stubs. The
  # result is a stub binary (1028 B) suitable for verifying that
  # decompiled code compiles without warnings. A functional firmware
  # image requires building inside TI Code Composer Studio or IAR
  # with the full SimpleLink SDK project.
endif

C_SRCS       := $(shell find $(SRC_DIR) -name '*.c' 2>/dev/null) $(SDK_SRCS)
S_SRCS       := $(shell find $(SRC_DIR) -name '*.S' 2>/dev/null)
OBJS         := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%,$(C_SRCS))) \
                $(patsubst $(SRC_DIR)/%.S,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%,$(S_SRCS))) \
                $(patsubst $(TI_SDK)/%.c,$(BUILD_DIR)/$(TI_SDK)/%.o,$(filter $(TI_SDK)/%,$(C_SRCS)))
DEPS         := $(OBJS:.o=.d)

# CC2642R1F has hardware FPv4-SP. bleware exercises the FPU in places
# (e.g. audio resampling, OAD CRC). Keep hard-float — Reset_Handler
# enables CP10/CP11 in SCB->CPACR as one of its first acts.
ARCH_FLAGS   := -mcpu=$(MCU) -mthumb -mfpu=$(FPU) -mfloat-abi=hard

CFLAGS       := $(ARCH_FLAGS) \
                $(SDK_CFLAGS) \
                -std=c11 \
                -Os -g3 \
                -ffunction-sections -fdata-sections \
                -fno-common -fno-builtin \
                -Wall -Wextra -Wpedantic -Wshadow \
                -Wno-unused-parameter \
                -I$(INC_DIR) \
                $(SDK_INC) \
                -MMD -MP

ASFLAGS      := $(ARCH_FLAGS) -g3 -I$(INC_DIR) -MMD -MP -Wa,--no-warn

LDFLAGS      := $(ARCH_FLAGS) \
                -T$(LD_SCRIPT) \
                -Wl,--gc-sections \
                -Wl,-Map=$(BUILD_DIR)/$(PROJECT).map \
                -nostartfiles -nostdlib

.PHONY: all clean compare size disasm help test renode-test
.DEFAULT_GOAL := all

# ---- Renode test image -------------------------------------------------------
# The production link uses --gc-sections, which drops the self-contained leaf
# routines (crc32_le, crc16_modbus, lcg_random_u15) the minimal reconstruction
# doesn't yet reference. `make test` relinks the same objects but force-retains
# those symbols (--undefined makes each a GC root) so the Renode suite can call
# them directly. Identical bytes otherwise — test scaffolding only.
TEST_ELF   := $(BUILD_DIR)/$(PROJECT)_test.elf
TEST_KEEP  := -Wl,--undefined=crc32_le -Wl,--undefined=crc16_modbus \
              -Wl,--undefined=lcg_random_u15 -Wl,--undefined=monitor_strlen \
              -Wl,--undefined=svc_5560_char_uuid_to_index

$(TEST_ELF): $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(TEST_KEEP) $(OBJS) -o $@

test: $(TEST_ELF)
	@$(SIZE) $(TEST_ELF)
	@echo "built $(TEST_ELF) (Renode test image — CRC/LCG leaves retained)"

# Renode emulation smoke tests. Runs from the repo root so the suite's
# @bleware/... paths resolve. Requires `renode-test` on PATH.
renode-test: $(TEST_ELF)
	@command -v renode-test >/dev/null 2>&1 || { \
		echo "renode-test not found on PATH — install Renode (https://renode.io)"; exit 1; }
	cd .. && renode-test bleware/tests/$(PROJECT).robot

all: $(BUILD_DIR)/$(PROJECT).bin size

$(BUILD_DIR):
	@mkdir -p $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# SDK and vendor source files — use vpath so they resolve against
# the absolute include paths in SDK_SRCS.
vpath %.c $(dir $(SDK_SRCS))
vpath %.S $(SRC_DIR)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(PROJECT).elf: $(OBJS) $(LD_SCRIPT)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# SDK objects — compile from absolute path to build/<absolute-path>.o
$(BUILD_DIR)/$(TI_SDK)/%.o: $(TI_SDK)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

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
	@echo "  sdk     — show TI SDK integration status"
	@echo "Variables:"
	@echo "  TI_SDK   = $(if $(TI_SDK),$(TI_SDK),(not set — weak stubs only))"
	@echo "  OEM_IMAGE = $(OEM_IMAGE)"

sdk:
	@echo "TI SDK path: $(if $(TI_SDK),$(TI_SDK),(not set))"
	@echo "SDK integration: $(if $(TI_SDK),active ($(SDK_INC)),weak stubs only (no BLE stack))"
	@echo ""
	@echo "To integrate the SDK:"
	@echo "  1. Download SimpleLink CC13x2/CC26x2 SDK 3.40 from TI.com"
	@echo "  2. Run: ./vendor/fetch_sdk.sh <path-to-sdk-zip>"
	@echo "  3. Build: make TI_SDK=vendor/ti-sdk-3.40"
	@echo "See vendor/README.md for details."

-include $(DEPS)
