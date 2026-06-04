# mainware — hardware notes

The main controller application — the bike's central firmware. Runs on
the same MCU as `mainboot` (ST STM32F413VGT6, Cortex-M4F) and is the
first-stage's "Loaded Application" slot tenant. Mainware owns user
interaction (BLE app dialog, kick-lock, frame-lock, horn, sounds,
power-mode), orchestrates the per-subsystem updaters (Shifter, Motor,
Battery firmware push), and runs the cloud-comms uplink (uBlox modem
`AT+UHTTPC` to the VanMoof backend).

## Binary identity

| | |
| --- | --- |
| File | `mainware_1.07.06.bin` |
| Size | 218784 bytes (≈ 213 KB) |
| Version | `1.07.06` (Nov  1 2021 10:25:04) |
| SHA-256 | `e041e66a7110a2bbf6882317f865bfb7d5ba293a4149470cf6367aeb2649b8a1` |
| SHA-512 | `574ebb811eda88fffae05f21560f2183ea6e97a383c36e075217910c0771e9491860a612954d7a04e6c5cc77bb4e6e60adecbed7fd88dd7463f046e2ed550e90` |

This is the *first* (oldest) shipped mainware, chosen as the baseline
to mirror shifterware's `0.237` policy — the diff between 1.07 and the
final 1.09 is non-trivial (~28 KB removed between 1.08 and 1.09,
likely the modem/cloud path being torn out), so 1.07 is the right
starting point to see the system at its most complete.

## VanMoof container envelope

The `.bin` is not a raw flashable image — it has a 16-byte VanMoof
envelope plus a 496-byte build-info / padding region. The full first
512 bytes (file offset `0x000..0x1FF`) are still **flashed** at the
target slot — they sit *before* the vector table so that `mainboot`
can validate the slot and print the build-date banner without parsing
DWARF.

| File offset | Size | Field |
| --- | --- | --- |
| `0x000` | 4 | Magic — file bytes `55 AA 55 AA` (i.e. a little-endian `uint32` of `0xAA55AA55`; reproduced exactly by `startup_stm32f413.S`). Same magic across every VanMoof firmware image. |
| `0x004` | 1 | `0xF4` (unknown — constant across all four mainware versions) |
| `0x005` | 1 | Patch version (`0x06` for 1.07.06) |
| `0x006` | 1 | Minor version (`0x07` for 1.07.06) |
| `0x007` | 1 | Major version (`0x01` for 1.07.06) |
| `0x008` | 4 | CRC32 (VanMoof poly — see `vanmoof/crc.go`) |
| `0x00C` | 4 | Total image length (little-endian; equals file size) |
| `0x010` | 12 | Build date as ASCII (`"Nov  1 2021\0"`) — `__DATE__` literal |
| `0x01C` | 9  | Build time as ASCII (`"10:25:04\0"`) — `__TIME__` literal |
| `0x025` | 475 | `0xFF` padding up to the 512-byte VTOR alignment boundary |
| `0x200` | …   | STM32 vector table (initial SP, Reset, NMI, …) — start of code-image proper |

The version byte order (patch / minor / major) is the same as the
shifterware version-word encoding observed in the loader.

## MCU

**ST STM32F413VGT6** — confirmed from the `mainboot/docs/hardware.md`
identification work: Cortex-M4F, 100-pin LQFP, 1 MB flash, 320 KB
contiguous SRAM (`0x20000000..0x2004FFFF`).

Build flags: `-mcpu=cortex-m4 -mthumb`. The image's initial SP is
`0x20037000` (≈ 220 KB above SRAM base — leaves the upper 100 KB of
SRAM2 free; mainboot uses the same SRAM region for image staging
during updates). Whether to use `-mfloat-abi=hard -mfpu=fpv4-sp-d16`
will be decided once an FPU-touching function shows up; default to
soft float for now.

## Memory map (provisional)

| Region | Start | End | Use |
| --- | --- | --- | --- |
| Flash (mainboot)          | `0x08000000` | `0x08007FFF` |  32 KB — first-stage (sectors 0+1) |
| Flash (subsystem blobs / shadow) | `0x08008000` | `0x0801FFFF` |  96 KB — shadow app + shifter/motor/battery blobs (TBC) |
| Flash (mainware envelope) | `0x08020000` | `0x080201FF` | 512 B — 16 B header + build-date + padding (start of sector 5) |
| Flash (mainware code)     | `0x08020200` | `0x080556A0` | ~213 KB — vector table + .text + .rodata, spans sectors 5 + part-of-6 |
| Flash (free)              | `0x08055700` | `0x0807FFFF` | ~170 KB unused at top of flash (room for growth / per-MCU OTA cache) |
| SRAM (mainware)           | `0x20000000` | `0x20036FFF` | working set; initial SP at `0x20037000` |

### Known SRAM globals (from decomp)

| Address | Size | Symbol | Module | Notes |
| --- | --- | --- | --- | --- |
| `0x2000010E` | ≥6 | `g_state` | (not yet decoded) | Status/console block. `g_state[5]` (byte at `0x20000113`) is the login state machine: `0xFA` = ready-to-accept password, non-`0xFA` = locked-out / scheduler-slot id. |
| `0x20000000` | 4 | `g_boot_marker` | `main` | Warm-boot magic. `main` compares it against `0x55AA55CF`; match → `boot_init_warm` (skip cold init), else `boot_init_cold`. Lives in the **retained low-RAM** below `.data` (`0x20000000..0x20000013` — not touched by the `.data` copy or `.bss` zero), so it survives a warm reset. |
| `0x20000014` | 1 | `g_systick_step` | `systick.c` | Muco-runtime SysTick increment-per-tick (initially 1). **Same SRAM address as in mainboot** — both wares' `.data` starts at +0x14 from SRAM base. |
| `0x20000076` | 1 | `g_update_mode` | `app.c` | subsystem firmware-update mode (`+1` of a small control block at `0x20000075`); `update_mode_request` only overwrites it from idle (`==2`). |
| `0x20000288` | ≥7 | `g_announce` | `app.c` | broadcast dirty-flags block; `announce_mark` sets `+5` (channel 0) / `+6` (channel 1). |
| `0x200004C0` | 0x190 | `g_scheduler` | `scheduler.c` | Muco 48-slot one-shot scheduler table. **Two** 6-byte bitmaps: `allocated` at +0x00 (set by `scheduler_alloc`, cleared by `scheduler_release`) and `armed` at +0x08 (set by `scheduler_start`, cleared by `scheduler_release`, scanned by `scheduler_tick`); `callbacks[48]` at +0x10, `counters[48]` at +0xD0. |
| `0x200083A8` | ≥0x404 | `g_ctx` | `main` / `console.c` | the application/session context struct (`session_ctx`). `main`'s super-loop addresses it directly at this fixed address; the console reaches the same object through `g_app_state.ctx_sub`. Known fields: audio block `+0xF4..+0x10C`, volume `+0x104/5/6`, `[0x2D9]` logged-in, `[0x2E0]` fail-count, `[0x398]` service password, `[0x3D4]` SOC override; super-loop also touches `[0x34D]`, `[0x402]`, `[0x350-0x354]`, `[0x3B0]`, `[0x3B8]`, `[0x3C0-0x3C6]` (semantics TBD). |
| `0x20009368` | 4 | `g_app_ctx` | (not yet decoded) | application-state block; `+0x2DC` is `ctx_sub`, the pointer to `g_ctx` (`0x200083A8`). Through it the console reaches `[0x2D9]` logged-in flag, `[0x2E0]` failed-login counter, `[0x398]` service password. |
| `0x20009704` | 4 | `g_systick_counter` | `systick.c` | free-running SysTick counter |
| `0x20009D98` | 4 | `g_log_func` | `log.h` (extern) | `(*g_log_func)(const char *fmt, …)`-style logger. Referenced by `exceptions.c`, `panic.c`, `console.c`. Set once during init (initialiser not yet decoded). |

### C runtime (startup) layout

`Reset_Handler` (`0x08043E54`) is the standard CubeF4 reset stub. Its
literal pool fixes the linker symbols the future `startup_stm32f413.S` +
linker script must reproduce:

| Symbol | Value | Meaning |
| --- | --- | --- |
| `_estack`  | `0x20037000` | initial SP (= vector slot 0) |
| `_sidata`  | `0x08055534` | `.data` load address (flash, after `.text`/`.rodata`) |
| `_sdata`   | `0x20000014` | `.data` start (SRAM) — first word is `g_systick_step` |
| `_edata`   | `0x20000180` | `.data` end (`.data` is 0x16C B) |
| `_sbss`    | `0x20000180` | `.bss` start (= `_edata`) |
| `_ebss`    | `0x20009DB0` | `.bss` end (`.bss` is ~40 KB) |

After copying `.data` and zeroing `.bss`, `Reset_Handler` paints the whole
free-RAM region `[_ebss 0x20009DB0, _estack 0x20037000)` (~180 KB) with the
word `0x0000000E` — a stack/heap fill pattern (high-water-mark groundwork) —
then calls, in order:

1. `SystemInit` (`0x08043AA4`) — FPU enable + RCC reset + VTOR (see above).
2. `__libc_init_array` (`0x08020DF8`) — newlib CRT: preinit array, `_init`,
   init array.
3. `main` (`0x0803DEA8`, 613 B) — the application super-loop.

`.data` ending at `0x20000180` and `.bss` running to `0x20009DB0` means the
statically-initialised + zero-init working set is ~40 KB; the scheduler
table (`0x200004C0`) and the console/app-state globals all fall inside that
`.bss` span, consistent with their addresses.

STM32F4 1 MB sector layout: sectors 0..3 = 16 KB each, sector 4 = 64 KB,
sectors 5..7 = 128 KB each. Mainware's 213 KB image starts at sector
5 (`0x08020000`) — the first 512 bytes of that sector are the VanMoof
envelope (container header + `__DATE__`/`__TIME__`), and the STM32
vector table begins at offset `0x200` into the sector
(`0x08020200`). That 512-B prefix lets mainboot validate the magic
and print the build banner before bothering with VTOR.

## Vector table (head, from raw bytes — image @ flash `0x08020200`)

| Slot | Vector | Value | Note |
| --- | --- | --- | --- |
| 0  | initial SP | `0x20037000` | mid-SRAM stack base |
| 1  | Reset      | `0x08043E55` | thumb (low bit set) |
| 2  | NMI        | `0x0803C975` | distinct handler |
| 3  | HardFault  | `0x0803C989` |  |
| 4  | MemManage  | `0x0803C99D` | M4 fault — real on this part |
| 5  | BusFault   | `0x0803C9B1` |  |
| 6  | UsageFault | `0x0803C9C5` |  |
| 11 | SVCall     | `0x0803C9D9` |  |
| 12 | DebugMon   | `0x0803C9ED` |  |
| 14 | PendSV     | `0x0803CA01` |  |
| 15 | SysTick    | `0x0803CA15` |  |

The 9 system-exception handlers at `0x0803C975..0x0803CA15` sit on
20-byte boundaries — distinct functions, not the shared-trap pattern
seen in mainboot. That's consistent with an application built against
ST's CubeF4 startup template, which gives every exception its own
named handler (most just `while(1);`-loops).

These are now decoded into `src/exceptions.c`. Each logs its own name
through `g_log_func`: `NMI`/`SVC`/`DebugMon`/`PendSV` log and return;
`MemManage`/`BusFault`/`UsageFault` log and spin. `HardFault` is a naked
tail-call (`tst lr,#4; ite eq; mrs r0,msp/psp; b.w fault_dump`) into the
frame dumper at `0x0803CB6C`, which prints the 8-word stacked frame and
the SCB fault-status/fault-address registers before spinning:

| Register | Address | Label in dump |
| --- | --- | --- |
| CFSR  (Configurable Fault Status) | `0xE000ED28` | `CFSR = %x` |
| HFSR  (HardFault Status)          | `0xE000ED2C` | `HFSR = %x` |
| DFSR  (Debug Fault Status)        | `0xE000ED30` | `DFSR = %x` |
| MMFAR (MemManage Fault Address)   | `0xE000ED34` | `MMAR = %x` (OEM drops the F) |
| BFAR  (BusFault Address)          | `0xE000ED38` | `BFAR = %x` |
| AFSR  (Auxiliary Fault Status)    | `0xE000ED3C` | `AFSR = %x` |

`SysTick_Handler` (`0x0803CA14`) is the Muco tick wrapper:
`scheduler_tick()` then `systick_tick()`. The Muco runtime's fatal-assert
path `muco_assert_fail` (`0x0803DAC4`, `src/panic.c`) shares the same
`g_log_func` slot — it prints `"FATAL error File [%s] line [%d]"` and
spins; the independent IWDG reboots the board.

## Banner strings of interest (first pass)

These show the mainware's role and which subsystems it speaks to:

- `'MT' (@) 2019 STM32F4, Start` — Muco runtime banner (re-used from
  mainboot's library; mainware links against the same Muco runtime).
- `Motorpcb Application: v%x.%02x.%02X (%s %s)` — version report from
  the motor MCU (TI TMS320F28054F via Modbus).
- `../src/F2806/f2806x.c` — TI motor MCU driver code path.
- `Autobaud no answer`, `Err Autobaud [%d]` — UART autobauding to a
  peripheral (probably the uBlox modem).
- `AT+UHTTPC=0,5,"/bike-message","https",…` — uBlox SARA modem HTTP
  POST to VanMoof backend.
- `ASK APP to unlock` — BLE handshake with the iPhone/Android app.
- `Cartridge removed`, `Locked wake by mems`, `Wake from shipping` —
  power-management state machine, MEMS-based wake detection.
- `Charging liPo %d%%`, `External battery removed`, `Restore power level %d`
  — battery management.
- `PRESS_VERY_LONG BLE off`, `SOUND_S%c vol %d`, `Clear horn queue` —
  user-interface inputs and audio output.

## Provenance

Unlike `mainboot` (third-party Muco runtime + application logic
written by Muco for VanMoof), mainware is **VanMoof's own
application** built on top of the Muco runtime / Cube HAL. The
F2806/CC2642/STM32L0 subsystem drivers are clearly bespoke (no
upstream equivalent). The HAL layer underneath will be ST CubeF4 —
`HAL_FLASH_*`, `HAL_UART_*`, `HAL_CRC_*`, `HAL_GPIO_*`, etc. — and
gets marked `vendor-stock` when recognised, same as in `mainboot`.

## Open questions

- Exact flash slot layout (where do shifter/motor/battery blobs live,
  in what order, with what envelopes?).
- VTOR — **resolved.** `SystemInit` (`0x08043AA4`) writes the stock
  `VTOR = 0x08000000`, but `main` (`0x0803DEA8`) **re-points it on its
  very first instruction**: `SCB->VTOR = 0x08020200`. So the live vector
  table is mainware's own at `0x08020200`, as documented; there's just a
  brief window during early init (before `main`) where VTOR still points
  at mainboot's table.
- FPU usage — **answered: yes.** `SystemInit` sets `SCB->CPACR |=
  0xF00000` (CP10/CP11 full access), enabling the FPU. None of the
  *currently decoded* functions emit `vpush`/`vpop`, so they build fine
  under soft-float, but the materialised image will need
  `-mfloat-abi=hard -mfpu=fpv4-sp-d16` once an FP-using function is
  decoded.
- Modem AT command flow — is there a YMODEM-over-AT path, or only
  HTTP POSTs?
- BLE protocol with the CC2642 (which is itself running bleware) —
  probably Modbus framing same as the motor / shifter side.
