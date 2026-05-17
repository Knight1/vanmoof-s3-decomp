# bleboot

Clean-room reconstruction of the **bleboot 1.0.0** BLE-MCU bootloader
(TI BIM — Boot Image Manager) that occupies the last 8 KB flash page of
the **TI CC2642R1F** on the VanMoof S3 BLE radio module.

| | |
| --- | --- |
| MCU | TI CC2642R1F (Cortex-M4F, BLE 5.2 SoC) |
| Image | `bleboot_1.0.0.bin` (8192 B, built `Apr 23 2020`) |
| Flash location | `0x00056000..0x00057FFF` (last 8 KB page, includes CCFG + OAD hdr) |
| Initial SP | `0x20014000` (top of 80 KB SRAM) |
| Status | `65 decomp-c / 0 vendor-stock / 1 named / 0 pending` (every function in the image is decoded) |

## What it does

The BIM is a small, unattended bootloader. On every reset:

1. `Reset_Handler` runs `SetupTrimDevice` (TI driverlib silicon trim).
2. `ResetISR_body` loads MSP, enables the FPU, walks the cinit table
   (zero-fill SRAM + an LZSS-compressed init record), and calls `main`.
3. `main` reads a chunk-size config byte and tail-calls `bim_dispatch`.
4. `bim_dispatch` chooses an OAD scan strategy:
   - **Full scan** (`bim_full_scan_and_launch`) walks slots 0..43 at
     4 KB stride on the external SPI flash. For each candidate it
     verifies CRC32 + a hash with hardware-ID salt; on success it
     promotes the image into internal flash and jumps.
   - **Quick scan** (`bim_quick_scan_and_launch`) walks the same 44
     slots on **internal** flash (8 KB stride) and launches the first
     one with the "promoted" status byte (`0xFE`) already set.
   - **Verify-and-launch** is a last-ditch path that reads a stashed
     header and jumps if its hash matches.
5. On every-path failure, `bim_panic_prep` + `bim_panic_indicate` light
   **DIO2** and spin in a `b .` loop.

## Building

```bash
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi   # Debian/Ubuntu
brew install --cask gcc-arm-embedded                         # macOS

cd bleboot/
make            # build/bleboot.bin (exactly 8192 B, padded with 0xFF)
make compare    # byte-diff vs bleboot_1.0.0.bin
make clean
```

`make compare` reports differing bytes. **Every static-data region is
byte-identical to the OEM**:

```
✓ Chip database table      (48 B)
✓ ADI step LUT              (8 B)
✓ "OAD NVM1" magic ×2      (16 B)
✓ SPI opcode literals       (8 B)
✓ TI CGT cinit handler tbl + body data + record table  (48 B)
✓ 0xFF fill gap          (3344 B)
✓ "BVER Apr 23 2020" block (32 B)
✓ CCFG (Customer Config)   (88 B)
```

The remaining ~52 % byte difference is GCC `-Os` vs TI CCS instruction
choices in the code region (different register allocation, literal-pool
placement, instruction ordering) with no behavioural impact.

## Validation with Unicorn

Without flashing to silicon, you can run the binary in a Unicorn-based
emulator that stubs the CC2642R1F's peripherals well enough to trace
the BIM boot pipeline end-to-end. The harness:

- Maps Flash / SRAM / ROM / MMIO / SCS regions.
- Stubs FCFG1 to report CC2642R1 PG3.
- Replaces ROM_API_TABLE entries with placeholder addresses; intercepts
  any branch into ROM, sets r0 to a per-slot configured value, and
  returns to the caller. Handles the mixed flat/double-indirect
  conventions across PRCM, SSI, VIMS, FLASH, HAPI tables.
- Tracks PRCM power-on/off state so `PRCM[13]` reports `1` after
  `PRCM[5]` and `2` after `PRCM[6]`, satisfying both `bim_ssi_init`'s
  and `bim_periph_power_off`'s polling loops with the same slot.
- Loads a real SPI-NOR dump (MX25L51245G format) and intercepts
  `bim_spi_flash_read(addr, len, dst)` to synthesize reads directly
  from the dump bytes.
- Feeds the JEDEC ID (`0xC2 0x19` = MX25L51245G) into SSIDataGet when
  `bim_spi_recv_bytes(dst=0x20000404)` is entered, so the chip-table
  match succeeds.
- Detects spin loops (>500 hits on one PC), `b .` panic traps, and
  hard-fault interrupts.

### Running it

```bash
pip install --user unicorn

cd bleboot/
python3 tools/unicorn_boot_trace.py bleboot_1.0.0.bin   # OEM only
python3 tools/unicorn_boot_trace.py build/bleboot.bin   # our build only
python3 tools/unicorn_boot_trace.py compare              # side-by-side
```

Each run drops a per-function-entry trace plus a milestone summary.
Drop an `SPI-Flash_*.rom` dump in `bleboot/` to feed real SPI data into
`bim_spi_flash_read` — without it, reads return zero.

### Validation result

OEM and our reconstruction **hit identical milestones in the same
order** with a real 64 MB SPI dump:

```
✓ Reset_Handler                          ✓ bim_ssi_init
✓ SetupTrimDevice                        ✓ bim_spi_release_from_dpd
✓ ResetISR_body                          ✓ bim_spi_probe_chip   (matches MX25 via 0xC2/0x19)
✓ _system_pre_init                       ✓ bim_full_scan_and_launch
✓ _auto_init_table                       ✓ bim_quick_scan_and_launch
✓ auto_init_zero_fill                    ✓ bim_verify_and_launch_image
✓ cinit_byte_stream_copy (LZSS)          ✓ panic loop hit (when no OAD found)
✓ main → bim_dispatch
```

Both binaries reach the same panic loop when the SPI flash has no
OAD-formatted image at any of the 44 slot anchors (`slot << 12` for
slot 0..43, covering SPI addresses `0x00000..0x2B000`). This is the
expected behaviour: on a real bike where the bleware has already been
promoted to internal flash, the quick-scan path would find it there;
without simulating internal flash content, the harness exhibits the
"first boot, no image" failure mode.

### Bug found during validation

The harness caught a literal-pool misalignment bug in `ResetISR_body`
before any flash attempt: the function landed at `0x5702A`
(2-byte but not 4-byte aligned), and the inline-asm `ldr rN, [pc,
#imm8*4]` loads truncate PC down to a 4-byte boundary, so they read
2 bytes off — loading `0x4000E000` instead of `0x20014000` for MSP
init. On real silicon this would have corrupted SP to a peripheral
address on the first instruction after `Reset_Handler`. Fixed by
`__attribute__((aligned(4)))` on `ResetISR_body` in `src/startup.c`.

## Project layout

```
bleboot/
├── Makefile             ← arm-none-eabi-gcc build
├── linker_cc2642r1.ld   ← memory map; pins all .rodata literals + BVER + CCFG to OEM addrs
├── include/             ← bim.h, exception.h, main.h
├── src/
│   ├── startup.c            ← Reset_Handler + ResetISR_body (naked inline asm)
│   ├── vector_table.c       ← 54-entry CC2642R1F vector table at 0x56000
│   ├── setup_trim.c         ← SetupTrimDevice (TI driverlib silicon trim)
│   ├── auto_init.c          ← _auto_init_table (TI CGT cinit walker)
│   ├── cinit_handlers.c     ← LZSS decompressor + generic memcpy handler
│   ├── chipinfo.c           ← family/hw-rev/assert mirrors
│   ├── setup.c              ← bim_setup_after_cold_reset_cfg1, ADI sequencer
│   ├── rts_hooks.c          ← _system_pre_init, _exit, auto_init_zero_fill
│   ├── flash.c              ← all SPI + internal flash primitives
│   ├── flash_literals.c     ← chip table, ADI LUT, OAD magic, SPI opcodes
│   ├── crc.c                ← bim_crc32_image, bim_crc32_buffer, chip-entry getter
│   ├── oad.c                ← OAD scan/promote/launch
│   ├── bim.c                ← bim_dispatch
│   ├── main.c               ← main()
│   ├── panic.c              ← bim_panic_prep, bim_panic_indicate
│   ├── exception.c          ← HardFault / NMI / Default trap loops
│   └── bver_ccfg.c          ← BVER block + CCFG @ fixed addresses
├── docs/
│   ├── progress.md          ← per-function decomp status (66 functions)
│   ├── hardware.md          ← MMIO map, ROM API tables, AON shadows
│   └── protocol.md          ← OAD wire format
├── ghidra/
│   ├── exports/             ← bleboot_program.json (refreshed each Ghidra run)
│   └── (scripts live in ~/ghidra_scripts/)
├── tools/
│   └── unicorn_boot_trace.py  ← Unicorn validation harness
├── reference/               ← TI driverlib excerpts, datasheet pins
└── build/                   ← build output (gitignored)
```

## Legal

Same as the parent repo: clean-room interoperability work under EU
Software Directive 2009/24/EC Art. 6 and DMCA §1201(f). The OEM image
is not redistributed; extract `bleboot_1.0.0.bin` from a `.pak` file or
JTAG dump of a CC2642R1F you own. No warranty — flashing a wrong image
to the BLE radio MCU will brick it. Use a spare BLE PCB or the Unicorn
harness for testing.
