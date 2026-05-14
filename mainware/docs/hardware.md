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
| `0x000` | 4 | Magic `0x55AA55AA` (little-endian; same magic used by every VanMoof firmware image) |
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
| `0x20000014` | 1 | `g_systick_step` | `systick.c` | Muco-runtime SysTick increment-per-tick (initially 1). **Same SRAM address as in mainboot** — both wares' `.data` starts at +0x14 from SRAM base. |
| `0x200004C0` | ~400 | `g_scheduler` | (not yet decoded) | Muco 48-slot one-shot scheduler table — `enabled_mask` bitmap at +0x08, callbacks at +0x10, counters at +0xD0. |
| `0x20009704` | 4 | `g_systick_counter` | `systick.c` | free-running SysTick counter |
| `0x20009D98` | 4 | `g_log_func` | (not yet decoded) | function pointer used by every system-exception handler — `(*g_log_func)(const char *fmt, …)`-style logger. |

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
- VTOR value mainboot writes before jumping — confirms `0x08020200`.
- FPU usage — does any function emit `vpush`/`vpop`?
- Modem AT command flow — is there a YMODEM-over-AT path, or only
  HTTP POSTs?
- BLE protocol with the CC2642 (which is itself running bleware) —
  probably Modbus framing same as the motor / shifter side.
