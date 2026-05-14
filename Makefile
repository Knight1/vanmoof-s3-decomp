# Top-level dispatcher for VanMoof S3 firmware decompilations.
#
# Each ware lives in its own subdirectory and has its own Makefile. This
# file is a thin forwarder so you can run `make shifterware` (or just
# `make`, which builds whatever is active) from the repo root.

WARES        := shifterware shifterboot mainboot
ACTIVE_WARE  := shifterware

.PHONY: all clean help $(WARES) all-wares compare size disasm

all: $(ACTIVE_WARE)

help:
	@echo "Targets:"
	@echo "  <ware>        — build one ware ($(WARES))"
	@echo "  all-wares     — build every ware listed in \$$(WARES)"
	@echo "  clean         — clean every ware"
	@echo "  compare       — diff active ware against its OEM image"
	@echo "  size          — show size of active ware"
	@echo "  disasm        — emit listing for active ware"
	@echo "Active ware: $(ACTIVE_WARE)"

$(WARES):
	$(MAKE) -C $@

all-wares: $(WARES)

clean:
	@for w in $(WARES); do $(MAKE) -C $$w clean; done

compare size disasm:
	$(MAKE) -C $(ACTIVE_WARE) $@
