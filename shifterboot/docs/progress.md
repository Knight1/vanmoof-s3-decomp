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
| 26 | vendor-stock (recognised; no decomp needed) |
| 0  | in-progress |
| 33 | decomp (asm or c) |
| 2  | named (rename in Ghidra, no source yet) |

`function_count = 78` per `ghidra/exports/shifterboot_program.json`.
The pending count will tighten as more functions are classified as
`vendor-stock` vs `custom`.

## Per-module decomp log

- `startup_mm32f031.S` — vector table wired into the `.isr_vector`
  section (48 slots, mapping vec[0]=`_estack`, vec[1]=`Reset_Handler`,
  vec[15]=`SysTick_Handler`, vec[43]=`USART1_IRQHandler`, everything
  else weak-aliased to `Default_Handler`). `Reset_Handler` is a
  minimal stub (set MSP → `set_sysclock_to_48m` → `_cold_reset`) that
  skips the OEM's vendor `SystemInit` wrapper and goes straight to
  the clock-tree config we already have. `_init_data_bss` is
  re-implemented as a direct linker-symbol-driven .data copy + .bss
  zero (no ARMCC scatter-table indirection); same observable end
  state. Build is now link-clean — `text = 2340`, `bss = 520`.

- `hal_stubs.S` — temporary `.weak` fallbacks for the MindMotion HAL
  / CMSIS leaves the C sources declare `extern` (USART_*, RCC_*,
  GPIO_*, NVIC_*, CRC_ResetDR). Stubs default to safe values
  ("no event pending" / "always ready" / no-op) so the build links
  without the BSP. `NVIC_SystemReset` is implemented inline (SCB
  AIRCR write) since a no-op fallback would be observably wrong.
  Every stub is `__attribute__((weak))` — vendoring the real BSP
  removes the fallbacks transparently.

- `main.c` — `main` (852 B @ `0x080001D8`). The bootloader's central
  orchestrator. Two phases sharing one 120-byte stack frame:
  - **Phase 1 (cold-boot validate-and-jump)** — reads both slot magic
    fields; if slot 1 has a fresh image (valid magic, TYPE_ID = 0xC1,
    CRC differs from slot 2), runs `image_verify_crc` and on success
    erases slot 2 then `flash_copy_region(slot 1 → slot 2)`. If the
    new image is bad-typed, CRC-failed, or already mirrored, marks
    slot 1 for erasure. After the slot 1 housekeeping, `mdelay(250)`
    listens for a `0x1B` byte parked in the RX buffer's first slot
    (a "wipe slot 2" probe from mainware). Then decides:
    `slot2_valid && !slot1_valid` → `boot_app(slot2 + 0x28)`;
    `slot2_valid || slot1_valid` → `NVIC_SystemReset()` (re-evaluate
    after the slot-state change); else fall into phase 2.
  - **Phase 2 (Modbus RTU server loop)** — poll `MODBUS_RX_IDX`,
    validate slave addr (`0x20`) and frame length, dispatch by
    function-code + register-low byte (= sub-id at frame[3]):
    `0x01` ping reply (template B), `0x81` apply (run
    `image_verify_crc`, reply, latch a `NVIC_SystemReset` on loop
    end), `0x82` over `0x10` is OTA-stream (32 B chunk), `0x95`
    erase slot 1, default reply template A. On cmd-0x81 success,
    the latched reset re-enters phase 1 — which now finds both
    slots valid with matching CRCs, erases slot 1, and boots
    slot 2. So the complete OTA cycle is:
    `0x95 → 0x82 × N → 0x81 → reset → cold-boot → boot slot 2`.
- `startup_mm32f031.S` — `Default_Handler` (custom: 2-byte `b .` stub);
  `_cold_reset` + `thunk_main` (VanMoof's 20-byte `__main` replacement
  packed into the unused vector-table tail).
- `systick.c` — `systick_tick`, `SysTick_Handler`, `mdelay`, and
  `boot_init_systick`. The stock MindMotion template has `b .` for
  SysTick; VanMoof installed a countdown decrementer that drives a
  millisecond-delay variable at SRAM `0x20000010`.
  - `boot_init_systick` (60 B @ `0x08001472`) — `SysTick_Config(48000)`
    inlined (1 ms tick at HCLK = 48 MHz), then `NVIC_SetPriority(SysTick, 0)`
    to bump SysTick to highest priority. The reload-value pool word
    `0x0000BB80 = 48000` corroborates the 48 MHz HCLK that
    `set_sysclock_to_48m` configures and that mainware-side
    `rcc_get_clocks_freq` reports. Sole caller is `main` at
    `0x0800020C`.
  - `mdelay` (20 B @ `0x080014CA`) — single-arg millisecond delay.
    Stores `ms` into `g_systick_countdown` and spins until
    `SysTick_Handler` ticks it down to zero. Sole OEM caller is
    `main` at `0x080002E2` with `ms = 250` (a 250 ms pause inside
    the image-sync chain). The OEM compiles with a stack-spill
    pattern (`push {r0, lr}; ldr r0, [sp]; ... pop {r3, pc}`) that
    `-O0` emits to give every parameter a stack slot; semantics are
    register-equivalent and `-Os` skips the spill.
- `uart.c` — `uart1_send_byte` (28 B @ `0x080000C8`),
  `uart1_send_buf` (28 B @ `0x080000E4`), and `boot_init_usart1`
  (150 B @ `0x08001578`).
  - The two TX wrappers are VanMoof-custom around MindMotion HAL
    `USART_SendData` / `USART_GetFlagStatus`; byte-send spins on
    USART SR bit 0 (TX-ready/complete on MM32); buffer-send is a
    `uxth`-clamped 16-bit decrement loop.
  - `boot_init_usart1(uint32_t baud_rate)` is the symmetric init for
    everything USART1: clocks (USART1 + GPIOB), NVIC (IRQ 27 priority
    3), alt-function for PB6/PB7 → USART1_TX/RX, 8-N-1 + Rx|Tx,
    RXNE IRQ enable, USART enable, then `GPIO_Init` for PB6
    (`AF_PP`) and PB7 (`IN_FLOATING`). Sole caller is `main` at
    `0x08000214` with `baud_rate = 9600` (materialised as `75 << 7`)
    — the canonical low-speed Modbus RTU rate for the S3
    inter-module bus.
  Build-incomplete state: the HAL primitives are declared `extern`
  in `uart.c` and resolve once the MindMotion BSP is vendored in.
  Compile-clean (-Wall -Wextra -Wpedantic -Wshadow); link still
  produces undefined references for the HAL leaves and the missing
  `Reset_Handler` chain, as expected.
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
- `modbus.c` — Modbus RTU framing primitives for the OTA-server
  dispatcher in `main`, plus the USART1 RX bottom edge:
  - `USART1_IRQHandler` (58 B @ `0x0800160E`) — Cortex-M0 vector slot
    43. One-byte-per-RXNE accumulator: `USART_GetITStatus(USART1, RXNE)`
    → `USART_ClearITPendingBit(USART1, RXNE)` → `USART_ReceiveData(USART1)`
    → append to the 45-byte inbound buffer at SRAM `0x200000C4`,
    indexed by the halfword counter at SRAM `0x20000014`. Bytes past
    index 44 are silently dropped (no overflow handling — the
    dispatcher resets the index after consuming a frame). The 45 B
    ceiling matches the longest Modbus RTU PDU shifterboot consumes
    (cmd-0x82 OTA stream: 11 B header + 32 B image payload + 2 B
    CRC). Frame-completion (3.5-char idle gap) is not detected here
    — that lives in `main`'s dispatcher.
  - `modbus_crc16` (56 B @ `0x08000100`) — RTU CRC16 (poly `0xA001`,
    init `0xFFFF`). Pool words at `0x080004CC` (init) and
    `0x080004D0` (poly). Returns the accumulated CRC in r0 — no RAM
    side effect. The OEM uses `asrs` for the post-XOR shift, which
    is bit-identical to `lsrs` here because bit 31 of the working
    register never gets set (the value is logically 16-bit
    throughout). Four callers, all in `main`'s inbound-frame
    validators (`0x08000370`, `0x080003B8`, `0x08000440`,
    `0x080004FA`), all called with `(buf, 6)` — i.e. the function-
    code-plus-five-data-bytes core of a Modbus PDU, with the
    trailing two CRC bytes compared as `uxtb(r0)` against `buf[6]`
    and `asrs(r0, #8)` against `buf[7]`. Diverges in surface from
    shifterware's `modbus_crc16_compute` (which writes to globals
    at `0x200000E7`/`0xE8`); algorithm and pool constants are
    identical.
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
| `0x08001404` | 110 | `NVIC_SetPriority` | CMSIS-Core M0 `core_cm0.h` — out-of-lined by the MindMotion BSP build. Sole on-target caller is `boot_init_systick`. |
| `0x080005b6` | 8  | `SetSysClock` | MindMotion `system_MM32F031x4x6_q.c` trampoline |
| `0x080005be` | 66 | `SystemInit` | MindMotion `system_MM32F031x4x6_q.c` (byte-identical) |
| `0x0800060c` | 46 | `Reset_Handler` | MindMotion `startup_MM32F031x4x6_q.s` (byte-identical) |
| `0x08001364` | 6  | `USART_SendData` | MindMotion HAL `hal_uart.c` |
| `0x08001372` | 20 | `USART_GetFlagStatus` | MindMotion HAL `hal_uart.c` |
| `0x08001388` | 20 | `USART_GetITStatus` | MindMotion HAL `hal_uart.c` |
| `0x0800136a` | 8  | `USART_ReceiveData` | MindMotion HAL `hal_uart.c` — `return (uint8_t)(USARTx->RDR @ +4)`. The OEM uses `uxtb` (8-bit), not the F1-style `& 0x1FF` mask. |
| `0x0800139c` | 4  | `USART_ClearITPendingBit` | MindMotion HAL `hal_uart.c` — single `USARTx->[+0x14] = mask` store. |
| `0x080013ac` | 8  | `CRC_ResetDR` | MindMotion HAL `hal_crc.c` |
| `0x08001788` | 16 | `__scatterload_copy` | Keil ARMCC runtime — word-by-word `ldm/stm` copy. Called by `_init_data_bss` for the .data init table entry. |
| `0x08001798` | 16 | `__scatterload_zeroinit` | Keil ARMCC runtime — word-by-word `stm` zero. Called by `_init_data_bss` for the .bss init table entry. |
| `0x08000c62` | 222 | `GPIO_Init` | MindMotion HAL `hal_gpio.c` — full per-pin mode/speed/OType/PuPd config. Called twice by `boot_init_usart1` (PB6, PB7). |
| `0x08000db6` | 70 | `GPIO_PinAFConfig` | MindMotion HAL `hal_gpio.c` — writes the AFRL / AFRH halfword bitfield. Called twice by `boot_init_usart1` to point PB6/PB7 at AF0 (USART1 TX/RX). |
| `0x08000e08` | 106 | `NVIC_Init` | MindMotion HAL `hal_misc.c` — consumes a 3-byte `NVIC_InitTypeDef` (channel, priority, ENABLE). One on-target caller: `boot_init_usart1` (sets up IRQ 27 / USART1). |
| `0x0800113c` | 28 | `RCC_AHBPeriphClockCmd` | MindMotion HAL `hal_rcc.c` — `(mask, ENABLE/DISABLE)` toggle on `RCC->AHBENR` (offset +0x14). Used to gate GPIOB (`bit 18`) and the CRC peripheral (`bit 6`). |
| `0x08001158` | 28 | `RCC_APB2PeriphClockCmd` | MindMotion HAL `hal_rcc.c` — same shape on `RCC->APB2ENR` (offset +0x18). Used to gate USART1 (`bit 14`). |
| `0x08001174` | 28 | `RCC_APB1PeriphClockCmd` | MindMotion HAL `hal_rcc.c` — same shape on `RCC->APB1ENR` (offset +0x1C). Dead code in this binary. |
| `0x08001190` | 28 | `RCC_APB2PeriphResetCmd` | MindMotion HAL `hal_rcc.c` — same shape on `RCC->APB2RSTR` (offset +0x0C). |
| `0x080011ac` | 28 | `RCC_APB1PeriphResetCmd` | MindMotion HAL `hal_rcc.c` — same shape on `RCC->APB1RSTR` (offset +0x10). |
| `0x08001296` | 116 | `USART_Init` | MindMotion HAL `hal_uart.c` — baud-rate divisor + CR1/CR2/CR3 from `USART_InitTypeDef`. One caller: `boot_init_usart1`. |
| `0x08001324` | 24 | `USART_Cmd` | MindMotion HAL `hal_uart.c` — `(USARTx, ENABLE/DISABLE)` toggle of CR1.UE. |
| `0x0800133c` | 20 | `USART_ITConfig` | MindMotion HAL `hal_uart.c` — `(USARTx, IT, ENABLE/DISABLE)` toggle of the corresponding interrupt-enable bit. |
| `0x0800078e` | 70 | `FLASH_EraseAllPages` | MindMotion HAL `hal_flash.c` — dead code (no callers / no data refs); mass-erase via MER+STRT then clear MER with mask `0x1FFB` (= `0x1FFD - 2`) |
| `0x080007d4` | 150 | `FLASH_ReadOutProtection` | MindMotion HAL `hal_flash.c` — dead code (no callers / no data refs); DISABLE-only path: erase OB page via OPTER+STRT, set OPTPG, write `0xA5` (RDP unprotect key) as halfword to `OB->RDP @ 0x1FFFF800`. Uses a unique size optimisation absent from the other HAL helpers: `asrs r0, r0, #17` materialises the `0xFFF` wait limit out of the already-loaded `0x1FFFF800` OB base. |
| `0x0800086a` | 120 | `FLASH_EraseOptionBytes` | MindMotion HAL `hal_flash.c` — dead code (no callers / no data refs); MM32-variant taking `Page_Address` (FLASH->AR), OPTKEYR-unlock, OPTER+STRT, clear OPTER/OPTPG via masks `0x1FDF`/`0x1FEF` (= `0x1FFD - 30 / -14`) |
| `0x080008e2` | 100 | `FLASH_ProgramWord` | MindMotion HAL `hal_flash.c` — dead code (no callers / no data refs); 32-bit-via-two-halfwords programmer with PG bit + 15-cycle waits, clears PG with mask `0x1FFE` (= `0x1FFD + 1`) |

Together with the live MindMotion-HAL flash primitives translated under
`src/flash.c` (FLASH_Unlock / FLASH_Lock / FLASH_GetStatus /
FLASH_WaitForLastOperation / FLASH_ErasePage / FLASH_ProgramHalfWord /
FLASH_ClearFlag), the shifterboot binary ships the **complete**
MindMotion `hal_flash.c` translation unit — gc-sections kept the dead
helpers because they share the unit with live callees.

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
| `0x080001d8` | 852 | `main` | decomp-c | Two-phase orchestrator. **Phase 1** (cold boot): inspect both image slots, copy slot 1 → slot 2 if a fresh image staged + verified, listen 250 ms for a `0x1B` byte (wipe-slot-2 probe), then either boot slot 2, reset to re-evaluate, or fall through. **Phase 2** (Modbus RTU server, slave addr `0x20`): dispatch on func code + register-low byte. Sub-ids: `0x01` ping (template B), `0x81` apply (verify → reply → latched reset), `0x82` over func `0x10` is OTA stream (32 B chunks), `0x95` erase slot 1, default sends template A. The two response templates (`0x20 03 02 00 01 C5 83 00` and `0x20 03 02 02 00 05 23 00`) live in the locals area at sp+104 / sp+112, copied in from pool words. Ghidra's "size = 802" is wrong: an unconditional branch from `0x08000332` forward to `0x0800052C` makes the analyser stop accounting too early — actual extent is 852 B. |
| `0x080014ae` | 20 | `systick_tick` | decomp-c | decrements `g_systick_countdown` if non-zero |
| `0x080014c2` | 8  | `SysTick_Handler` | decomp-c | trampoline to `systick_tick` |
| `0x080014ca` | 20 | `mdelay` | decomp-c | busy-wait `ms` milliseconds — set `g_systick_countdown = ms`; spin until SysTick decrements to 0. Sole caller is `main` at `0x080002E2` with `ms = 250`. The emulator (`tools/emulate_mm32f031.py`) short-circuits this body since it doesn't simulate SysTick IRQs. |
| `0x08000158` | 100 | `image_verify_crc` | decomp-c | image header + payload CRC validator @ slot 1. Returns 0 ok / 1 CRC mismatch / 2 header invalid. Calls `CRC_ResetDR` (vendor-stock) + `memcpy` + `crc32_words`. Same shape as shifterware's `image_verify_crc` @ `0x08003AC6`. |
| `0x080013cc` | 32 | `crc32_words` | decomp-c | feed `count` u32 words from `src` into the MM32F031 hardware CRC engine at `0x40023000`, return accumulated CRC. |
| `0x08001740` | 36 | `memcpy` | decomp-c | word-fast + byte tail; void return (non-POSIX). Word path triggers on `(dst \| src) & 3 == 0`. |
| `0x080016f0` | 26 | `boot_app` | decomp-c | application-boot trampoline: stash Reset_Handler @ SRAM `0x20000018`, MSR MSP from vec[0], BLX to the stash. Sole caller is `main` at `0x0800031C` with `0x08004828` (slot 2 + 40). Closes the boot-handoff question. |
| `0x08001764` | 36 | `_init_data_bss` | decomp-asm | walks a 2-entry init table at flash `0x080017A8..0x080017C7`: each entry is `{src, dst, len, fn}`, where `fn` is `__scatterload_copy` (for .data: 28 B from `0x080017C8` → `0x20000000`) or `__scatterload_zeroinit` (for .bss: 1756 B at `0x2000001C..0x200006F7`). The table format is canonical Keil ARMCC `__rt_lib_init` scatterload. Our `startup_mm32f031.S` re-implements this directly against `_sdata/_edata/_sbss/_ebss/_sidata` linker symbols (skipping the table indirection) — behaviour identical, byte layout differs. |
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
| `0x08000100` | 56 | `modbus_crc16` | decomp-c | Modbus RTU CRC16 (poly `0xA001`, init `0xFFFF`). Per-byte XOR + 8-bit shift-and-XOR. Returns `uint16_t` directly (unlike shifterware's RAM-storing `modbus_crc16_compute`). Four callers, all inbound-frame validators in `main` (`0x08000370`, `0x080003b8`, `0x08000440`, `0x080004fa`), called with `(buf, 6)`. |
| `0x0800160e` | 58 | `USART1_IRQHandler` | decomp-c | Modbus RX bottom edge — vector slot 43. Reads one byte per RXNE via `USART_ReceiveData`, appends to the 45-byte inbound buffer at SRAM `0x200000C4`, indexed by halfword counter at SRAM `0x20000014`. Overflow bytes silently dropped. No frame-completion logic — the 3.5-char idle gap detection lives in `main`'s dispatcher. |
| `0x08001472` | 60 | `boot_init_systick` | decomp-c | CMSIS `SysTick_Config(48000)` inlined → 1 ms tick at HCLK = 48 MHz (reload = 47999, CTRL = `CLKSOURCE \| TICKINT \| ENABLE`). Followed by a VanMoof override that raises the SysTick exception priority from CM0-lowest (3) to highest (0) — so `mdelay` cadence isn't perturbed by other IRQs (in this binary the only other handled vector is USART1). Sole caller is `main` at `0x0800020C`. Materialises the OEM's "ticks too large" failure path as a `nop; b .` trap that is dead at runtime (48000-1 ≤ 0xFFFFFF). |
| `0x08001578` | 150 | `boot_init_usart1` | decomp-c | USART1 + GPIO bring-up. Enables USART1 and GPIOB clocks; NVIC IRQ 27 at priority 3; `GPIO_PinAFConfig(GPIOB, 6, AF0)` + `GPIO_PinAFConfig(GPIOB, 7, AF0)` to wire PB6/PB7 to USART1; `USART_Init(USART1, {baud_rate, 8b, 1-stop, no-parity, Rx\|Tx})`; enable RXNE IRQ; `USART_Cmd(USART1, ENABLE)`; finally `GPIO_Init` for PB6 (AF push-pull) and PB7 (floating input). Sole caller `main` at `0x08000214`, with `baud_rate = 9600` (materialised as `75 << 7`). |
| `0x08001554` | 34 | `FUN_08001554` | pending | halfword-stream copy: `memcpy_halfwords` via `FUN_080014f8` |
| ... | | | | (~57 more pending) |

Full list in `ghidra/exports/shifterboot_program.json`.

## Open questions

Resolved (kept for the audit trail):

- ~~**Where does the bootloader jump to the application?**~~ Resolved
  2026-05-20 by `boot_app` (`0x080016F0`) and the cold-boot fate logic
  in `main`: `boot_app(slot2_base + 0x28)` is called when slot 2 is
  valid and slot 1 is not. The trampoline stashes Reset_Handler at
  SRAM `0x20000018`, swaps MSP to `vec[0]`, and BLX'es through the
  stash.
- ~~**What integrity scheme guards the application image?**~~ Resolved
  by `image_verify_crc` (`0x08000158`): MPEG-2 CRC32 (poly `0x4C11DB7`,
  init `0xFFFFFFFF`, no reflection) over the masked header + payload.
  Driven through the MM32F031 hardware CRC peripheral at `0x40023000`.
- ~~**Does it support OTA updates by itself?**~~ Resolved by `main`'s
  phase 2 Modbus dispatcher: full OTA support — `0x95` erases slot 1,
  `0x82` over `0x10` streams 32 B chunks, `0x81` applies (verify +
  latched reset). The mainware sends the chunks; shifterboot writes
  flash + replies on the same bus.
- ~~**`FUN_0800054C` clock-tree match?**~~ Resolved 2026-05-20 as
  `set_sysclock_to_48m` (decomp-c). Corroborated independently by
  SysTick's reload value `48000-1` in `boot_init_systick`.

Still open:

- Initial SP `0x200006F8` — what state in SRAM[0x6F8..0x1000) does the
  loader expect to be preserved across the warm jump?
- Is `_init_data_bss` (`0x08001764`) hand-written by VanMoof or borrowed
  from a CMSIS reference startup? (The MindMotion BSP itself relies on
  Keil's `__main` for runtime init; the bespoke `_cold_reset` calls
  this function so it is at least *adapted* to VanMoof's setup.)
- The cmd-0x81 reply byte at offset 3 is computed as
  `((BE16(frame[4..5]) & 0x7F) << 1)`. Looks like a derived "echo"
  field (request register-address truncated to 7 bits then doubled),
  but the semantic — what the host does with it — is unclear without
  decoding the mainware-side Modbus dispatcher.
