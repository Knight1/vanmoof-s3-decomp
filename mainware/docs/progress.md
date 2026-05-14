# mainware — decomp progress

Target binary: `mainware_1.07.06.bin` (218784 bytes, ARM Cortex-M4,
STM32F413VGT6). Loaded into Ghidra at `0x08020000` so the 512-byte
envelope at file offset `0..0x1FF` lands at the start of flash sector
5, and the vector table at file offset `0x200` lands at `0x08020200`
— naturally 512-B-aligned for VTOR. Image spans sector 5 + part of
sector 6 (ends ~`0x080556A0`, ~213 KB).

See `docs/hardware.md` for the canonical binary identity, the envelope
format, the MCU identification work, and the version-history note that
picks 1.07.06 as the baseline.

## Decomp scope policy

Mainware is **VanMoof's own application** written by VanMoof (or
VanMoof contractors) on top of the Muco runtime + ST CubeF4 HAL. The
working policy is identical to `mainboot`:

- **Translate every function** that has observable behaviour
  (skip 2-byte `b .` trap stubs — they get listed as
  `decomp-asm`, one shared source line).
- **Recognise ST CMSIS / HAL / LL** stock functions when they
  appear and mark them `vendor-stock`. Mainware almost certainly
  links against `HAL_FLASH_*`, `HAL_UART_*`, `HAL_CRC_*`,
  `HAL_GPIO_*`, `HAL_TIM_*`, and a chunk of CMSIS-Core
  intrinsics. The build pulls those from a vendored Cube tree
  later.
- **Recognise Muco runtime** functions shared with `mainboot` —
  `systick_tick`, `scheduler_tick`, `systick_delay`, the
  RCC-reset and CRC helpers. These also become `vendor-stock`
  rather than getting re-decoded from scratch; same code, same
  expected names, just embedded in a different image.
- **Recognise libc** (`memcpy`, `memset`, `strlen`, `strcmp`,
  `printf` family) — these are pulled in from `arm-none-eabi-newlib`
  by the Cube build. Mark `vendor-stock` and supply from the
  vendored libc.

The bespoke layer worth understanding deeply is the **application
layer**: the super-loop / scheduler structure, the BLE / Modbus
command tables, the power-state machine, the modem driver, the
per-subsystem updater flows, and the bike-state model.

## Summary

| Count | Status |
| --- | --- |
| 805 | pending (auto-named `FUN_xxxxxxxx`) |
| 0   | vendor-stock |
| 0   | in-progress |
| 1   | decomp-c |
| 0   | decomp-asm |
| 3   | named (rename in Ghidra, no source yet) — `Reset_Handler`, `SysTick_Handler`, `scheduler_tick` |

`function_count = 809` per `ghidra/exports/mainware_program.json`
(refresh after every mutating Ghidra run; see top-level `CLAUDE.md`).

## Per-module decomp log

- `systick.c` — `systick_tick`. Identical shape to mainboot's
  `systick_tick`: increment `g_systick_counter` (uint32) by
  `g_systick_step` (uint8) on every SysTick interrupt. The step byte
  lives at SRAM `0x20000014` — **same address as in mainboot**, since
  both wares' Muco runtime starts `.data` at that offset. The counter,
  however, lives at SRAM `0x20009704` in mainware (vs `0x2000083C` in
  mainboot) — the same Muco library linked into a bigger image gets
  a different `.bss` placement. The instruction stream is byte-shape
  identical to mainboot (load counter-ptr, load counter, load step-ptr,
  ldrb step, add, str, bx lr); only the two literal-pool entries
  differ. The mainware **SysTick_Handler** at `0x0803ca14` is the
  Muco wrapper: `bl scheduler_tick; bl systick_tick`, with
  `scheduler_tick` at `0x080306d8` — a 48-slot version of mainboot's
  16-slot scheduler (table at SRAM `0x200004C0`, with the bitmap +
  callbacks + counters scaled up). Both `SysTick_Handler` and
  `scheduler_tick` are named but not yet sourced; the scheduler's
  larger table size means it can't share a `.c` file with mainboot's.

## Functions

### Decoded

| Address | Size | Name | Source file | Notes |
| --- | --- | --- | --- | --- |
| `0x080232e0` | 14 | `systick_tick` | `src/systick.c` | `g_systick_counter += g_systick_step`; counter at SRAM `0x20009704`, step at SRAM `0x20000014` (shared `.data` offset with mainboot) |

### Vendor-stock (recognised, no decomp needed)

| Address | Size | Name | Source |
| --- | --- | --- | --- |

### Named (no source yet)

| Address | Size | Name | Why named |
| --- | --- | --- | --- |
| `0x08043E54` |  72 | `Reset_Handler` | vector slot 1 target (thumb addr `0x08043E55`) |
| `0x0803ca14` |  12 | `SysTick_Handler` | vector slot 15 target; body is `bl scheduler_tick; bl systick_tick` |
| `0x080306d8` |  96 | `scheduler_tick` | called from `SysTick_Handler`; 48-slot one-shot timer/callback dispatcher (Muco runtime, scaled from mainboot's 16) — table at SRAM `0x200004C0`, bitmap at `+0x08`, callbacks at `+0x10`, counters at `+0xD0` |

### Pending decomp targets (small leaves to look at next)

| Address | Size | Notes |
| --- | --- | --- |
| `0x0803c974` | 12 | NMI_Handler — calls a diagnostic logger via fn-pointer at SRAM `0x20009D98` with a string arg, then returns (does NOT loop). Pattern shared by slots 2/11/12/14. |
| `0x0803c99c` | 12 | MemManage_Handler — same dispatcher pattern, but ends in `b .` (infinite loop). Shared with slots 4/5/6. |
| `0x0803cb6c` | 166 | Fault dumper called by HardFault_Handler — reads R0-R12/LR/PC/xPSR from stacked frame + reads SCB CFSR/HFSR/DFSR/MMFAR/BFAR/AFSR, prints each via the dispatcher; ends `b .`. |
| `0x0803c988` | 18 | HardFault_Handler — `tst lr,#4` to pick MSP vs PSP, branches to the fault dumper above. |
| `0x080306d8` | 96 | scheduler_tick — 48-slot scheduler dispatch (Muco, scaled from mainboot 16). Behaviour-equivalent to mainboot's `scheduler.c` but cannot share source — different table size. |

Full list in `ghidra/exports/mainware_program.json` once generated.

## Open questions

- Exact mainware flash slot — `0x08040000` is the working hypothesis
  from VTOR alignment; confirm from `mainboot`'s "Jump to App" code
  path.
- VTOR set by mainboot before jump — must equal the slot base
  `0x08040000` (or the per-slot equivalent) for the table at file
  offset `0x200` to dispatch correctly.
- FPU usage — does any function emit `vpush`/`vpop` (would require
  switching to `-mfloat-abi=hard -mfpu=fpv4-sp-d16`)?
- Modem command flow — is the uBlox SARA driver a clean state machine
  or a soup of inline `printf`+`expect` calls?
- BLE-side protocol — the bleware on the CC2642 talks to mainware over
  Modbus (same bus the shifter and motor use)? Or a separate UART /
  SPI link?
- What is at file offset `0x010..0x028` exactly — the ASCII build
  date+time looks like literal `__DATE__` + `__TIME__` placed at a
  known location by the linker script. Confirm by inspecting the
  early `.text` for a reference to `0x08040010`.
