# vanmoof-s3-decomp — motorware (TI TMS320F28054F, C2000/C28x DSP — NOT ARM)
# Target firmware: motor-controller application.
#
# There is no GCC backend for the C28x; a byte-equivalent rebuild needs TI's
# cl2000 codegen tools (Code Composer Studio), which are not wired in here yet.
# Until then the build is analysis-only: this Makefile drives the container
# codec (tools/bootstream.py) that unwraps the VanMoof header, parses the C28x
# boot-ROM data stream, reconstructs the flash image, and verifies a byte-exact
# round-trip — the container-level analogue of the other wares' `compare`.

PROJECT      := motorware
PY           := python3
BUILD_DIR    := build
TOOLS        := tools

# OEM image (override on the command line). The current packed version is .22.
OEM_DIR      ?= ../../VanMooof-Firmware/ES3/motorware
OEM_IMAGE    ?= $(OEM_DIR)/motorware_S.0.00.22.bin

.PHONY: all info verify image map clean help
.DEFAULT_GOAL := help

all: verify image

# Full container report: header, version, CRC check, block map, round-trip.
info: $(OEM_IMAGE)
	$(PY) $(TOOLS)/bootstream.py "$(OEM_IMAGE)"

# Container verification — recompute the CRC and prove the byte-exact
# round-trip (rebuild == original). Fails loudly if either check breaks.
verify: $(OEM_IMAGE)
	@$(PY) $(TOOLS)/bootstream.py "$(OEM_IMAGE)" | tee $(BUILD_DIR)/verify.log | \
		grep -E 'crc32|round-trip'
	@grep -q 'crc32 .*OK' $(BUILD_DIR)/verify.log || { echo "CRC MISMATCH"; exit 1; }
	@grep -q 'round-trip  rebuild==content: True' $(BUILD_DIR)/verify.log || { \
		echo "ROUND-TRIP FAILED"; exit 1; }
	@echo "container OK: CRC matches and rebuild is byte-exact"

# Reconstruct the flash image regions (raw LE, 2 bytes/word) for disassembly.
image: $(OEM_IMAGE) | $(BUILD_DIR)
	$(PY) $(TOOLS)/bootstream.py --extract=$(BUILD_DIR)/image "$(OEM_IMAGE)"
	@echo "regions + manifest in $(BUILD_DIR)/image/"

map: info

$(BUILD_DIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "motorware (TMS320F28054F / C2000 — analysis only, no C build yet)"
	@echo "Targets:"
	@echo "  info    — full container report (header, CRC, block map, round-trip)"
	@echo "  verify  — assert CRC matches and rebuild is byte-exact"
	@echo "  image   — extract reconstructed flash regions -> $(BUILD_DIR)/image/"
	@echo "  clean   — remove $(BUILD_DIR)/"
	@echo "Variables:"
	@echo "  OEM_IMAGE=$(OEM_IMAGE)"
