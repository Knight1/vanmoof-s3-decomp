# shifterboot — decomp progress

Target binary: `shifterboot.bin` (6144 bytes, ARM Cortex-M0, MM32F031F6U6).
Loaded into Ghidra at `0x08000000`. The bootloader sits at the base of
flash and (presumably) jumps to the application at some higher offset
once an integrity check passes.

See `docs/hardware.md` for the canonical binary identity (size, SHA hashes)
and the MindMotion BSP lineage analysis.

## Decomp scope policy

**Decode only VanMoof-custom code.** Functions that are byte-for-byte
copies of canonical vendor sources (MindMotion BSP, MindMotion HAL,
ARM CMSIS) are *recognised* and marked `vendor-stock` — no separate C
translation, no source file in `src/`. The byte-equivalent build will
later pull these in from a vendored copy of the MindMotion BSP, but
the decomp work itself focuses on the bespoke parts of the bootloader.

## Summary

| Count | Status |
| --- | --- |
| ?? | pending (VanMoof-custom, awaiting decomp) |
| 6  | vendor-stock (recognised; no decomp needed) |
| 0  | in-progress |
| 27 | decomp (asm or c) |
| 4  | named (rename in Ghidra, no source yet) |

`function_count = 78` per `ghidra/exports/shifterboot_program.json`.
The pending count will tighten as more functions are classified as
`vendor-stock` vs `custom`.

## Per-module decomp log

- `startup_mm32f031.S` — `Default_Handler` (custom: 2-byte `b .` stub);
  `_cold_reset` + `thunk_main` (VanMoof's 20-byte `__main` replacement
  packed into the unused vector-table tail).
- `systick.c` — `systick_tick`, `SysTick_Handler`, and `mdelay`.
  The stock MindMotion template has `b .` for SysTick; VanMoof
  installed a countdown decrementer that drives a millisecond-delay
  variable at SRAM `0x20000010`.
  - `mdelay` (20 B @ `0x080014CA`) — single-arg millisecond delay.
    Stores `ms` into `g_systick_countdown` and spins until
    `SysTick_Handler` ticks it down to zero. Sole OEM caller is
    `main` at `0x080002E2` with `ms = 250` (a 250 ms pause inside
    the image-sync chain). The OEM compiles with a stack-spill
    pattern (`push {r0, lr}; ldr r0, [sp]; ... pop {r3, pc}`) that
    `-O0` emits to give every parameter a stack slot; semantics are
    register-equivalent and `-Os` skips the spill.
- `uart.c` — `uart1_send_byte` (28 B @ `0x080000C8`) and
  `uart1_send_buf` (28 B @ `0x080000E4`). VanMoof-custom wrappers
  around MindMotion HAL `USART_SendData` / `USART_GetFlagStatus`;
  the byte-send spins on USART SR bit 0 (TX-ready/complete on MM32),
  and the buffer-send is a decrement-then-test loop with a
  `uxth`-clamped 16-bit length. Build-incomplete state: the HAL
  primitives are declared `extern` in `uart.c` and resolve once the
  MindMotion BSP is vendored in. Compile-clean (-Wall -Wextra
  -Wpedantic -Wshadow); link still produces undefined references
  for the HAL leaves and the missing `Reset_Handler` chain, as
  expected.
- `image.c` — halfword-stream helpers used by `main` to parse the
  image header during OTA verification.
  - `image_read_halfword` (6 B @ `0x080014F8`) — single `*p` halfword
    load. Called by `image_copy_halfwords` and once directly from
    `main` at `0x08000260`.
  - `image_copy_halfwords` (34 B @ `0x08001554`) — `memcpy` for
    halfword-aligned buffers, `count` is `uint16_t` (the OEM emits
    `uxth` on the index each iteration). Also called from
    `FUN_080016A6` (pending).
  - `image_read_u32_le` (28 B @ `0x080001BC`) — assembles a 32-bit
    little-endian value from two halfword reads, leaving the
    halfwords in the caller's staging buffer. Called 6× from `main`
    (at `0x08000228`, `0x08000232`, `0x0800024A`, `0x08000270`,
    `0x080002CE`, `0x080002FA`) — almost certainly one read per
    u32 field of the VanMoof image header.
- `util.c` — `memcpy` (36 B @ `0x08001740`). Word-fast + byte tail.
  Same shape as shifterware's `memcpy` @ `0x08005D6C`. Safe to name
  `memcpy` because the build uses `-nostdlib` (no libc collision).
- `crc.c` — `crc32_words` (32 B @ `0x080013CC`). Single-word feeder
  for the MM32F031 hardware CRC peripheral at `0x40023000` (the
  literal-pool word at `0x08001400` points there). The peripheral
  uses the MPEG-2 polynomial `0x4C11DB7` with init `0xFFFFFFFF` —
  same engine shifterware's image-CRC patcher already targets.
  `CRC_ResetDR` (the matching reset helper at `0x080013AC`, 8 B) is
  vendor-stock (MindMotion HAL) and stays `extern`.
- `image.c` — `image_verify_crc` (100 B @ `0x08000158`) added.
  Validates the OTA staging image at flash `0x08001800`:
  1. magic == `0xAA55AA55` → else return `2` (header invalid).
  2. length `< 0x3000` (12 KB) → else return `2`.
  3. CRC the 40 B header with `crc` and `length` fields masked to
     `0xFFFFFFFF`, then continue the CRC over the
     `(length - 40) / 4` payload words. Compare against the stored
     `crc` field.
  Two callers: `main` at `0x0800027E` (cold-boot image check) and
  `0x08000392` (cmd-0x81 "apply image" Modbus dispatch). The OEM
  emits two dead stores after the header-CRC step (writing the
  original `crc` and `length` back into the local buffer at +8 and
  +0xC); they're never read back and `-Os` drops them. Same role
  as shifterware's `image_verify_crc` @ `0x08003AC6` (same 100 B
  size, same shape).
- `image.c` — `boot_app` (26 B @ `0x080016F0`). **Application-boot
  trampoline.** Stashes the target's Reset_Handler at SRAM
  `0x20000018` (a fixed scratch slot — pool word at `0x08001710`),
  `MSR MSP, vec[0]` to install the app's initial SP, then re-reads
  the stash and `BLX`es into Reset_Handler. The scratch round-trip
  is required because `MSR MSP` invalidates the current stack
  frame, so locals can't survive across the switch.
  Sole caller is `main` at `0x0800031C` with the argument
  `0x08004828` (= slot 2 base + `IMAGE_HDR_SIZE`). This **closes
  the boot-handoff question** we'd flagged in earlier progress
  notes — shifterboot DOES branch into another image region; the
  prior "no direct branch found" observation missed this BLX
  through a stashed function pointer. The handoff is triggered by
  the cmd-0x81 OTA-apply path, not by cold reset.
- `ota.c` — `ota_program_chunk` (78 B @ `0x08001658`). Per-chunk
  flash writer used by `main`'s cmd-0x82 Modbus OTA streaming
  loop (sole caller, at `0x080004B6`). Walks `count_bytes` of an
  inbound Modbus frame's payload starting at `frame + 11` (the
  11-byte Modbus header offset is materialised by the OEM as
  `adds r4, #0xB`), packs each consecutive byte pair as
  little-endian halfwords into the SRAM scratch buffer at
  `0x200000F2` (shared with `flash_copy_region`), then flushes
  16 halfwords (32 B, materialised as `movs r2, #0x10`) to flash
  at `dst` via `flash_program_range`. The flush size is **fixed
  at 16 halfwords** regardless of `count_bytes` — for a partial
  last chunk where `count_bytes < 32`, the scratch buffer's
  tail still contains stale halfwords from the previous call,
  which the OEM (and we) write to flash unchanged. The inner
  counters (`buf_idx`, `stream_pos`) are `uxtb`-clamped to 8
  bits.
- `flash.c` — embedded-flash unlock / lock / status-clear primitives
  plus the page-erase wrapper. Same partitioning as shifterware's
  `flash_store.c`.
  - `flash_unlock` (12 B @ `0x080006B0`) — KEY1 (`0x45670123`) +
    KEY2 (`0xCDEF89AB`) to `FLASH->KEYR`. Callers:
    `flash_erase_page` and the still-pending flash-programmer
    wrapper at `FUN_080014FE`.
  - `flash_lock` (14 B @ `0x080006BC`) — `FLASH->CR |= 1<<7`. Same
    two callers as `flash_unlock`.
  - `flash_clear_status` (6 B @ `0x08000BDE`) — `FLASH->SR = bits`.
    Used 2× in each of `flash_erase_page` and `FUN_080014FE`
    (pre-op clear of PGERR/WRPRTERR/EOP, post-op clear of EOP).
  - `flash_erase_page` (32 B @ `0x08001534`) — wrapper:
    `flash_unlock` → `flash_clear_status(PGERR|WRPRTERR|EOP)` →
    `flash_do_page_erase(page_addr)` (still pending @ `FUN_08000746`)
    → `flash_clear_status(EOP)` → `flash_lock`. Sole caller is
    `flash_erase_pages` (the multi-page iterator below).
    The OEM discards the inner erase's return value; we mirror
    that (cast to `(void)`).
  - `flash_do_page_erase` (72 B @ `0x08000746`) — inner page-erase.
    Sole caller is `flash_erase_page`. Polls `flash_wait_status`
    for `READY`, sets PER + AR + STRT, polls again, then clears
    PER via `FLASH->CR &= 0x1FFD`. Returns one of `FLASH_ST_*`
    (cannot actually return BUSY — preserved dead-but-harmless
    branch matches OEM bytes). Byte-size-equivalent to the OEM
    (56 B body + 16 B pool = 72 B total).
  - `flash_get_status` (54 B @ `0x080006CA`) — decodes
    `FLASH->SR` into a `FLASH_ST_*` code with priority BSY >
    PGERR > WRPRTERR > READY. Isolates BSY via `lsls #0x1F;
    lsrs #0x1F`; isolates PGERR/WRPRTERR via `ands` against
    `4` / `0x10`.
  - `flash_busy_step` (26 B @ `0x08000700`) — `volatile int i = 0;
    i = 0xFF; while (i) i--;`. The double-initial-store and
    stack-slot access pattern in the disassembly only makes
    sense if `i` is volatile (otherwise gcc folds the
    assignment).
  - `flash_wait_status` (44 B @ `0x0800071A`) — polls
    `flash_get_status` until !BUSY or `timeout == 0`. On
    timeout-exhaustion writes `FLASH_ST_TIMEOUT` (5) — the OEM
    does this unconditionally on `timeout == 0`, so a valid
    non-BUSY status read on the final iteration is overwritten
    to TIMEOUT. Preserved.

  **Discipline note (2026-05-19)**: an earlier revision of this
  file imported the `FLASH_ST_*` enum values from shifterware
  by analogy without grounding them in shifterboot's own
  disassembly. The values were subsequently verified directly
  from shifterboot's `flash_get_status` body (which loads `1`,
  `2`, `3`, `4` per cascade branch) and `flash_wait_status`
  (which writes `5` on timeout); the source comments were
  rewritten to cite shifterboot's own evidence. Cross-firmware
  comparisons in this repo stay strictly as "post-decomp
  confirmation", not as "decomp inputs".
  - `flash_program_halfword` (64 B @ `0x08000946`) — inner halfword
    writer. Caller is expected to have already unlocked the flash
    and cleared the sticky SR bits. Polls `flash_wait_status(15)`
    (short budget — programming a halfword is fast), sets PG, writes
    `*(uint16_t *)addr = value`, polls again, clears PG via
    `FLASH->CR &= 0x1FFE`. The OEM materialises `0x1FFE` as
    `0x1FFD + 1` (`adds r1, #1`) so the literal pool word from the
    page-erase path is shared. Returns `FLASH_ST_*`.
  - `flash_program_range` (54 B @ `0x080014FE`) — `unlock` →
    `clear PGERR|WRPRTERR|EOP` → loop calling
    `flash_program_halfword(dst + i*2, src[i])` with `i` `uxth`-clamped
    → `clear EOP` → `lock`. Callers: `flash_copy_region` (below) and
    `FUN_08001658` (still pending). Mirrors `flash_erase_page`.
  - `flash_copy_region` (74 B @ `0x080016A6`) — page-by-page region
    copy. Reads each 1-KB page from `src` into a 1-KB SRAM scratch
    buffer at `0x200000F2` via `image_copy_halfwords`, then writes
    the scratch buffer to flash at `dst` via `flash_program_range`.
    **Size is implicit**: `n_pages = (dst - src) / 0x400`. The OEM's
    slot placement (slot 1 @ `0x08001800`, slot 2 @ `0x08004800`,
    12 KB apart) makes this `12 pages`. Sole caller is `main` at
    `0x08000296`, used to mirror slot 1 → slot 2 after slot 2 has
    been erased and slot 1's CRC has been validated.
  - `flash_erase_pages` (32 B @ `0x08000138`) — signed counted
    loop that calls `flash_erase_page(base_addr + i * 0x400)` for
    `i = 0..n_pages-1`. The OEM uses `blt` so `n_pages <= 0` is a
    no-op. Called 4× from `main` (`0x0800028E`, `0x080002C6`,
    `0x080002F2`, `0x080003F8`) — wipes different flash regions
    before / during OTA. Structurally identical to shifterware's
    `flash_erase_pages` at `0x08003832` (same signature, same
    page constant, same `unlock-every-iteration` inefficiency).
- `clock.c` — `set_sysclock_to_48m` (106 B @ `0x0800054C`). Called
  by the vendor-stock `SetSysClock` trampoline at `0x080005B6`,
  which is itself called at the tail of vendor-stock `SystemInit`
  (`0x080005BE`) during Reset_Handler. **NOT** byte-identical to
  MindMotion's published `SetSysClockTo48M` — the OEM skips the
  explicit PLLMUL / PLLSRC / PLLON dance and instead writes a
  cleared `RCC->CFGR` with PPRE = /2, then switches `SW` to `10`
  (PLL position) and spins on `SWS == 10`. The shifterware-side
  `rcc_get_clocks_freq` (in `shifterware/src/hal.c`) confirms
  this yields SYSCLK = 48 MHz when `RCC->CR` bit 20 is clear
  (which this function ensures). Flash latency set to 1 wait
  state with prefetch buffer enabled (`FLASH->ACR = 0x11`).
  Two MM32F031-specific `RCC->CR` bits (bit 20 and bit 2) are
  cleared during setup; their precise roles are TBD without the
  vendor RM, but bit 20 is the documented "48 MHz vs 72 MHz"
  selector per the shifterware decomp.

## Functions

### Vendor-stock (recognised, no decomp needed)

| Address | Size | Name | Source |
| --- | --- | --- | --- |
| `0x0800052e` | 18 | `NVIC_SystemReset` | CMSIS-Core M0 `core_cm0.h` standard inline |
| `0x080005b6` | 8  | `SetSysClock` | MindMotion `system_MM32F031x4x6_q.c` trampoline |
| `0x080005be` | 66 | `SystemInit` | MindMotion `system_MM32F031x4x6_q.c` (byte-identical) |
| `0x0800060c` | 46 | `Reset_Handler` | MindMotion `startup_MM32F031x4x6_q.s` (byte-identical) |
| `0x08001364` | 6  | `USART_SendData` | MindMotion HAL `hal_uart.c` |
| `0x08001372` | 20 | `USART_GetFlagStatus` | MindMotion HAL `hal_uart.c` |
| `0x08001388` | 20 | `USART_GetITStatus` | MindMotion HAL `hal_uart.c` |
| `0x080013ac` | 8  | `CRC_ResetDR` | MindMotion HAL `hal_crc.c` |

The 9 trap stubs at `0x08000632..0x08000644` are byte-identical copies
of `Default_Handler` (one per CMSIS-style exception slot, including
the F1-lineage MemManage/BusFault/UsageFault/DebugMon slots that don't
exist on CM0). They originate from `startup_MM32F031x4x6_q.s`'s
`EXPORT [WEAK]` aliases — treat all 9 as stock; the file contains a
single `Default_Handler` definition.

### VanMoof-custom (decomp targets)

| Address | Size | Name | Status | Notes |
| --- | --- | --- | --- | --- |
| `0x080000b4` | 20 | `_cold_reset` + `thunk_main` | decomp-asm | replaces Keil `__main`, packed into vector-table tail |
| `0x080001d8` | 802 | `main` | named | bootloader's actual logic |
| `0x080014ae` | 20 | `systick_tick` | decomp-c | decrements `g_systick_countdown` if non-zero |
| `0x080014c2` | 8  | `SysTick_Handler` | decomp-c | trampoline to `systick_tick` |
| `0x080014ca` | 20 | `mdelay` | decomp-c | busy-wait `ms` milliseconds — set `g_systick_countdown = ms`; spin until SysTick decrements to 0. Sole caller is `main` at `0x080002E2` with `ms = 250`. The emulator (`tools/emulate_mm32f031.py`) short-circuits this body since it doesn't simulate SysTick IRQs. |
| `0x08000158` | 100 | `image_verify_crc` | decomp-c | image header + payload CRC validator @ slot 1. Returns 0 ok / 1 CRC mismatch / 2 header invalid. Calls `CRC_ResetDR` (vendor-stock) + `memcpy` + `crc32_words`. Same shape as shifterware's `image_verify_crc` @ `0x08003AC6`. |
| `0x080013cc` | 32 | `crc32_words` | decomp-c | feed `count` u32 words from `src` into the MM32F031 hardware CRC engine at `0x40023000`, return accumulated CRC. |
| `0x08001740` | 36 | `memcpy` | decomp-c | word-fast + byte tail; void return (non-POSIX). Word path triggers on `(dst \| src) & 3 == 0`. |
| `0x080016f0` | 26 | `boot_app` | decomp-c | application-boot trampoline: stash Reset_Handler @ SRAM `0x20000018`, MSR MSP from vec[0], BLX to the stash. Sole caller is `main` at `0x0800031C` with `0x08004828` (slot 2 + 40). Closes the boot-handoff question. |
| `0x08001764` | 36 | `_init_data_bss` | named | called from `_cold_reset`; CMSIS-style .data copy + .bss zero — likely VanMoof-written rather than vendor (the MindMotion BSP relies on Keil `__main` instead). To confirm by inspection. |
| `0x0800054c` | 106 | `set_sysclock_to_48m` | decomp-c | VanMoof-tuned clock-tree bring-up: HSI on → clear two MM32-specific `RCC->CR` bits (20, 2) → `RCC->CFGR = 0x400` (PPRE=/2) → `FLASH->ACR = 0x11` (1 WS + prefetch) → switch `SW` to `10` (PLL) → wait `SWS == 10`. Skips the standard MindMotion PLLMUL/PLLSRC/PLLON sequence — relies on MM32F031's "SW=PLL with PLLMUL=0" routing 48 MHz directly. |
| `0x080000c8` | 28 | `uart1_send_byte` | decomp-c | UART1 single-byte send-and-wait: `USART_SendData(USART1, b)` then spin on `USART_GetFlagStatus(USART1, 1)` (SR bit 0 = TX-ready). VanMoof-custom wrapper; calls vendor-stock HAL primitives. |
| `0x080000e4` | 28 | `uart1_send_buf`  | decomp-c | UART1 buffer transmit loop on top of `uart1_send_byte`; `len` is `uint16_t` (compiler emits `uxth` on each iteration). |
| `0x080014f8` |  6 | `image_read_halfword` | decomp-c | `return *p` halfword load; non-inlined leaf used by `image_copy_halfwords` and once directly from `main`. |
| `0x08001554` | 34 | `image_copy_halfwords` | decomp-c | halfword memcpy; the OEM `uxth`-clamps the index → `count` is `uint16_t`. Also called by `FUN_080016A6` (pending). |
| `0x080001bc` | 28 | `image_read_u32_le` | decomp-c | Reads a 32-bit LE value from `src` as two halfwords (via `image_copy_halfwords`, count=2), then OR's them: `(staging[1] << 16) \| staging[0]`. Called 6× from `main` — one per u32 field of the image header. |
| `0x08000138` | 32 | `flash_erase_pages` | decomp-c | signed counted loop calling `flash_erase_page(base + i*0x400)` for `n_pages` iterations. Called 4× from `main` to wipe flash regions before OTA. Same shape and signature as shifterware's `flash_erase_pages` @ `0x08003832`. |
| `0x080001bc` | 28 | `FUN_080001bc` | pending | reads a 32-bit value (two halfwords, LE-assembled) from a stream — flash-image header reader |
| `0x08001534` | 32 | `flash_erase_page` | decomp-c | five-step page-erase wrapper: unlock → clear PGERR/WRPRTERR/EOP → `flash_do_page_erase` → clear EOP → lock. The "multi-byte UART sequence" speculation in the prior progress note was wrong — the `0x34` and `0x20` constants are `FLASH->SR` W1C masks, not UART bytes. |
| `0x08000746` | 72 | `flash_do_page_erase` | decomp-c | inner page-erase: wait READY → PER → AR=addr → STRT → wait → clear PER via `& 0x1FFD`. Returns `FLASH_ST_*` status. Byte-size-equivalent to OEM (72 B total). |
| `0x0800071a` | 44 | `flash_wait_status` | decomp-c | polls `flash_get_status` until !BUSY or timeout. Writes `FLASH_ST_TIMEOUT` unconditionally when `timeout == 0`. |
| `0x080006ca` | 54 | `flash_get_status` | decomp-c | priority cascade BSY > PGERR > WRPRTERR > READY against `FLASH->SR`. Isolates BSY via `lsls/lsrs #0x1F`. |
| `0x08000700` | 26 | `flash_busy_step` | decomp-c | `volatile int i = 0; i = 0xFF; while (i) i--;` — double-initial-store pattern in the disasm is the giveaway that `i` is volatile. |
| `0x08000946` | 64 | `flash_program_halfword` | decomp-c | inner halfword writer: `wait_status(15)` → set PG → write halfword → `wait_status(15)` → clear PG via `& 0x1FFE` (= `0x1FFD + 1`, shares the page-erase pool word). |
| `0x080014fe` | 54 | `flash_program_range` | decomp-c | wrapper: unlock → clear PGERR/WRPRTERR/EOP → loop `flash_program_halfword(dst + i*2, src[i])` → clear EOP → lock. Mirrors `flash_erase_page`'s structure. Callers: `flash_copy_region` and `FUN_08001658` (pending). |
| `0x080016a6` | 74 | `flash_copy_region` | decomp-c | page-by-page region copy through an SRAM scratch buffer at `0x200000F2`. Size implicit: `n_pages = (dst - src) / 0x400`. Sole caller is `main`'s slot1→slot2 image-sync path. |
| `0x08001658` | 78 | `ota_program_chunk` | decomp-c | per-chunk inbound-OTA flasher. Skips 11 bytes of Modbus frame header, packs LE halfwords from the payload into scratch at `0x200000F2`, flushes 16 halfwords (32 B) to flash at `dst` via `flash_program_range`. Sole caller is `main`'s cmd-0x82 streaming loop at `0x080004B6`. |
| `0x080006b0` | 12 | `flash_unlock` | decomp-c | canonical KEY1 (`0x45670123`) + KEY2 (`0xCDEF89AB`) to `FLASH->KEYR`. Used by `flash_erase_page` and the pending flash-programmer wrapper `FUN_080014FE`. |
| `0x080006bc` | 14 | `flash_lock` | decomp-c | `FLASH->CR |= 0x80` (LOCK bit). Same two callers as `flash_unlock`. |
| `0x08000bde` |  6 | `flash_clear_status` | decomp-c | `FLASH->SR = bits` (W1C the chosen flag bits). Called 2× from `flash_erase_page` and 2× from `FUN_080014FE`. |
| `0x08001554` | 34 | `FUN_08001554` | pending | halfword-stream copy: `memcpy_halfwords` via `FUN_080014f8` |
| ... | | | | (~57 more pending) |

Full list in `ghidra/exports/shifterboot_program.json`.

## Open questions

- Where does the bootloader jump to the application image? (vector
  fix-up address / direct branch / `vector_offset` register?)
- What integrity scheme guards the application image? CRC? Hash?
- Does it support OTA updates by itself, or is the update mechanism in
  the application?
- Initial SP `0x200006F8` — what state in SRAM[0x6F8..0x1000) does the
  loader expect to be preserved across the warm jump?
- Is `_init_data_bss` (`0x08001764`) hand-written by VanMoof or borrowed
  from a CMSIS reference startup? (The MindMotion BSP itself relies on
  Keil's `__main` for runtime init; the bespoke `_cold_reset` calls
  this function so it is at least *adapted* to VanMoof's setup.)
- `FUN_0800054c` (called from stock `SetSysClock`) — does it match
  MindMotion's `SetSysClockTo48M` / `SetSysClockTo72M`, or did VanMoof
  customise the clock-tree config?
