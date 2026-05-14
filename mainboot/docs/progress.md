# mainboot — decomp progress

Target binary: `muco-boot.bin` (32768 bytes, ARM Cortex-M4, STM32F469/F479,
320 KB SRAM). Loaded into Ghidra at `0x08000000`. The image is a
third-party **Muco Technologies** STM32F4 bootloader (banner
`"STM32 bootloader <%X.%02X> Muco Technologies (c)2019"`) that VanMoof
licensed for the main-controller first-stage. It manages multiple
firmware slots (Loaded / Shadow / Shifter / Motor / Battery
Application) with CRC-verified A/B updates.

See `docs/hardware.md` for the canonical binary identity (size, SHA
hashes), the MCU identification work, and the Muco provenance note.

## Decomp scope policy

The shifterboot rule ("decode only VanMoof-custom code") does
**not** apply cleanly here. The whole binary is third-party (Muco
Technologies) — there is no VanMoof-custom inner core to focus on.
We still want a buildable reconstruction, so the working policy is:

- **Translate every function** that has observable behaviour
  (skip 2-byte `b .` trap stubs — they get listed as
  `decomp-asm`, one shared source line).
- **Recognise ST CMSIS / HAL / LL** stock functions when they
  appear (they will — Muco almost certainly used STM32 CubeF4 HAL
  underneath their loader). Mark those `vendor-stock` and let the
  build pull them from a vendored Cube tree later.
- **Surface, but do not separately re-implement, ARM CMSIS-Core
  intrinsics** (`__DSB`, `__ISB`, `NVIC_SystemReset`, etc.) —
  these will be `vendor-stock` like in shifterboot.

The bespoke layer worth understanding deeply is the **image-table
format and integrity scheme**: what the loaded/shadow/per-subsystem
slots look like in flash, what fields the version banners are
reading, what CRC polynomial+seed the integrity check uses, and
how mainboot hands per-subsystem blobs off (probably over Modbus
to the eShifter/motor/battery MCUs).

## Summary

| Count | Status |
| --- | --- |
| 170 | pending (Muco — awaiting decomp) |
| 1   | vendor-stock (recognised; ST/ARM CMSIS) — `NVIC_SetPriorityGrouping` |
| 0   | in-progress |
| 7   | decomp-c |
| 1   | decomp-asm |
| 1   | named (rename in Ghidra, no source yet) — `SysTick_Handler` |

`function_count = 180` per `ghidra/exports/mainboot_program.json`
(refresh after every mutating Ghidra run; see top-level CLAUDE.md).

## Per-module decomp log

- `rcc.c` — `rcc_reset_all_peripherals` and the empty
  `rcc_post_reset_hook` it calls. Resets every peripheral on every
  bus (APB1 → APB2 → AHB1 → AHB2 → AHB3) by writing
  `0xFFFFFFFF` then `0` to each `*RSTR`. Returns 0.
- `scheduler.c` — `scheduler_tick`. 16-slot one-shot timer/callback
  dispatcher invoked by `SysTick_Handler` before `systick_tick`.
  The table lives in `.bss` at SRAM `0x2000038C` (literal pool entry
  at `0x0800389c`); layout is a 16-bit `enabled_mask` at `+0x04`,
  sixteen callback pointers at `+0x08..+0x47`, sixteen counters at
  `+0x48..+0x87` (`0x88` bytes total). Per enabled slot: if the
  counter is non-zero it is decremented; if the decremented value
  equals 1 the slot's callback is invoked (so the callback fires
  exactly on the `2 → 1` transition). GCC produces 52 B vs OEM's
  90 B — the OEM compiler recomputes `base + i*4` on every field
  access where GCC hoists the table base into one register and walks
  it forward by `+4` each loop. Behaviour-equivalent, not
  byte-equivalent. First-bytes of the registration API are very
  likely `FUN_080006a0` / `FUN_080006c8`, both of which sit right
  next to `systick_tick` and weren't classified yet.
- `systick.c` — `systick_get_count` and `systick_delay`.
  `systick_get_count` is the trivial 6-byte getter that returns
  `*g_systick_counter` (literal at `0x0800069c` =
  `0x2000083C`) — byte-equivalent to OEM. `systick_delay(ticks)`
  is the busy-wait used throughout the bootloader: sample
  `systick_get_count` for `start`, add `g_systick_step` to
  `ticks` (skipped iff `ticks == 0xFFFFFFFF`, the OEM
  effectively-infinite sentinel), then spin until
  `current - start >= ticks`. The `+= step` is the standard
  "round up to the next tick" guard so callers get a *minimum*
  of `ticks` periods even if SysTick fires just after the start
  sample. GCC `-Os` emits 28 B vs OEM's 34 B (GCC uses IT-blocks
  to predicate the adjustment; OEM uses an explicit branch) —
  behaviour-equivalent.
- `systick.c` — `systick_tick`. Increments the `g_systick_counter`
  free-running counter at SRAM `0x2000083C` (.bss) by the
  `g_systick_step` byte at SRAM `0x20000014` (.data, initialised
  to 1). Called from `SysTick_Handler` (OEM `FUN_08003754` —
  a 12-byte handler that first calls a 90-byte scheduler
  dispatcher at `FUN_08003840`, then `systick_tick`). The OEM
  function is byte-shape but not byte-equivalent: GCC schedules
  the two pool loads back-to-back, OEM interleaves load-addr +
  deref per variable, so 6 of 14 instruction bytes differ
  (literal pool and outer shape match).
- `string.c` — `strlen`. Canonical Thumb-2 `strlen` with the
  `ldrb.w rN, [rM], #1` post-indexed-byte-load idiom, returning
  `(end - start) - 1`. C `-Os` reproduces the OEM bytes
  identically (all 16 bytes match `0x08000520..0x0800052F`).
- `dead_stubs.S` — `dispatch_disabled_stub`. A 16-byte function
  whose guard literal is hard-coded to zero, so the body never
  runs. The body would have loaded a function pointer
  (`0x080055EC`) and a SRAM context pointer (`0x20000200`) and
  called the function — but the `bl` has been replaced with a
  4-byte `nop.w`, leaving the symbol in place for two real call
  sites inside the main loop (in `FUN_08004254`). Written in
  asm because byte-equivalence requires preserving the
  literal-pool entries and the wide NOP. Assembles to 28 bytes
  that match the OEM byte-for-byte.

## Functions

### Decoded

| Address | Size | Name | Source file | Notes |
| --- | --- | --- | --- | --- |
| `0x08000204` | 16 | `dispatch_disabled_stub`    | `src/dead_stubs.S` | guard==0 → cbz always fires; body is a NOP'd-out `bl`; symbol kept for 2 callers in main loop |
| `0x08000520` | 16 | `strlen`                    | `src/string.c`     | canonical Thumb-2 `strlen` (post-indexed `ldrb.w`); byte-equivalent to OEM |
| `0x080005d0` | 2  | `rcc_post_reset_hook`       | `src/rcc.c`        | empty `bx lr`; placeholder hook Muco never filled in |
| `0x080005d4` | 38 | `rcc_reset_all_peripherals` | `src/rcc.c`        | pulse-resets all peripherals via the five RCC `*RSTR` registers, returns 0 |
| `0x0800067c` | 14 | `systick_tick`              | `src/systick.c`    | `g_systick_counter += g_systick_step`; called by `SysTick_Handler` |
| `0x08000694` | 6  | `systick_get_count`         | `src/systick.c`    | trivial getter; returns `g_systick_counter` — byte-equivalent to OEM |
| `0x080006a0` | 34 | `systick_delay`             | `src/systick.c`    | busy-wait `ticks` SysTick periods; `0xFFFFFFFF` is the OEM forever-sentinel |
| `0x08003840` | 90 | `scheduler_tick`            | `src/scheduler.c`  | 16-slot one-shot timer/callback dispatcher; table at SRAM `0x2000038C` |

### Vendor-stock (recognised, no decomp needed)

| Address | Size | Name | Source |
| --- | --- | --- | --- |
| `0x080006c8` | 32 | `NVIC_SetPriorityGrouping` | ARM CMSIS-Core M4 `core_cm4.h` standard `__STATIC_INLINE` — clears PRIGROUP (`AIRCR[10:8]`) + VECTKEYSTAT, ORs in `(param & 7) << 8` and the `0x5FA` VECTKEY |

### Named (no source yet)

| Address | Size | Name | Why named |
| --- | --- | --- | --- |
| `0x08003754` | 12 | `SysTick_Handler` | vector slot 15 target; body is `bl <scheduler>; bl systick_tick` |

### Pending decomp targets (small leaves to look at next)

| Address | Size | Notes |
| --- | --- | --- |

Full list in `ghidra/exports/mainboot_program.json` (174 still
auto-named `FUN_xxxxxxxx`).

## Open questions

- Exact MCU part (F4 F469/F479 vs F7 F745/F746/F756)?
- Where does mainboot jump to the application image? (`0x08008000`
  guessed from STM32 F4 sector layout — confirm.)
- Integrity scheme (CRC32? SHA? signature?)
- OTA delivery path — does mainboot accept image uploads itself, or
  does it leave that to the application?
- Why is the initial SP at `0x20050000` while `.bss` ends at
  `0x20000950`? — i.e., what is the upper ~315 KB of SRAM used for?
  Stack growth alone is unlikely to need that much; probably an
  image staging area used by the upload path.
- Is there a USB DFU or YMODEM path baked into the bootloader?
