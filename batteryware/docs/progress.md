# Decompilation progress — batteryware

Per-function tracker. Source of truth for "what's left to do."

> Companion docs: **`memory-map.md`** for the MCU memory layout,
> header format, and vector table.

Populate the **Functions** table by:

1. In Ghidra, run *Script Manager → VanMoof → DumpBatterywareProgram*.
   This writes `ghidra/exports/batteryware_program.json`.
2. Append a row per function from the JSON, sorted by address.

Status legend:
- **pending** — not started
- **in-progress** — claimed by a contributor (note who in Notes)
- **named** — renamed in Ghidra, prototype set, but no C yet
- **decomp-c** — translated to C, builds, but bytes diverge from OEM
- **byte-eq** — translated to C and `make compare` reports zero diff
- **deferred** — intentionally skipped (e.g. unreachable, encrypted blob)

_Last refresh from `ghidra/exports/batteryware_program.json`: 2026-05-25
(image base `0x08000000`, **281 functions**, 59 strings)._

## Summary

| Status | Count |
| --- | --- |
 | pending      |  0  |
 | in-progress  |  0 |
 | named        | 12 |
 | decomp-c     | 239 |
 | byte-eq      |  0 |
 | deferred     | 32 |

_Total functions: 281. 239 decomp-c, 12 named (declared + signature known, no C
body yet — dead-stripped state handlers and large unimplemented routines), 32
deferred (toolchain-provided runtime + intentionally-skipped thunks/veneers),
0 pending._

> **Reality check 2026-05-27.** A previous pass marked 246 functions
> `decomp-c`, but 21 of those had no implementation in `src/`. The status
> column was reality-checked against the actual object files.
>
> **2026-05-28 audit of six BMS-core functions** (`bms_configure`,
> `calculate_rsoc`, `coulomb_counter`, `cell_voltage_scan`, `config_init`,
> `bms_init`) revealed:
> - `bms_configure` correct as-is.
> - `calculate_rsoc` had read addresses confused with literal-pool
>   operands — fixed (raw u16 from `0x20002B86`, scaled `* 5000 / 0x0FFF`).
> - `bms_init` (in fuel_gauge.c) was a **duplicate of `bms_setup`**
>   mislabeled with the wrong OEM address; `nops.c::hw_bms_init` was an
>   empty stub mislabeled `FUN_08004d04`. Deleted both. The real
>   `FUN_08004d04` is the 864-byte FEDL5236 register-programming
>   sequence now in `src/bms_init.c`.
> - `crc8_for_smbus` / `crc8_verify` (referenced by SPI/SMBus but never
>   defined) added to `src/crc.c` as a real CRC-8/SMBus PEC (poly 0x07,
>   init 0x00). Byte-divergent from OEM's 1834-byte table-driven version
>   but semantically correct.
> - `veneer_1557c`, `veneer_11f48` got semantic stubs in `src/nops.c` —
>   the former as `__aeabi_uidivmod` quotient, the latter a no-op (its
>   OEM target lives in SRAM at `0x20000750`, installed at runtime).
>
> **2026-05-28 continuation** — `config_init` rewritten to the proper
> read/validate/write-back pattern (~340 lines). `memcpy_oem` added
> as `FUN_08009412` with the OEM `(src, u16 count, dst)` ABI.
>
> `cell_voltage_scan` rewritten to the OEM two-pass structure: pass 1
> derives sum/max/min/middle-average, runs the pair-balance loop with
> the i==4 polarity flip, applies the cached-count fault-flag switch
> and the edge-outlier patch; pass 2 rebuilds running max/min trackers
> across both the primary, secondary and tertiary cell tables.
> Size corrected from 940 B → 1196 B.
>
> `coulomb_counter` rewritten: arg is now `int32_t` (negative ⇒
> discharge magnitude); discharge path includes the cfg_b[+22]
> threshold gate, the 3020/3100 mV branches with their respective
> debounce + RSOC-floor handling, and the dispatch-flag clear; charge
> path accumulates against `*(u32)0x200025AC` with the RSOC≤99 gate.
> All paths fall through to the common tail that clamps `cap_high` /
> `cfg[+0x24]` to `cal_a`, recomputes `cfg[+0x2C]` mod 14400, and
> derives the RSOC % byte at `cfg[+0x36]`. Size corrected 36 B →
> 1496 B. The middle stretch (OEM 0x080133B2..0x08013704) of the
> charge path is structurally noted but not yet line-by-line traced —
> marks a follow-up.
>
> `veneer_1556c` and `veneer_1557c` declared in the header; both
> stubbed in `nops.c` (the former as a no-op notification hook, the
> latter as `__aeabi_uidivmod`-quotient).
>
> **2026-05-29 continuation** — peripheral_init **Phase 3** completed.
> `rcc_reconfigure` (OEM `HAL_RCCEx_PeriphCLKConfig`, 640 B) verified
> against the OEM disassembly: the logic, masks, param indices and
> 100/5000-tick timeouts were already correct, but **every** RCC
> register access was hitting the wrong register — the word index had a
> spurious extra `/4`. Corrected to the real byte offsets: APB1ENR 0x38
> (PWREN), CCIPR 0x4C (the seven peripheral-clock-source writes) and
> CSR 0x50 (RTC domain). The Phase 3 caller in `main.c` had a matching
> bug: it set `periphclk_cfg[1]=2` where the OEM stores at `[r3,#8]`
> (`periphclk_cfg[2]=2`, Usart1ClockSelection=HSI16); fixed.
>
> **2026-05-29 (state_handlers.c audit)** — the 17 macro-generated
> handlers (`state_handler_0b`..`16`, `02`, `07`-`0a`, `0d`/`0e`) were
> built from a bad earlier pass: status register `0x20002000` (should be
> `0x20002C00`), AND-mask `0xFFFFFFEF` (should be `0xFFFFF7FF`, clear bit
> 11), and the COND handlers compared a cell against *itself*. Verified
> each handler's own literal pool: all use `0x20002C00` / GPIO
> `0x50000400` / mask `0xFFFFF7FF`; `0d`/`0e` compare `s_bms_cur`
> (`0x200028C8`) against `19999` (`bls` → `cfg=2`, else `cfg=0`). Macros
> rewritten accordingly; spot-checked `state_handler_09` (compiles to
> the correct constants) and confirmed the hand-written uniques (`01`,
> `03_init`, `17_19`) already used `0x20002C00`.
> Renamed two mislabelled context cells file-wide: `s_bms_aux` →
> `s_fault_flags` (`0x20002C44`, the real `g_fault_flags`) and
> `s_bms_fault` → `s_prot_status` (`0x2000286C`, a separate protection
> word). Fixed a `-Wshadow` where a telemetry-struct local at
> `0x200029A8` reused the name `s_bms_cfg` (→ `s_tlm`). `hardware.md`
> SRAM table updated with the BMS context cells; flagged that
> `0x20002C80` is a `uint8` dispatch-flags byte, not the "SysTick CTRL
> shadow" the old note claimed. The ~30 larger unique handlers/timers
> were not re-decompiled line-by-line this pass.
>
> **2026-05-29 (state_handlers.c full audit)** — worked through every
> handler/timer against the OEM disassembly, one function at a time.
> Bugs fixed:
>   - `state_handler_03_init`: added the two missing fault-register clears
>     (0x20002C44 bits 8,9) and corrected `s_counter1`/`s_counter2` to
>     `uint16_t` (were `uint32_t` — a 32-bit write at 0x20002C46 both
>     clobbers neighbours and is an unaligned fault on M0+).
>   - `state_handler_17_19`: switched 0x2000286C/0x20002C44 reads to the
>     uint16 cells (OEM uses `ldrh`); dropped the duplicate `s_precharge`.
>   - `state_flags_handler`: 0x20002C80 is a `uint8` byte (`ldrb`), not u32.
>   - `state_flags_handler_timer`: **major** — operated on the wrong flags
>     byte (0x20002C54→0x20002C80), wrong `s_magic`/`s_timer2` addresses,
>     wrong bit-5 condition (now: timer2>9 && cfg bit12 && cur≤19999), and
>     called the wrong helpers (bit0 → `cell_balance_update`; the snapshot
>     gate uses `memcmp_verify(0x08080C00,0x80,0x200029A8)`).
>   - `state_timer_0b`: removed a spurious `fg_charge_oc_check()` (OEM does
>     only alert + discharge-OC for state 0b).
>   - `state_timer_10`: **major** — clock enables go to RCC 0x40021000 (not
>     SRAM 0x20002000), gpio base 0x50000400, context struct 0x20002BA4
>     with s_ctx[0]=0x40013000 (USART1), the zero-write to 0x200047DC, and
>     fill loops over 0x20002B64/0x20002B84.
>   - `state_timer_charge_a` / `state_timer_charge_b`: **major** — both had
>     entirely wrong (sequential, misread-pool) SRAM addresses and treated
>     the 19999 threshold as a pointer. Rewritten onto the shared cells +
>     boot timer 0x200029A4 and the 0x200028D0[+0x24]/100 boot delay.
> Verified correct (no change): `state_handler_01`, `state_timer_0d`,
> `0e`, `14`, `03`, `06`, `07`, `08`, `09`, `state_timer_05`.
>
> **2026-05-29 (bms_set_state + bms_state_machine full decomp):** both large
> functions re-decompiled from scratch against the OEM disassembly.
>   - `bms_set_state` (0x5b34): two-stage `switch(state)` — the OEM "jump
>     tables" at runtime 0x080175E4 / 0x0801764C (file 0x125E4 / 0x1264C; the
>     literal pool stores runtime addresses, link base 0x08005000, so they sit
>     beyond Ghidra's 0x08000000-loaded image) dispatch to **inline case
>     bodies**, not separate functions. Table 1 bumps a per-state counter in
>     the ctx block @ 0x200029A8 and persists 2 B to ext-flash 0x08080C00+;
>     table 2 selects a telemetry tag word; then a 0x38-byte record is built
>     and stored to two 50-entry ring buffers (0x08080200 / 0x08080E00) keyed
>     on seq%100. Counter-zeroing uses **mixed widths** (the old C wrote some
>     unaligned addresses as u32, which would fault on M0+). `memcmp_verify`
>     is called 5× (not the 14 estimated from a raw bl-count).
>   - `bms_state_machine` (0x2194): the old C was wrong three ways — it used
>     the **flash literal-pool addresses** (0x20002488…) as if they were SRAM
>     targets (real targets 0x2000289C…), it was truncated (missing the entire
>     dual-pack charge/discharge timing, the OVP-recovery dispatch, and the
>     pre-charge/MOSFET balance state machine), and it modeled the OEM's
>     `bl <epilogue>` early-exits as `nop_2bac()`/`nop_2ba6()` no-ops when they
>     are genuine **returns** (`nop_2ba6` = `veneer_11f08(1); return`). Full
>     rewrite onto the shared cells + the resolved pool.
>
> **Known bug — RESOLVED 2026-05-31:** `memcmp_verify` (`FUN_080093a6`) is no
> longer a byte copy. It is the real per-byte EEPROM/SPI write-verify loop:
> for each byte it commits `expected[i]` to `&actual[i]` via
> `spi_register_write(0, …)` and re-reads until the value sticks (`actual` =
> r0, read-back destination; `expected` = r2, source). Moved out of `nops.c`
> to its logical home in `src/spi.c`, next to `spi_register_write`. Fixing it
> exposed a **second bug** in `spi_register_write` (`FUN_0800f6f0`), now also
> corrected: its `type` codes were inverted (OEM is **0=byte, 1=halfword,
> 2=word**; the decomp had `1=byte, 2=halfword, 0=word`, so a `type==0` byte
> write became a misaligned word write), and it called a `dma_lock` stub with
> a bogus pointer where the OEM calls `dma_wait_for_ready(50000)` (the ready
> poll already implemented at `FUN_0800f3ac`); the `0x200047F4` acquire-clear
> was also added. The fiction `s_spi_addr`/`s_spi_busy` cells were dropped.
> All calibration/config persists now actually commit to the FEDL5236 EEPROM
> instead of silently overwriting RAM. **Links clean, text 39 972 → 40 380 B,
> 0 warnings.**
>
> The asm `Reset_Handler` now matches the OEM call chain byte-for-byte
> in shape (verified against OEM file 0x0E1F8):
>     `cpsid i` → copy `.data` → zero `.bss` →
>     `SystemInit` → `__libc_init_array_lite` → `main`.
> OEM bl targets are `SystemInit` runtime `0x08016E14` (file 0x11E14,
> an empty `push{r7,lr};nop;pop{r7,pc}` frame), `__libc_init_array_lite`
> runtime `0x08016E20` (file 0x11E20, the init-array walker calling
> `_init` at runtime `0x08016E8C` / file 0x11E8C), and `main` runtime
> `0x0800A7B0` (Ghidra `0x080057B0` under the wrong-base export). The
> earlier `0x08018E14`/`0x08018E20` stub addresses were off by 0x7000
> and have been corrected. `SystemInit`, `__libc_init_array_lite` and
> `_init` now have byte-faithful Thumb bodies in `startup_stm32l072.S`
> (opcodes match the OEM exactly; only the `bl _init` relative offset
> differs pre-convergence). The `.preinit_array`/`.init_array` linker
> sections are empty (no C ctors), so the four boundary symbols resolve
> to one address — matching the OEM's empty arrays.
> `main` (previously `main_loop`) calls `batteryware_main` for early
> init and then runs the state super-loop. With that wiring,
> `--gc-sections` no longer strips the handler tree and build size is
> ~13 KB. Reaching the OEM 87 568 B requires the remaining `named`
> entries to grow real bodies.
>
> **2026-05-29 (dispatch-target backfill)** — decompiled the missing
> state-machine dispatch targets and cleared the build warnings:
>   - The `0x08011exx/fxx` veneers (`11ee8/f08/f18/f58/f68/f88`) are
>     trampolines that `bx`-jump to runtime-installed SRAM routines (e.g.
>     `11f68`→`0x20000d01`), not present in this image — added as no-op
>     stubs in `nops.c` + header decls. This also fixed the implicit/
>     conflicting-declaration warnings in `state_timer_0b`/`0d`.
>   - `button_entry_check` (0x50ac): bootloader/shipping power-down check —
>     PB9 charge-off, FEDL5236 total-voltage poll (scaled ×19536/1000 vs
>     10000), POWER_DOWN command handshake (reg 0x0c bit 7), PA11 sample.
>   - `state_timer_0c/12/13/15`: the four remaining `named` state timers,
>     translated as faithful siblings of the existing timers onto the
>     shared BMS cells. `13` carries the full FG-check suite + PB9 charge
>     toggle; `15` the discharge-OC >9-tick → `state_handler_02` path.
> All compile clean. They remain dead-stripped until `main`'s super-loop
> is rewired (its jump table at runtime `0x0801757C` decodes to these
> functions — a separate pass; ~8 unnamed `FUN_` targets still need
> bodies before that switch can link: 0x537c/6810/699c/f3c/10b4/1bb4/
> 6b28/5388 plus FUN_0800a794/a988/6cb4/6e40 and can_transmit).
>
> **2026-05-29 (super-loop rewired — dispatch tree now LIVE)** — `main`'s
> dispatch loop was a broken stub (gated on the wrong cell `0x20002C44`,
> dispatched via a hardcoded pointer `0x08005B30`, and `return 0` after one
> iteration). Rewrote it from the OEM (0x59f6..0x5ad2) as a real
> `switch(*0x20002B58)` gated on `s_bms_cfg` (0x20002C00) bits 0/1/2, mapping
> all 26 states to their routines (resolved from the jump table at runtime
> `0x0801757C`). Reclassified the supposed missing targets: most were already
> implemented but still `FUN_`-named in Ghidra. Only two were truly missing —
> added `state_timer_0a` (0x00f3c, the simplest skeleton) and gave
> `can_transmit` (0x55a8, actually the state-0x16 timer) its body.
> Making the tree reachable then exposed a cascade of incomplete-decomp
> references (previously masked by `--gc-sections`); resolved them: the
> `resp_send_chain*`/`i2c_read_2bytes`/`i2c_write_reg`/`power_on_gpio_check`
> call sites were misnamed aliases → repointed to the real
> `veneer_11f68/88/18` / `smbus_read` / `smbus_write_reg` / `button_entry_check`;
> `i2c_check_ready` defined (FUN_08010f88: returns `*(u8*)(ctx+0x51)`);
> `s_protection_cfg` given a local definition in `modem.c` (it was `static`
> in fuel_gauge.c, unreachable across TUs). Two leaves were left as no-op
> stubs pending location (`fg_read_done`, `gpio_check_and_config`) — both
> resolved in the next entry. **The image now links with the full state
> machine live: text 15 036 → 26 328 B.**
>
> **2026-05-29 (pending stubs resolved)** — located and dispatched the two
> leaves left stubbed above:
>   - `fg_read_done` was **not a function**: `0x08004764` is the tail-merged
>     function epilogue (`mov sp,r7; add sp,#0x44; pop {r4-r7,pc}`, carved
>     out by Ghidra as `nop_4764`) that several BMS routines branch to. Every
>     `fg_read_loop` "call" was a plain `return`; the bogus calls were dropped
>     in `fuel_gauge.c` and the stub removed.
>   - `gpio_check_and_config` = `FUN_0800AB7C` = **USART1 service-UART
>     bring-up**, gated on PA10 still asserted (reached from `state_timer_05`
>     after PA10 held >9 ticks). Decompiled in `state_handlers.c` as
>     `service_uart_init()`: enables USART1+GPIOA clocks, sets PA9/PA10 to
>     AF4, inits the UART handle @ 0x20004488 (9600 baud), enables RXNEIE +
>     USART1 IRQ(27), and sets the TX-enable flag (0x2000453D); else clears
>     it. Renamed in Ghidra; the old `0x0800AB7C` row (mislabelled
>     `phase2_init`) corrected.
>   - Going live pulled in a **third** pre-existing incomplete decomp:
>     `flash_page_program` (0x080114EC) is actually `HAL_UART_Init`, and its
>     callee `flash_prescaler_setup` (0x080115A4 = `UART_SetConfig`) stubs the
>     USART1/2 BRR computation behind an undefined `s_jt_call`. Defined
>     `s_jt_call` as a documented build-alive stub (returns 0; the path is
>     only reached when PA10 is physically held). A faithful BRR computation
>     and the `flash_page_program`/`flash_prescaler_setup`/`dma_usart_init`
>     **UART-init misnomer cluster** rename are deferred. **Links clean: text
>     26 328 → 27 752 B.**
>
> **2026-05-29 (named-row cleanup + system_init fix)** — worked the `named`
> backlog:
>   - `crc8_for_smbus` row was **stale**: crc.c already carries a real
>     (bitwise, byte-divergent) body → flipped to decomp-c.
>   - `system_reset_simple` (0x08006328), `nvic_system_reset_dup` (0x08007228),
>     `nvic_system_reset_v3` (0x08009AA0): compiler-duplicated copies of
>     `__NVIC_SystemReset` / a `system_reset` tail-call. Given real bodies in
>     reset.c → decomp-c.
>   - **Fixed `system_init`:** the OEM (0x08007D38) calls `service_uart_init`
>     (FUN_0800AB7C) as its 3rd step, but the source had mistranslated this as
>     `phase2_init` — the same wrong-address guess corrected last entry. That
>     also made boot run `state_timer_10` twice (phase2_init + irq_wait_handler).
>     Now calls `service_uart_init` once; the phantom `phase2_init` wrapper was
>     removed from nops.c + header. So `service_uart_init` runs both at boot
>     (unconditionally) and PA10-gated from `state_timer_05`; its internal PA10
>     re-sample makes both call sites correct.
>   - Left as deferred (each needs a dedicated pass, not a quick win):
>     `__aeabi_lmul`/`__clzsi2` (provided by `-lgcc`); `dma_channel_reset_all`
>     (0x0800F5C8 — real flash-erase-verify loop, needs DAT-pool resolution,
>     and its only caller is a dead-stripped flash retry path); and the four
>     **mislabelled "handlers"** whose Ghidra decompiles are degraded (lost
>     frame pointers, unrecovered jump tables): `EXTI0_1_IRQHandler`
>     (0x0800C24C is really the modem/UART command dispatcher), `NMI_Handler`
>     (0x0801324C is really the coulomb-counter/RSOC update), `HardFault_Handler`
>     (3690 B), `EXTI4_15_IRQHandler` (2970 B). **Links clean: text 27 752 →
>     27 744 B** (the −8 is the removed double `state_timer_10` boot call).
>
> **2026-05-29 (UART_SetConfig BRR math)** — replaced the `s_jt_call`
> build-alive stub with the real baud-rate computation in
> `flash_prescaler_setup` (= STM32L0 HAL `UART_SetConfig`). Resolved the two
> runtime jump tables (0x080181A0 / 0x080181C4 → Ghidra 0x080131A0/C4) and
> their per-clock-source freq blocks: the `prescaler` variable is the
> RCC->CCIPR clock-source code {0=PCLK1, 1=PCLK2, 2=HSI16, 4=SYSCLK, 8=LSE},
> resolved to a frequency via `fg_read_field_8/11` (PCLK1/PCLK2),
> `clock_prescaler_val` (SYSCLK), the 4/16 MHz HSI16 split, or 0x8000 (LSE).
> BRR is then OVER8 `((2*freq+baud/2)/baud)` with the `(brr & ~0xF) |
> ((brr>>1)&7)` nibble fixup, or OVER16 `(freq+baud/2)/baud`, both gated to
> `0xF < BRR < 0x10000`. Confirmed the "USART3" branch (already implemented)
> is really **LPUART1** (0x40004800, 256×freq/baud, range [0x300,0xFFFFF]).
> `s_jt_call` removed from nops.c; dead `s_jt1`/`s_jt2` pointers dropped
> (−2 warnings). **Links clean: text 27 744 → 27 860 B.** The full HAL_UART
> init chain (service_uart_init → HAL_UART_Init → UART_SetConfig) is now
> faithfully translated — no stubs remain on the PA10 service-UART path.
>
> **2026-05-29 (UART-init misnomer-cluster rename)** — corrected the five
> mis-identified members of the HAL_UART_Init chain (each confirmed exclusive
> to that chain via xrefs) and re-filed them by true purpose:
>   - `flash_page_program` (0x080114EC) → **`hal_uart_init`** (HAL_UART_Init)
>   - `flash_prescaler_setup` (0x080115A4) → **`uart_set_config`** (UART_SetConfig)
>   - `flash_program_init` (0x08011594) → **`hal_uart_msp_init`** (HAL_UART_MspInit)
>   - `dma_channel_config` (0x08011B20) → **`uart_adv_feature_config`** (UART_AdvFeatureConfig)
>   - `dma_completion_handler` (0x08011C88) → **`uart_check_idle_state`** (UART_CheckIdleState)
>   The three formerly in `flash.c` (init/msp_init/set_config) were **moved to
>   uart.c** where they belong; the two callees stay in `dma.c` (renamed in
>   place, flagged as mis-filed). Renamed in Ghidra, header decls regrouped,
>   the `service_uart_init` call site updated. Adding the `(uint32_t *)` cast
>   on the `uart_check_idle_state` call removed a pre-existing incompatible-
>   pointer warning (**warnings 18 → 17**). Pure rename/move: **text unchanged
>   at 27 860 B**, links clean. No `flash_*`/`dma_*` UART misnomers remain.
>
> **2026-05-29 (HardFault — vector-mislabel discovery)** — "decomp HardFault"
> resolved to a labelling bug, not a 3690-byte translation. The vector table
> (after the 0x28-byte image header) holds **runtime** addresses (link base
> 0x08005000), so slot 3 = `0x0800B329` maps to Ghidra `0x08006328` (−0x5000),
> which is **`system_reset_simple`** — a clean reset-on-fault. The Ghidra
> function named `HardFault_Handler` at `0x0800B328` is a mislabel: that value
> was used as a Ghidra address without the −0x5000 offset, landing in the
> middle of the big UART telemetry/command processor (no prologue — it reads
> `r7`/`r1` live, hence the `unaff_r7` decompile). Verified `0x08006328`
> disassembles to `push;bl system_reset;pop`.
>   - Wired the HardFault vector in `startup_stm32l072.S` directly to
>     `system_reset_simple` (was a weak `Default_Handler` infinite loop);
>     dropped its weak alias. **Links clean: text 27 860 → 27 864 B** (+4:
>     `system_reset_simple` is now reachable/kept).
>   - Renamed `0x0800B328` → `uart_cmd_report_blk_b328` in Ghidra; plate
>     comment added on `0x08006328` marking it the real HardFault.
>   - **Same −0x5000 defect affects the `Reset_Handler` (0x080131F8) and
>     `NMI_Handler` (0x0801324C) labels** — their real entries are at Ghidra
>     0x0800E1F8 / 0x0800E24C (currently "No function found"), confirming the
>     pattern. Rows flagged. (Our startup.S Reset_Handler is independently
>     correct.) The `EXTI*` "handler" labels are suspected to share the defect.
>
> **2026-05-29 (vector table re-pointed to real entries)** — decoded all 48
> OEM vectors through the −0x5000 runtime→Ghidra offset instead of translating
> the mislabeled blobs:
>   - **NMI / SVCall / PendSV and every unused IRQ** → runtime 0x0801324D →
>     Ghidra 0x0800E24C = `b .` trap loop = **our `Default_Handler`**. Already
>     faithful; no change.
>   - **HardFault** → `system_reset_simple` (done previously).
>   - **EXTI0_1 (IRQ5)** → 0x0800724C and **EXTI4_15 (IRQ7)** → 0x08007278 were
>     already implemented but **mis-named** `uart_check_parity_error` /
>     `uart_check_overrun_error` — 0x40010400 is the **EXTI** base (offset 0x14
>     = EXTI_PR), not USART1. They clear EXTI line 0 (PB0 button) and line 13
>     (PC13 power button) and set bits @ 0x20002BFC. Renamed to
>     `EXTI0_1_IRQHandler` / `EXTI4_15_IRQHandler` (uart.c) — strong defs now
>     override the weak `Default_Handler` aliases, so the vector runs the real
>     button ISRs instead of trapping. **Verified in the linked vector table.**
>   - The big `EXTI0_1_IRQHandler`/`EXTI4_15_IRQHandler` Ghidra blobs (0x0800C24C
>     / 0x0800C278) were the +0x5000 mislabels → renamed `cmd_proc_blk_c24c/c278`.
>   - **ADC1_COMP (IRQ12)** → 0x080004C4: real ISR, function created + named in
>     Ghidra, translated in a follow-up (see next entry).
>   - SysTick / IRQ25 / IRQ27 point to runtime-installed SRAM trampolines
>     (0x2000199C / 0x20000E70 / 0x20001AA0) — can't be wired statically.
>   **Links clean: text 27 864 → 27 936 B** (+72: the two EXTI ISRs are now
>   reachable/kept). All vector slots verified against the linked `.vector`.
>
> **2026-05-29 (ADC1_COMP_IRQHandler translated)** — decompiled the last
> statically-wireable real handler at Ghidra `0x080004C4` (vector slot 28,
> IRQ12; OEM runtime `0x080054C5`). It is the STM32L0 ADC EOC/OVR ISR for the
> HAL-style handle at `0x200024F4` (`handle[0]` = ADC instance, runtime
> `0x40012400`). Behaviour:
>   - **EOC** (ISR/IER bit 2): store `DR & 0xFFF` into the u16 sample buffer
>     `@ 0x20002558` at index `cell(0x20002550)*4 + conv(0x20002582)`, then
>     `conv++`. This is the buffer `cell_balance_update()` consumes.
>   - **EOS** (bit 3) on a software-triggered single sequence: disable
>     EOCIE/EOSIE, mark `State` READY (clear REG_BUSY); set bit 0 of the
>     sequence-ready flag `@ 0x20002554`.
>   - **OVR** (bit 4): record `HAL_ADC_ERROR_OVR` in the handle `ErrorCode`
>     (`+0x58`) and clear the flag.
>   The `0x20002550/54/82` globals are the same storage the DMA capture path
>   in `dma.c` names `s_dma_*` (cross-referenced in comments; not refactored).
>   Translated in `src/fuel_gauge.c` (next to `cell_balance_update`); prototype
>   added to `batteryware.h`. The strong C def overrides the weak
>   `Default_Handler` alias, so IRQ12 now runs the real ISR — **verified: linked
>   vector slot 28 (`0x08005098`) = `0x0800AE85`** (was the `Default_Handler`
>   trap `0x0800B899`). **Links clean: text 27 936 → 28 204 B** (+268); 17
>   warnings (all pre-existing in `cell_balance_update`, none in the new ISR).
>   With this, every statically-wireable exception/IRQ vector now points at
>   real code; only the SysTick/IRQ25/IRQ27 SRAM trampolines remain (installed
>   at runtime, can't be wired from the image).
>
> The VanMoof image header is byte-equal to the OEM across 0x10..0x27:
> the build date/time field (0x10..0x24) is all-zero in the OEM, so the
> earlier placeholder `"May 25 2026"`/`"00:00:00"` strings were removed.
> The remaining header diffs are the CRC32 (0x08, post-build patch) and
> imageSize (0x0C, linker-computed — will match once fully decompiled).
>
> The linker base has been corrected to `0x08005000` (the application
> slot above `bmsboot`); literal-pool references like `AHBPrescTable`
> at `0x080181E8` and `PLLMulTable` at `0x08018200` resolve cleanly
> with this base.
>
> **2026-05-29 (rodata materialised — read-only data as real C)** — with the
> OEM image now available
> (`VanMooof-Firmware/ES3/batteryware/batteryware_1.17.1.bin`), audited every
> absolute-address flash data read in the source. Two strategic findings:
> (1) **byte-equivalence is out of reach** — `make compare` shows a uniform
> ~97% diff because our linker places functions at different addresses than the
> OEM (and the original toolchain's codegen would differ regardless); the
> realistic target is a behaviour-faithful real-C reconstruction. (2) The
> magic flash addresses are a decomp artefact — the original used **symbols**,
> so converting magic-address reads to symbol references is both the better
> source representation and removes the need to pin data at fixed addresses
> (symbol resolution makes every read valid wherever the linker places it).
> Changes:
>   - **Numeric tables materialised as C arrays** (were absolute reads pointing
>     past our image): `s_rsoc_table[101]` u16 (was `0x080174B0`),
>     `s_fg_shift_table[8]` u8 (was `0x080181F8`), `s_cell_score_lut[146]` u32
>     (was `0x08017698`) — all in `fuel_gauge.c`, values extracted verbatim from
>     the OEM image.
>   - **`PLLMulTable`** un-`static`'d in `rcc.c` and shared with `tick.c`'s
>     `clock_prescaler_val` (both index the same OEM table at `0x08018200` by the
>     same PLLMUL field); declared in `batteryware.h`. `tick.c` was reading an
>     absolute address that fell past our image — now reads the real symbol.
>   - **8 log/format strings repointed** from absolute addresses to the existing
>     `strings.c` symbols (`s_fedl5236_init`, `s_predischg_lock_ctr`,
>     `s_max_cell_voltage`…`s_rsoc_adjust`) in `bms_init.c`, `bms_setup.c`,
>     `state_handlers.c`. The strings were already in the image by symbol; the
>     code just reached them by the wrong (OEM) address.
>   - `s_rsoc_table`, `s_fg_shift_table`, `PLLMulTable` and the strings now all
>     resolve to valid in-image symbols (≈`0x0800bxxx`); **links clean, text
>     28 204 → 28 400 B, 17 warnings (no regression).**
> **Flagged for follow-up (NOT fixed here):**
>   - `s_cell_score_lut` is defined in source but the optimiser **elides it**:
>     with `cell_balance_update`'s current `result` formula, `result` is provably
>     in [234,250] (12-bit ADC) while the smallest LUT entry is 0x358 (856), so
>     the scoring loop always yields `0x91` and the table is dead. The OEM keeps
>     the table live → the result-scaling (MUL=610/DIV=10000/VREF=2500000) is
>     **suspect** and needs review when `cell_balance_update` is verified.
>   - **Code-as-data decomp bugs discovered** (a function call mistranslated into
>     a data read; the OEM bytes at these addresses are Thumb code, not data):
>     `modem.c:120/126/130` (`0x08007204`/`0C`/`10`/`14`), `fuel_gauge.c:540`
>     (`s_cell_voltage_table` @ `0x08013E50`), `tick.c:245` (`s_shifts` @
>     `0x080107D8`). These are code-translation issues for the code phase, not
>     rodata.
>
> **2026-05-29 (image-header finalisation — CRC + imageSize)** — the build now
> emits a header that passes the OEM's own verification. Reverse-engineered the
> CRC by decompiling batteryware's `flash_verify_header` (runtime `0x0800B340`):
> it copies the 40-byte header to scratch, forces bytes `[8:16)` (the CRC and
> imageSize fields) to `0xFFFFFFFF`, then runs the **STM32L0 hardware CRC**
> (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reversal, no final XOR; words fed
> big-endian-order) over the patched header followed by `[0x28 : imageSize]` —
> i.e. the whole image with those two fields masked. Feeding little-endian
> words to the unit equals byte-swapping each 32-bit word and running
> CRC32/MPEG-2. **Verified byte-exact against both OEM images** (1.17.1 →
> `0x2E0150DA`, 1.14.1 → `0x27F3DE41`).
>   - Added `tools/patch_image_header.py` and wired it into the Makefile `.bin`
>     rule: after `objcopy`, it sets `imageSize` (+0x0C) to the file length and
>     writes the computed CRC32 to +0x08. Our image now self-verifies:
>     `imageSize=28400`, `crc32=0xC8E9A976`, recompute == stored (PASS), so
>     bmsboot's gate would accept it.
>   - **Flagged:** our C `flash_verify_header` (currently dead-code-eliminated)
>     models the hardware-CRC+DMA path as a software `crc32_mpeg2()` call that is
>     only `extern`-declared (never defined — harmless while gc'd). Reworking it
>     to drive the HW CRC peripheral via `dma_transfer`/`dma_transfer_irq`, like
>     the OEM, is a code-phase task.
>
> **2026-05-30 (cell_balance_update score math fixed)** — the previously-flagged
> result formula was wrong. Decompiled the OEM `cell_balance_update`
> (`0x08000880`): the per-cell/phase score input is a 64-bit **rational**
> transfer curve, not the linear `(2500000 - adc·610²/10000)/10000` our decomp
> had. Read the literal pool (`be4=610`, `be8=10000`, `bec=2500000`) and the
> 64-bit `lmul`/`lmul`/sub/`ldivmod` sequence at `0x0800093a–0x080009c4`:
>   `result = (adc · 610 · 10000) / (2500000 − adc · 610)`
> computed entirely in 64-bit (shared sub-expr `p1 = adc·610` in both numerator
> and denominator). With a 12-bit ADC, `p1 ≤ 2,497,950` so the denominator stays
> ≥ 2050 and `result` spans ~[0, 1.2e7] — covering the full descending LUT range
> `[0x358 .. 0x2f2fe]`. Replaced the formula in `fuel_gauge.c`; the optimiser no
> longer folds `score` to `0x91`, and **`s_cell_score_lut` is now retained**
> (`0x0800c0c0`, 584 B) and actually indexed. **Links clean, text 28 400 →
> 29 668 B, 17 warnings (no regression).** This resolves the cell-scoring item
> flagged in the rodata entry above.
>
> **2026-05-30 (three code-as-data bugs fixed)** — the trio flagged in the
> rodata entry turned out to be **literal-pool addresses** the decomp had
> passed where the pool *contents* (pointers/values) belong, not Thumb code:
> - `modem.c::bootloader_entry` — the two `uart_printf` args `0x08007204` /
>   `0x0800720C` were literal-pool slots holding runtime pointers `0x080171C0`
>   / `0x080171D0` (→ Ghidra `0x080121C0` / `0x080121D0` = `"\nShipping Mode\r"`
>   / `"\nFEDL5236_PowerDown_Start()\r"`); repointed to the existing
>   `s_shipping_mode` / `s_pdown_start` symbols. `memcmp_verify(0x08007214,
>   0x80, 0x08007210)` → the real operands `(0x08080C00, 0x80, 0x200029A8)`
>   (EEPROM config block vs RAM copy — identical to `state_handlers.c:442`).
>   Also fixed the exit mask `0xFFFFFFFD` → `0xFFFFFDFF` (clears CR bit 9, not
>   bit 1) and corrected the stale "I am VanMoof AP" comment.
> - `fuel_gauge.c::fg_cell_balance` — `s_cell_voltage_table` was pointed at the
>   literal pool `0x08013E50`; the pool holds the real SRAM operands
>   `0x200027D4` (cell-voltage array), `0x2000286E` (mid-cell average), and
>   `0x200027FE` (cached count / last-balanced index) — all cross-confirmed by
>   `calculate_rsoc`. Control flow was already correct.
> - `tick.c::clock_config` — this whole function was a **dead, wrong duplicate**
>   of `FUN_08010554` (it modelled a fictional `RCC[0x4C]` register). The
>   faithful decomp already exists as `rcc.c::rcc_configure` (HAL_RCC_ClockConfig
>   over FLASH→ACR + RCC→CR/CFGR, called from `main`). Removed the duplicate and
>   its header prototype; the `s_shifts @0x080107D8` bug went with it.
> **Links clean, text 29 716 B, 16 warnings (one fewer — no regression).**
>
> **2026-05-30 (UART command processor mapped + veneers resolved)** — began the
> big remaining Gap-2 item. Established that `uart_cmd_report_blk_b328`
> (0x0800b328), `cmd_proc_blk_c24c` (0x0800c24c) and `cmd_proc_blk_c278`
> (0x0800c278) are **not** three functions (nor vector handlers) — they are
> spurious interior labels inside **one** ~2.5 KB function, `FUN_0800afa4`, the
> UART command/telemetry processor: per-byte RX state machine (sync 0xAA →
> command validated against bitmask 0x10048 → accumulate into buf 0x20004548 →
> crc16 check) that falls through into a ~0x60-field telemetry cascade and a
> command-dispatch tail (4 jump tables + the 0x80 modem/flash-update path).
> Resolved the SRAM-installed veneer trampolines: the OEM Reset_Handler
> (0x0800e1f8) copies .data from flash LMA 0x0801821C → SRAM VMA 0x200000C0
> (len 0x23F0), so `veneer_11f38`/`_11eb8`/`_11ed8` are long-calls into the
> RAM-resident copies of `modem_send_2bytes` / `temp_offset_send` / `crc16_calc`
> respectively — all already decompiled. **Fidelity flag:** the existing
> `uart_protocol_handler` (src/uart.c) does **not** match FUN_0800afa4 (wrong
> SRAM addrs 0x20002CEC, invented `command_parser`/`veneer_a6aa`, no cascade) —
> it is a fabricated approximation slated for a full faithful rewrite. Structure
> + global map recorded for the rewrite; no code change yet this step.
>
> **2026-05-31 (DMA/flash leaf cluster reconstructed — OTA path now real).**
> The five `dma_lock`/`dma_byte_copy`/`memcpy_hw`/`dma_channel_reset_all`/
> `modem_deinit_thunk` stubs that the func-0x10 / OAD flash path linked through
> are gone, replaced by their real targets. Reading the OEM literal pools
> exposed a systemic defect: the "dma_*" pollers are actually **FLASH
> controller** accesses, but the decomp had guessed DMA bases.
>   - `dma_wait_for_ready` (FUN_0800f3ac) and `dma_wait_done` (FUN_08015360):
>     base `0x40020020` → **`0x40022000`** (poll FLASH_SR @ 0x40022018 BSY).
>     `dma_wait_done` also called an **undefined** `dma_fault_handler` (only
>     unlinked because it was dead-stripped) → now `dma_error_clear_v2`.
>   - `dma_error_clear` (FUN_0800f490) + `dma_error_clear_v2` (FUN_08015434,
>     byte-identical): status `0x40020020`→`0x40022000` (FLASH_SR), clear-base
>     `0x40020020`/`0x40020040`→**`0x200047E0`** (the SRAM error shadow @
>     0x200047F4).
>   - `atomic_copy_16words` (FUN_08015294): was a wrong incrementing copy with
>     SRAM base. It is **STM32L0 flash half-page programming** — FLASH_PECR
>     (0x40022004) FPRG+PROG, 16 words to the **fixed** latch (dst not
>     advanced), `dma_wait_done(50000)`, clear PROG then FPRG (0xFFFFFBFF).
>     `dma_compare` (misnamed) drives it to program N bytes in 0x40 chunks =
>     the OTA image-write path (`modem.c`).
>   - `dma_transfer`/`dma_transfer_irq` (FUN_0800eecc/ef5a): mode-3 word copy
>     also writes a **fixed** `*(ctx[0])` (was incrementing); mode-1/mode-2
>     rewired to the real `bytes_to_words`/`memcpy_halfword` (the stubs were
>     no-ops). `memcpy_halfword` (FUN_0800f11a) corrected: **double-deref**
>     `*(*dst_pp)` (sig now `uint32_t **`) and high-half-first packing
>     `(src[2i]<<16)|src[2i+1]`.
>   - `flash_word_write` (FUN_0800f264): `dma_lock`→`dma_wait_for_ready(50000)`;
>     fixed the error-shadow clear address `0x200047F8`→**`0x200047F4`**.
>   - `dma_channel_reset_all` (FUN_0800f5c8): real body (flash half-page erase
>     loop over cfg[1]..cfg[1]+cfg[2]*0x80, FLASH_PECR PROG/ERASE clears) in
>     `dma.c`; flipped **named → decomp-c**.
>   - `modem_deinit` (FUN_08010d84): `modem_deinit_thunk`→`modem_isr_ack()`;
>     bit-6 clear now targets `*(ctx[0])` not `ctx[0]`.
> **Links clean, text 39 972 → 40 744 B; full-build warnings 16 → 10 (the
> stub removals plus an -Wshadow fix in dma_error_clear, no regression).**
> Still dead-stripped/deferred: `dma_irq_copy` (FUN_0801518c, a separate real
> function whose only decomp still passes a literal address as the loop count).
>
> **2026-05-31 (image-header patcher consolidated).** The per-ware
> `tools/patch_image_header.py` moved to the shared repo-root
> `../tools/patch_image_header.py` (now cross-firmware, accepts multiple bins,
> rejects non-`0xAA55AA55` images). It absorbed shifterware's equivalent
> `patch_image_crc.py`; the Makefile `.bin` rule now calls `../tools/`. Output
> verified byte-identical (batteryware crc `0x45F432A1`, shifterware `0x342CE931`
> unchanged across the switch). See `tools/README.md`.
>
> **2026-05-31 (Renode test harness added).** `tests/` now carries a Renode
> harness: `batteryware.repl` (STM32L072 — CPU/NVIC/flash/SRAM/USART1-2),
> `batteryware.resc`, and `batteryware.robot` (run via `make renode-test`). The
> robot suite checks the vector table is well-formed, the reset/relocation chain
> reaches `main()` (peripheral-free, hard requirement), CPU liveness, and — with
> RCC/FLASH status registers pinned ready via `sysbus Tag` — that boot reaches the
> `uart_resp_handler` super-loop. A USART1 Modbus-RTU drive is scaffolded for
> follow-up. Authored against the linker map + reset disassembly; not yet run in
> CI (no Renode in the build sandbox). See `tests/README.md`.
>
> **2026-06-01 (console dispatch table located — not "past end of .bin").**
> The `cmd.c` comment claimed `command_parser`'s 24-pointer jump table at
> `0x08017FD8` was past the image end (`0x15610`) and stubbed every slot
> NULL. That was a runtime-vs-Ghidra address mix-up (the same −0x5000
> offset as the vector mislabels). The table is at *runtime* `0x08017FD8`
> = file `0x17fd8` in `bmsv007.bin` = Ghidra `0x08012FD8`, fully present.
> Verified the OEM cmd table (47-byte entries at file `0x17b9c`) maps
> `Reset BMS` → action 4 → `jump[4]` = runtime `0x0800ef20`. Decompiled
> that handler from the bin: rejects an `=VALUE` arg (`veneer_a6be`), arms
> a reset flag at SRAM `0x20002C48`, persists it to data-EEPROM
> `0x08080001` via `memcmp_verify`, prints `"\nOK\r"` (`@0x080172f4`),
> flushes the UART, then `NVIC_SystemReset()` (`SCB->AIRCR=0x05FA0004`,
> confirmed in `nvic_system_reset_v3` @ `0x0800eaa0`). Entries 0–9 are
> in-file (handler addrs documented in `cmd.c`); 10–23 fall past the
> `0x18000` dump end. **No code change** — the handlers are frame-sharing
> `mov pc` continuations of `command_parser`, so `s_jump_table` stays NULL
> until that dispatch is reworked; only the misleading comment and the
> `protocol.md` console section were corrected.
## Functions

| Addr | Size | Name | Status | Notes |
| --- | --- | --- | --- | --- |
| `0x08000130` | 266 | `__aeabi_uidivmod` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x0800023c` | 6 | `__aeabi_uidiv` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000244` | 2 | `trap_div0` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000248` | 56 | `__aeabi_ldiv0` | decomp-c | 64-bit div-by-zero handler (returns 0xFFFFFFFFFFFFFFFF) |
| `0x08000288` | 80 | `__aeabi_lmul` | deferred | ARM runtime 64-bit multiply — provided by `-lgcc` (OEM has its own byte-divergent copy at this address). No C needed. |
| `0x080002d8` | 406 | `__aeabi_ldivmod` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000470` | 22 | `__clz64` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000488` | 42 | `__clzsi2` | deferred | ARM runtime count-leading-zeros — provided by `-lgcc` (OEM has its own byte-divergent copy). No C needed. |
| `0x080004c4` | 380 | `ADC1_COMP_IRQHandler` | decomp-c | ADC EOC/OVR ISR (vector slot 28, IRQ12 = 0x080054C5 runtime − 0x5000). EOC → store DR&0xfff into u16 buffer @ 0x20002558; EOS → set seq-ready flag @ 0x20002554; OVR → ErrorCode. Translated in `src/fuel_gauge.c`; strong def overrides weak alias, vector now runs the real ISR |
| `0x08000658` | 392 | `main_clock_setup` | decomp-c | System clock tree initialisation |
| `0x0800080c` | 54 | `modem_init` | decomp-c | Modem peripheral initialisation |
| `0x08000850` | 34 | `dma_stop` | decomp-c | Stop all DMA transfers |
| `0x08000880` | 1614 | `cell_balance_update` | decomp-c | Cell balancing: voltage measurement and FET control |
| `0x08001060` | 72 | `state_handler_0b` | decomp-c | BMS state handler (0b) |
| `0x080010b4` | 272 | `state_timer_0b` | decomp-c | BMS state timer (0b) |
| `0x080011d8` | 72 | `state_handler_0c` | decomp-c | BMS state handler (0c) |
| `0x0800122c` | 396 | `state_timer_0c` | decomp-c | BMS state timer (0c) — discharge-OC + recovery debounce |
| `0x080013e0` | 72 | `state_handler_12` | decomp-c | BMS state handler (12) |
| `0x08001434` | 396 | `state_timer_12` | decomp-c | BMS state timer (12) — sibling of 0c, cfg_blk[0x7a/0x7c] |
| `0x080015e8` | 72 | `state_handler_13` | decomp-c | BMS state handler (13) |
| `0x0800163c` | 562 | `state_timer_13` | decomp-c | BMS state timer (13) — full FG check suite + PB9 charge toggle vs cfg_blk[0x16] |
| `0x08001898` | 72 | `state_handler_02` | decomp-c | BMS state handler (02) |
| `0x080018ec` | 46 | `capacity_decrement` | decomp-c | Decrement capacity counter by amount |
| `0x08001920` | 132 | `rsoc_lookup` | decomp-c | Lookup RSOC from state-of-charge table |
| `0x080019b8` | 358 | `state_timer_15` | decomp-c | BMS state timer (15) — discharge-OC >9-tick -> state_handler_02 |
| `0x08001b44` | 90 | `state_handler_0d` | decomp-c | BMS state handler (0d) |
| `0x08001bb4` | 364 | `state_timer_0d` | decomp-c | BMS state timer (0d) |
| `0x08001d40` | 90 | `state_handler_0e` | decomp-c | BMS state handler (0e) |
| `0x08001db0` | 384 | `state_timer_0e` | decomp-c | BMS state timer (0e) |
| `0x08001f50` | 72 | `state_handler_14` | decomp-c | BMS state handler (14) |
| `0x08001fa4` | 378 | `state_timer_14` | decomp-c | BMS state timer (14) |
| `0x08002140` | 72 | `state_handler_15` | decomp-c | BMS state handler (15) |
| `0x08002194` | 2406 | `bms_state_machine` | decomp-c | Main BMS state machine — full pass (dual-pack MOSFET timing, OVP recovery, precharge SM) |
| `0x08002ba6` | 6 | `nop_2ba6` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08002bac` | 6 | `nop_2bac` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08002bc8` | 202 | `state_handler_03_init` | decomp-c | BMS state handler (03_init) |
| `0x08002cb8` | 142 | `discharge_mosfet_set` | decomp-c | Control discharge MOSFET (on/off) |
| `0x08002d50` | 106 | `charge_mosfet_set` | decomp-c | Control charge MOSFET (on/off) |
| `0x08002dc4` | 894 | `ymodem_receive` | decomp-c | YMODEM firmware receive protocol |
| `0x08003188` | 60 | `ymodem_send_byte` | decomp-c | YMODEM byte transmit |
| `0x080031d8` | 120 | `fg_watchdog_kick` | decomp-c | Fuel gauge watchdog reset |
| `0x0800325c` | 1314 | `fg_scan` | decomp-c | Fuel gauge register scan/poll loop |
| `0x080039c2` | 2860 | `fg_coulomb_update` | decomp-c | Fuel gauge coulomb counter update |
| `0x08004634` | 164 | `fg_read_loop` | decomp-c | Fuel gauge readout loop |
| `0x08004764` | 6 | `nop_4764` | decomp-c | Tail-merged function epilogue (shared return); the old `fg_read_done` "calls" were just branches here |
| `0x0800478c` | 296 | `smbus_read_nack` | decomp-c | SMBus read with NACK termination |
| `0x080048cc` | 312 | `smbus_read` | decomp-c | SMBus read transaction |
| `0x08004a18` | 724 | `smbus_write_reg` | decomp-c | SMBus register write transaction |
| `0x08004d04` | 864 | `bms_init` | decomp-c | BMS chip initialisation sequence |
| `0x080050ac` | 424 | `button_entry_check` | decomp-c | Bootloader/shipping power-down entry check (PB9 off, FEDL5236 POWER_DOWN, PA11 sample) |
| `0x0800527c` | 86 | `led_flash` | decomp-c | Flash status LED |
| `0x080052d8` | 148 | `bms_configure` | decomp-c | Send configuration byte to BMS IC |
| `0x0800537c` | 10 | `nop_537c` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08005388` | 278 | `state_flags_handler_timer` | decomp-c | Timer-driven state flag handler |
| `0x080054cc` | 198 | `state_handler_17_19` | decomp-c | BMS state handler (17_19) |
| `0x080055a8` | 366 | `can_transmit` | decomp-c | state-0x16 periodic timer (name is a decomp misnomer; charge-recovery debounce on s_cell[0]) |
| `0x08005738` | 72 | `state_handler_16` | decomp-c | BMS state handler (16) |
| `0x0800578c` | 26 | `nvic_system_reset` | decomp-c | NVIC system reset |
| `0x080057b0` | 666 | `main` | decomp-c | OEM `main` — early init, then **boot mode-report** (re-decomped 2026-05-29: latches power-on mode from ext-flash `0x08080001` → `0x20002C48`; 8-way "Power On" + 4-way "Power On Detect" UVP/OVP dispatch printing the `s_*_mode` strings via `uart_printf`; DP/VanMoof via GPIOB PB11), then state-machine super-loop. **Loop tail still suspect**: it gates on `0x20002C44` but the OEM gates on `s_bms_cfg` (`0x20002C00`) bits 0/1/2 and dispatches via a jump table at runtime `0x0801757C` — pending a separate pass. |
| `0x08005b34` | 818 | `bms_set_state` | decomp-c | Transition BMS state — full pass (two-stage switch, per-state counters, 0x38-B telemetry ring buffers) |
| `0x08006328` | 14 | `system_reset_simple` | decomp-c | **Real HardFault_Handler** (OEM vector slot 3 = 0x0800B329 runtime − 0x5000). Tail-calls `system_reset` → reset-on-fault (reset.c) |
| `0x08006336` | 10 | `system_reset` | decomp-c | System reset via NVIC |
| `0x08006340` | 142 | `flash_verify_header` | decomp-c | Verify flash image header CRC |
| `0x080063e0` | 788 | `state_timer_05` | decomp-c | Complex fault-dispatch engine with cell balancing + protection checks |
| `0x08006748` | 186 | `state_handler_01` | decomp-c | BMS state handler (01) |
| `0x08006810` | 292 | `state_timer_03` | decomp-c | Discharge monitoring state timer |
| `0x08006948` | 74 | `state_handler_07` | decomp-c | BMS state handler (07) |
| `0x0800699c` | 292 | `state_timer_06` | decomp-c | Discharge + alert monitoring |
| `0x08006ad4` | 74 | `state_handler_08` | decomp-c | BMS state handler (08) |
| `0x08006b28` | 292 | `state_timer_07` | decomp-c | Charge + alert monitoring |
| `0x08006c60` | 72 | `state_handler_0f` | decomp-c | BMS state handler (0f) |
| `0x08006cb4` | 292 | `state_timer_08` | decomp-c | Charge + alert (duplicate) |
| `0x08006dec` | 72 | `state_handler_10` | decomp-c | BMS state handler (10) |
| `0x08006e40` | 280 | `state_timer_09` | decomp-c | Discharge + charge monitoring |
| `0x08006f68` | 72 | `state_handler_11` | decomp-c | BMS state handler (11) |
| `0x08006fbc` | 288 | `state_timer_10` | decomp-c | DMA/USART init with threshold config |
| `0x080070f8` | 78 | `modem_reinit` | decomp-c | Modem reinitialisation |
| `0x08007158` | 20 | `system_reset_with_arg` | decomp-c | System reset with stored argument |
| `0x0800716c` | 10 | `nop_716c` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08007178` | 132 | `bootloader_entry` | decomp-c | Jump to bootloader |
| `0x0800721c` | 10 | `nop_721c` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08007228` | 26 | `nvic_system_reset_dup` | decomp-c | `__NVIC_SystemReset` copy (reset.c) |
| `0x0800724c` | 36 | `EXTI0_1_IRQHandler` | decomp-c | **Real EXTI0_1 ISR** (vector slot 21, IRQ5): clears EXTI_PR line 0 (PB0 button), sets flag bit1 @ 0x20002BFC. Was mis-named `uart_check_parity_error` (0x40010400 is EXTI, not USART1). Wired in startup.S |
| `0x08007278` | 40 | `EXTI4_15_IRQHandler` | decomp-c | **Real EXTI4_15 ISR** (vector slot 23, IRQ7): clears EXTI_PR line 13 (PC13 power button, 0x2000), sets flag bit0 @ 0x20002BFC. Was mis-named `uart_check_overrun_error`. Wired in startup.S |
| `0x080072a8` | 156 | `batteryware_main` | decomp-c | Main application entry (after init) |
| `0x08007368` | 1220 | `config_init` | decomp-c | BMS config struct init from EEPROM (0xB8 bytes) |
| `0x080078c8` | 924 | `bms_setup` | decomp-c | Full BMS init: clears SRAM, configures GPIO, loads EEPROM, calls FEDL5236 init, validates calibration, recalculates RSOC |
| `0x08007cf8` | 64 | `memcpy_byte` | decomp-c | Byte-by-byte memory copy |
| `0x08007d38` | 62 | `system_init` | decomp-c |  |
| `0x08007d78` | 430 | `gpio_init_buttons` | decomp-c | GPIO port A-D pin init + button/LED/charge MOSFET setup |
| `0x08007f50` | 100 | `modem_config` | decomp-c | Modem configuration |
| `0x08007fc4` | 80 | `uart_puthex_byte` | decomp-c | UART transmit byte as hex |
| `0x08008014` | 148 | `uart_puthex_16` | decomp-c | UART transmit 16-bit as hex |
| `0x080080a8` | 256 | `uart_puthex_32` | decomp-c | Print uint32 as 8 hex chars via uart_putchar |
| `0x080081a8` | 1872 | `uart_putdec_64` | decomp-c | Print uint64 as decimal via uart_putchar with division chain |
| `0x08008998` | 148 | `uart_printf` | decomp-c | Printf-like UART formatter (%x, %i, %d, %l, %s, %w) |
| `0x08008f28` | 68 | `nibble_to_hex` | decomp-c | Nibble to ASCII hex character |
| `0x08008f6c` | 56 | `hex_to_nibble` | decomp-c | ASCII hex character to nibble |
| `0x08008fa4` | 210 | `peripheral_init` | decomp-c | Peripheral clock and GPIO init |
| `0x08009084` | 64 | `dma_init` | decomp-c | DMA controller initialisation |
| `0x080090dc` | 110 | `flash_dma_start` | decomp-c | Start flash DMA operation |
| `0x0800915c` | 106 | `dma_compare` | decomp-c | DMA memory compare |
| `0x080091d4` | 214 | `flash_write_verify` | decomp-c | Flash write with verify |
| `0x080092b8` | 238 | `word_to_bytes` | decomp-c | Split word into byte array |
| `0x080093a6` | 108 | `memcmp_verify` | decomp-c | EEPROM/SPI write-verify: per byte, `spi_register_write(0,&actual[i],expected[i])` re-read until it sticks. In `src/spi.c` (2026-05-31). NOT a memcmp. |
| `0x08009412` | 88 | `memcpy` | decomp-c | Memory copy (byte count) |
| `0x0800946c` | 92 | `delay_ms` | decomp-c | Millisecond delay loop |
| `0x080094d4` | 24 | `fault_led_trigger` | decomp-c | Trigger fault LED pattern |
| `0x080094ec` | 40 | `charge_mosfet_on` | decomp-c | Turn charge MOSFET on |
| `0x08009520` | 38 | `charge_mosfet_off` | decomp-c | Turn charge MOSFET off |
| `0x08009558` | 106 | `fg_uvp1_check` | decomp-c | Under-voltage protection level 1 check |
| `0x080095d4` | 106 | `fg_uvp2_check` | decomp-c | Under-voltage protection level 2 check |
| `0x08009650` | 106 | `fg_ovp1_check` | decomp-c | Over-voltage protection level 1 check |
| `0x080096cc` | 106 | `fg_ovp2_check` | decomp-c | Over-voltage protection level 2 check |
| `0x08009748` | 86 | `fg_threshold_check` | decomp-c | Fuel gauge voltage/current threshold check |
| `0x080097b0` | 76 | `fg_alert_monitor` | decomp-c | Fuel gauge alert register monitor |
| `0x0800980c` | 118 | `fg_discharge_oc_check` | decomp-c | Discharge over-current check |
| `0x0800989c` | 118 | `fg_charge_oc_check` | decomp-c | Charge over-current check |
| `0x0800992c` | 52 | `config_resend_all` | decomp-c | Resend all configuration registers |
| `0x0800997c` | 128 | `fg_charge_status` | decomp-c | Fuel gauge charging status check |
| `0x08009a10` | 46 | `fg_status_flag_get` | decomp-c | Read fuel gauge status flag |
| `0x08009a44` | 54 | `fg_status_flag2_get` | decomp-c | Read fuel gauge status flag 2 |
| `0x08009a80` | 22 | `fg_clear_status` | decomp-c | Clear fuel gauge status register |
| `0x08009aa0` | 26 | `nvic_system_reset_v3` | decomp-c | `__NVIC_SystemReset` copy (reset.c) |
| `0x08009ac4` | 820 | `command_parser` | decomp-c | KEY=VALUE command parser with 24-entry dispatch table |
| `0x0800a6aa` | 4 | `veneer_a6aa` | deferred | Thunk/veneer to another function |
| `0x0800a6ba` | 4 | `veneer_a6ba` | deferred | Thunk/veneer to another function |
| `0x0800a6be` | 4 | `veneer_a6be` | deferred | Thunk/veneer to another function |
| `0x0800a6e0` | 14 | `nop_a6e0` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800a70c` | 134 | `atoi_hex_offset1` | decomp-c | Hex string to int (offset=1) |
| `0x0800a794` | 374 | `state_timer_charge_a` | decomp-c | Charge monitoring with bootloader entry threshold |
| `0x0800a934` | 72 | `state_handler_09` | decomp-c | BMS state handler (09) |
| `0x0800a988` | 374 | `state_timer_charge_b` | decomp-c | Charge monitoring variant (duplicate) |
| `0x0800ab28` | 72 | `state_handler_0a` | decomp-c | BMS state handler (0a) |
| `0x0800ab7c` | 348 | `service_uart_init` | decomp-c | USART1 service-UART bring-up, gated on PA10 held (was mislabelled `phase2_init`) |
| `0x0800ad00` | 88 | `uart_puts` | decomp-c | UART string transmit |
| `0x0800ad64` | 76 | `uart_putchar` | decomp-c | UART single character transmit |
| `0x0800adbc` | 272 | `uart_resp_handler` | decomp-c | UART RX-ring drain (polled from main): feeds each byte to uart_protocol_handler / command_parser, or ymodem_receive per the 0x20002C00 mode word. (Was stubbed to uart_tx_flush; now the real loop — wires the command processor in.) |
| `0x0800aee4` | 138 | `uart_tx_isr` | decomp-c | UART TX interrupt handler |
| `0x0800af80` | 32 | `uart_tx_flush` | decomp-c | Flush UART TX buffer |
| `0x0800afa4` | ~2500 | `uart_protocol_handler` | decomp-c | **Head of the one big UART command processor** (FUN_0800afa4) — faithful: RX state machine (mask 0x10048 ⇒ cmd 3/6/0x10) + full ~120-field telemetry cascade + report-table switch; cmd 0x10 → flash_stream_handler, cmd != 3 → modem_command_dispatch. Subsumes the b328/c24c/c278 interior blocks. Wired in via the rebuilt uart_resp_handler. |
| `0x0800b328` | 3690 | `uart_cmd_report_blk_b328` | mislabel | **Interior block of FUN_0800afa4** (telemetry-report cascade), not a function — folded into `uart_protocol_handler`. NOT HardFault (real = `system_reset_simple` @ 0x08006328). |
| `0x0800c24c` | 44 | `cmd_proc_blk_c24c` | mislabel | **Interior block of FUN_0800afa4**, folded into `uart_protocol_handler`. NOT EXTI0_1 (real @ 0x0800724C). |
| `0x0800c278` | 2970 | `cmd_proc_blk_c278` | mislabel | **Interior block of FUN_0800afa4**, folded into `uart_protocol_handler`. NOT EXTI4_15 (real @ 0x08007278). |
| `0x0800ce9e` | 1690 | `modem_command_dispatch` | decomp-c | Dispatch tail of FUN_0800afa4 (cmd != 3): counter-store (0xF020-23), history dump (0xF45), shipping (0x95), per-command config tables (cw<0x1b), OAD firmware-update (0x80), default echo/bootloader/config-bit tail. |
| `0x0800d75e` | 34 | `cmd_counter_inc` | decomp-c | Increment command counter |
| `0x0800d780` | 34 | `cmd_counter_inc_v2` | decomp-c | Increment command counter (v2) |
| `0x0800d7fc` | 34 | `cmd_counter_inc_v3` | decomp-c | Increment command counter (v3) |
| `0x0800d81e` | 40 | `cmd_write_and_inc` | decomp-c | Write command and increment counter |
| `0x0800d846` | 4 | `cmd_send_response_stub` | decomp-c | Empty cmd_send_response stub |
| `0x0800d84a` | 4 | `cmd_send_response_stub2` | decomp-c | Empty cmd_send_response stub (duplicate) |
| `0x0800d850` | 70 | `cmd_send_response` | decomp-c | Send command response |
| `0x0800d896` | 48 | `cmd_send_8byte` | decomp-c | Send 8-byte command response |
| `0x0800d8c6` | 16 | `protocol_reset` | decomp-c | Clears protocol state counter at 0x20002D8C |
| `0x0800d8f0` | 2152 | `flash_stream_handler` | decomp-c | cmd-0x10 streaming: header+CRC16, EEPROM calibration cascade, cw==0x82 OAD flash-page program |
| `0x0800e1b4` | 8 | `thunk_e1b4` | deferred | Thunk to another function |
| `0x0800e1bc` | 8 | `thunk_e1bc` | deferred | Thunk to another function |
| `0x0800e1c4` | 4 | `thunk_e1c4` | deferred | Thunk to another function |
| `0x0800e1c8` | 2 | `nop_e1c8` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800e1ca` | 14 | `epilogue_e1ca` | decomp-c | Function epilogue / return stub |
| `0x0800e250` | 58 | `peripheral_reset` | decomp-c | Peripheral reset with timeout check |
| `0x0800e290` | 10 | `epilogue` | decomp-c | Empty return thunk |
| `0x0800e29c` | 92 | `flash_timeout_check` | decomp-c | Flash operation timeout check |
| `0x0800e304` | 14 | `tick_get` | decomp-c | Get tick counter value |
| `0x0800e318` | 14 | `tick_val_get` | decomp-c | Get tick value |
| `0x0800e32c` | 14 | `tick_ms_get` | decomp-c | Get millisecond tick |
| `0x0800e340` | 14 | `tick_timeout_get` | decomp-c | Get timeout tick value |
| `0x0800e354` | 718 | `flash_op_start` | decomp-c | Start flash operation with DMA |
| `0x0800e63c` | 286 | `usart1_dma_setup` | decomp-c | USART1 DMA configuration |
| `0x0800e774` | 16 | `nop_e774` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800e784` | 16 | `nop_e784` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800e794` | 224 | `dma_deinit` | decomp-c | DMA deinitialisation |
| `0x0800e878` | 250 | `nvic_reconfigure` | decomp-c | Reconfigure NVIC interrupt priorities |
| `0x0800e984` | 188 | `flash_program_start` | decomp-c | Start flash program operation |
| `0x0800ea44` | 192 | `flash_erase_start` | decomp-c | Start flash erase operation |
| `0x0800eb04` | 140 | `flash_wait_ready` | decomp-c | Wait for flash ready |
| `0x0800eb90` | 54 | `delay_us` | decomp-c | Microsecond delay loop |
| `0x0800ebd0` | 48 | `nvic_enable_irq` | decomp-c | Enable NVIC interrupt |
| `0x0800ec04` | 62 | `nvic_enable_irq_dsb` | decomp-c | Enable NVIC interrupt with DSB |
| `0x0800ec48` | 212 | `interrupt_set_priority` | decomp-c | Set interrupt priority |
| `0x0800ed24` | 66 | `flash_page_erase` | decomp-c | Flash page erase with timeout |
| `0x0800ed6c` | 42 | `flash_opt_byte_op` | decomp-c | Flash option byte operation |
| `0x0800ed96` | 32 | `nvic_enable_irq_s` | decomp-c | Enable NVIC interrupt (short) |
| `0x0800edb6` | 32 | `nvic_enable_irq_s_dsb` | decomp-c | Enable NVIC interrupt (short) with DSB |
| `0x0800edd6` | 26 | `flash_erase_page_wrapper` | decomp-c | flash_page_erase boolean wrapper |
| `0x0800edf0` | 200 | `crc_init` | decomp-c | HAL_CRC_Init — re-id'd from `dma_channel_init`; 0x04C11DB7 = CRC-32 poly literal pool entry was misread as a CPAR address. Moved to crc.c |
| `0x0800eebc` | 16 | `crc_msp_init` | decomp-c | HAL_CRC_MspInit — weak empty stub. Re-id'd from `nop_eebc`; moved to crc.c |
| `0x0800eecc` | 142 | `dma_transfer` | decomp-c | DMA transfer setup |
| `0x0800ef5a` | 158 | `dma_transfer_irq` | decomp-c | DMA transfer with IRQ |
| `0x0800eff8` | 290 | `bytes_to_words` | decomp-c | Byte-stream to word-array packer (reverse of word_to_bytes) |
| `0x0800f11a` | 110 | `memcpy_halfword` | decomp-c | Halfword memory copy |
| `0x0800f188` | 218 | `crcex_polynomial_set` | decomp-c | HAL_CRCEx_Polynomial_Set — re-id'd from `dma_mem_config` (the "mask" was the polynomial, "size" was the POLYSIZE encoding). Moved to crc.c |
| `0x0800f264` | 110 | `flash_word_write` | decomp-c |  |
| `0x0800f2dc` | 148 | `flash_unlock_both` | decomp-c | Unlock flash (main + option) |
| `0x0800f384` | 36 | `flash_enable_prefetch` | decomp-c | Enable flash prefetch buffer |
| `0x0800f3ac` | 224 | `dma_wait_for_ready` | decomp-c | DMA status poll with timeout |
| `0x0800f490` | 302 | `dma_error_clear` | decomp-c | DMA error flag clear |
| `0x0800f5c8` | 188 | `dma_channel_reset_all` | decomp-c | Flash half-page erase loop (cfg[1]..+cfg[2]*0x80, step 0x80) via dma_channel_reset + dma_wait_for_ready; clears PROG/ERASE in FLASH_PECR. In `src/dma.c` (2026-05-31). |
| `0x0800f694` | 78 | `flash_unlock` | decomp-c | Unlock flash controller |
| `0x0800f6f0` | 166 | `spi_register_write` | decomp-c | Mutex-guarded register write; type 0=byte/1=halfword/2=word; polls `dma_wait_for_ready(50000)`. Type-code inversion + lock-helper bug fixed 2026-05-31. |
| `0x0800f7a0` | 58 | `dma_channel_reset` | decomp-c | Reset a DMA channel |
| `0x0800f7e4` | 732 | `gpio_pin_config` | decomp-c | Multi-pin GPIO mode/type/speed config |
| `0x0800fae0` | 424 | `gpio_pin_reset` | decomp-c | Reset GPIO pin configuration |
| `0x0800fca4` | 58 | `gpio_bit_read` | decomp-c | Read GPIO pin state |
| `0x0800fcde` | 58 | `gpio_bit_write` | decomp-c | Write GPIO pin state |
| `0x0800fd18` | 136 | `dma_flash_start` | decomp-c | Start DMA flash transfer |
| `0x0800fdac` | 1864 | `rcc_osc_config` | decomp-c | HAL_RCC_OscConfig — 7-phase multi-oscillator bring-up (HSE/HSI/MSI/LSI/LSE/HSI48/PLL) with per-phase RDY polling. Re-id'd from `bus_fault_reset` stub / `peripheral_op` placeholder. Lives in rcc.c |
| `0x0801053e` | 8 | `dma_get_status` | decomp-c | Returns passed register value (empty placeholder) |
| `0x08010554` | 622 | `rcc_configure` | decomp-c | HAL_RCC_ClockConfig — FLASH latency staging (raise/lower with 5 s timeout), HPRE/PPRE1/PPRE2 prescaler programming, SYSCLK source switch with SWS ack poll, SystemCoreClock recalc + HAL_InitTick. Re-id'd from `usart_bus_config` stub. Lives in rcc.c |
| `0x080107e4` | 312 | `clock_prescaler_val` | decomp-c | Calculate clock prescaler value |
| `0x08010930` | 14 | `tick_counter_read` | decomp-c | Read tick counter |
| `0x08010944` | 34 | `fg_read_field_8` | decomp-c | Read fuel gauge field (8-bit) |
| `0x08010970` | 34 | `fg_read_field_11` | decomp-c | Read fuel gauge field (11-bit) |
| `0x0801099c` | 640 | `rcc_reconfigure` | decomp-c | `HAL_RCCEx_PeriphCLKConfig` — RTC clock-domain (APB1ENR/PWR/CSR) + CCIPR peripheral-clock-source selection. Verified byte-by-byte; fixed every RCC register offset (were all off by a stray `/4`) |
| `0x08010c48` | 310 | `dma_usart_init` | decomp-c | Initialise DMA for USART |
| `0x08010d84` | 82 | `modem_deinit` | decomp-c | Modem deinitialisation |
| `0x08010dd6` | 16 | `modem_isr_ack_dup` | decomp-c | Modem ISR acknowledge (duplicate) |
| `0x08010de6` | 16 | `modem_isr_ack` | decomp-c | Modem ISR acknowledge |
| `0x08010df8` | 364 | `smbus_transmit` | decomp-c | SMBus transmit transaction |
| `0x08010f78` | 16 | `modem_restart_thunk` | decomp-c | Modem restart thunk |
| `0x08010f88` | 24 | `bus_ready_check` | decomp-c | Check bus ready status |
| `0x08010fa0` | 120 | `dma_byte_handler` | decomp-c | DMA byte transfer handler |
| `0x0801101c` | 58 | `dma_byte_done` | decomp-c | DMA byte transfer done |
| `0x08011056` | 144 | `dma_byte_handler_v2` | decomp-c | DMA byte handler (v2) |
| `0x080110e8` | 116 | `dma_halfword_handler_v2` | decomp-c | DMA halfword handler (v2) |
| `0x08011160` | 46 | `flash_op_cleanup` | decomp-c | Clean up after flash operation |
| `0x0801118e` | 142 | `dma_halfword_handler` | decomp-c | DMA halfword handler |
| `0x0801121c` | 274 | `timeout_poll` | decomp-c | Poll with timeout |
| `0x08011338` | 132 | `dma_timeout_copy` | decomp-c | DMA copy with timeout |
| `0x080113c4` | 282 | `dma_transfer_done` | decomp-c | DMA transfer completion handler |
| `0x080114ec` | 164 | `hal_uart_init` | decomp-c | `HAL_UART_Init` (uart.c) — was misnamed `flash_page_program` |
| `0x08011594` | 16 | `hal_uart_msp_init` | decomp-c | `HAL_UART_MspInit` (uart.c) — was misnamed `flash_program_init` |
| `0x080115a4` | 1218 | `uart_set_config` | decomp-c | `UART_SetConfig` (uart.c) — kernel-clock-source → BRR (OVER8/OVER16 for USART1/2; 256×freq/baud for LPUART1@0x40004800). Was misnamed `flash_prescaler_setup` |
| `0x08011b20` | 324 | `uart_adv_feature_config` | decomp-c | `UART_AdvFeatureConfig` (dma.c) — was misnamed `dma_channel_config` |
| `0x08011c88` | 140 | `uart_check_idle_state` | decomp-c | `UART_CheckIdleState` (dma.c) — was misnamed `dma_completion_handler` |
| `0x08011d18` | 248 | `timeout_poll_v2` | decomp-c | Poll with timeout (v2) |
| `0x08011e14` | 10 | `SystemInit` | decomp-c | OEM SystemInit — empty -O0 frame; byte-faithful asm in `startup_stm32l072.S` (opcodes match OEM exactly) |
| `0x08011e20` | 72 | `__libc_init_array_lite` | decomp-c | Standard newlib `__libc_init_array` (empty pre-init, `_init`, empty init array); byte-faithful asm in `startup_stm32l072.S` (only `bl _init` offset differs pre-convergence) |
| `0x08011e68` | 18 | `memset_byte_copy` | decomp-c | Memset byte copy |
| `0x08011e7a` | 16 | `memset_byte_fill` | decomp-c | Memset byte fill |
| `0x08011e8c` | 12 | `_init` | decomp-c | crti/crtn-style `_init` (no C ctors); byte-faithful asm in `startup_stm32l072.S` (opcodes match OEM exactly) |
| `0x08011e98` | 12 | `veneer_11e98` | deferred | Thunk/veneer to another function |
| `0x08011ea8` | 10 | `veneer_11ea8` | deferred | Thunk/veneer to another function |
| `0x08011eb8` | 10 | `veneer_11eb8` | deferred | Thunk/veneer to another function |
| `0x08011ec8` | 10 | `veneer_11ec8` | deferred | Thunk/veneer to another function |
| `0x08011ed8` | 10 | `veneer_11ed8` | deferred | Thunk/veneer to another function |
| `0x08011ee8` | 10 | `veneer_11ee8` | deferred | Thunk/veneer to another function |
| `0x08011ef8` | 10 | `veneer_11ef8` | deferred | Thunk/veneer to another function |
| `0x08011f08` | 10 | `veneer_11f08` | deferred | Thunk/veneer to another function |
| `0x08011f18` | 10 | `veneer_11f18` | deferred | Thunk/veneer to another function |
| `0x08011f28` | 10 | `veneer_11f28` | deferred | Thunk/veneer to another function |
| `0x08011f38` | 10 | `veneer_11f38` | deferred | Thunk/veneer to another function |
| `0x08011f48` | 10 | `veneer_11f48` | deferred | Thunk/veneer to another function |
| `0x08011f58` | 10 | `veneer_11f58` | deferred | Thunk/veneer to another function |
| `0x08011f68` | 10 | `veneer_11f68` | deferred | Thunk/veneer to another function |
| `0x08011f88` | 10 | `veneer_11f88` | deferred | Thunk/veneer to another function |
| `0x080131f8` | 128 | `Reset_Handler` | deferred | **CONFIRMED mislabel** (−0x5000 vector defect): this is not the reset handler — it overlaps the start of `coulomb_counter` (`0x08013228`, already decomp-c). The real reset vector (runtime 0x080131F9 → Ghidra 0x0800E1F8) is the `.data`-copy/bss-zero/SystemInit/main chain, faithfully reproduced in `startup_stm32l072.S`. No C needed here. |
| `0x08013228` | 1496 | `coulomb_counter` | decomp-c | Signed coulomb integrator (charge/discharge split, RSOC % update tail). Size corrected 2026-05-28. |
| `0x0801324c` | 502 | `NMI_Handler` | deferred | **CONFIRMED mislabel** (−0x5000 vector defect): the body is the coulomb-counter/RSOC-update tail (veneer_1556c/1557c, rsoc_set, +0x36 RSOC% / +0x24/+0x2c capacity) — i.e. it lies **inside** `coulomb_counter` (`0x08013228`, already decomp-c), not an NMI handler. The real NMI vector (runtime 0x0801324D → Ghidra 0x0800E24C) is a `b .` trap = our `Default_Handler`. No C needed here. |
| `0x08013800` | 90 | `rsoc_set` | decomp-c | Set relative state of charge |
| `0x08013860` | 64 | `calculate_rsoc` | decomp-c | RSOC percentage calculation helper |
| `0x080138ac` | 1196 | `cell_voltage_scan` | decomp-c | Two-pass scan: balance triggering + pair-fault flags + sum/avg/min/max + outlier patch + secondary/tertiary tables. Size corrected 2026-05-28. |
| `0x08013d88` | 198 | `fg_cell_balance` | decomp-c | Cell balancing control |
| `0x08014130` | 180 | `crc8_calc` | decomp-c | CRC-8 calculation |
| `0x080141e4` | 1834 | `crc8_for_smbus` | decomp-c | CRC-8/SMBus PEC (crc.c) — bitwise C, byte-divergent from OEM table version |
| `0x080149b8` | 186 | `state_flags_handler` | decomp-c | Handle BMS state flags |
| `0x08014a90` | 88 | `shipping_mode_check` | decomp-c | Check shipping mode condition |
| `0x08014af8` | 246 | `timer_scheduler` | decomp-c | Periodic timer with 1000/500/100/10ms divisors |
| `0x08014ea4` | 150 | `crc16_calc` | decomp-c | CRC-16 calculation |
| `0x08014f40` | 128 | `modem_send_2bytes` | decomp-c | Modem 2-byte send |
| `0x08014fd0` | 168 | `temp_offset_send` | decomp-c | Send temperature offset |
| `0x0801507c` | 36 | `flash_unlock_opt` | decomp-c | Unlock flash option bytes |
| `0x080150ac` | 36 | `flash_lock_opt` | decomp-c | Lock flash option bytes |
| `0x0801518c` | 248 | `dma_irq_copy` | decomp-c | DMA IRQ copy routine |
| `0x08015294` | 160 | `atomic_copy_16words` | decomp-c | Atomic 16-word copy |
| `0x08015340` | 26 | `get_tick_ms` | decomp-c | Get current tick in milliseconds |
| `0x08015360` | 206 | `dma_wait_done` | decomp-c | Wait for DMA completion |
| `0x08015434` | 304 | `dma_error_clear_v2` | decomp-c | DMA error flag clear (duplicate of dma_error_clear) |
| `0x0801556c` | 10 | `veneer_1556c` | deferred | Thunk/veneer to another function |
| `0x0801557c` | 10 | `veneer_1557c` | deferred | Thunk/veneer to another function |
| `0x0801558c` | 10 | `veneer_1558c` | deferred | Thunk/veneer to another function |
| `0x0801559c` | 10 | `veneer_1559c` | deferred | Thunk/veneer to another function |
| `0x080155cc` | 10 | `veneer_155cc` | deferred | Thunk/veneer to another function |
| `0x080155ec` | 10 | `veneer_155ec` | deferred | Thunk/veneer to another function |

---

## 2026-06-03 — cross-version diff vs prior release 1.14.1

The prior OEM release `batteryware_1.14.1.bin` (Nov 1 2021 build, 83 940 B) was
imported into the `vanmoof` Ghidra project and diffed against the decomp target
1.17.1 (87 568 B). Method: position-independent function-body hashing, made
fair by re-importing **both** images under `/diff/` with identical pure
auto-analysis (the curated 1.17.1 has hand-refined boundaries that don't match
fresh-auto 1.14.1, which inflates false diffs). Full write-up in
`changelog-1.14.1-to-1.17.1.md`.

Result: **1.14.1 → 1.17.1 is a broad release** (+3 628 B / +4.3 %, three minor
versions, build-date field blanked, reset vector moved ~0x1C00 from
heavy reordering) — not a surgical patch like PowerBank 1.11.01→1.11.05. Only
**93 / 226** function bodies are byte-identical (~41 %); ~35 more are
relocation-only; the rest changed/restructured. Unchanged core = libc/math, HAL
leaf drivers, fuel-gauge protection checks, MOSFET/CRC/modem helpers. Touched =
BMS state machine + state handlers/timers, fuel-gauge coulomb/scan/cell-balance,
the UART command/telemetry processor, init/HAL bring-up, CAN. Two changes
characterised at instruction level:
- `bms_set_state` history record widened 0x30→0x38 B, +3 fields
  (`fg_charge_status` + two status-flag words) + per-transition `uart_printf`.
- UART command processor (`FUN_0800afa4`) recompiled with a smaller stack frame
  (0x11c→0x84, drops r8/r9) and refactored to delegate to new helpers
  `FUN_0800ce9e`/`FUN_0800d8f0` (replacing 1.14.1's `FUN_0800f4c0`/`FUN_0800ef2c`);
  Modbus dispatch core unchanged (1534 equal instr).

**Per-function decode (`docs/compare-1.14.1/`).** The 12 changed functions in the
`fg_*` / `*_mosfet_*` / `bms_*` / `cell_balance_update` set were decompiled in both
versions and adversarially verified (README + charge.md / fuel_gauge.md / bms.md).
Highlights: `bms_init` FEDL5236 reg `0x0e` init `0x0a→0x9a` and 6 of 7 debug
`uart_printf` removed; `bms_state_machine` drops the host-enable gate (protection
dispatch now unconditional), inverts bit-4 set→clear on state 0xb/0xc entry, raises
the state-4 recharge timeout 50→300 ticks; `cell_balance_update` gains an outer gate
(`*0x200024f4 == 0x40012400` else no-op); `charge_mosfet_set` drops status-word bits
`0x20000`/`0x200`; `fg_scan`+`fg_coulomb_update` were split from one 1.14.1 function
(`FUN_08003c7c`) with the temperature-protection gate widened (status bits 9/10) and
the reg-`0x68` retry scan removed; `fg_read_loop` + the coulomb tail mirror cell index
9 to global `0x20002824`; `fg_charge_status`/`fg_status_flag_get`/`fg_status_flag2_get`
were extracted from the inline UART report cascade (`FUN_0800d004`), `fg_charge_status`
dropping its `flag&2` OR-bit1 clause.

**⚠ Secondary-fuse path (`docs/compare-1.14.1/fuse.md`).** `state_handler_17_19`
(only path that blows the pack's irreversible PB7 fuse) audited across every call
site in both images. Fuse **trigger byte-identical** (PB7 high iff `s_prot_status`
bit11==0 && (`g_fault_flags` bit6|bit7) && `s_bms_cfg` bit15==0). The handler's only
body edit (`s_bms_cfg &= ~0x200` → `~0x800`) is in the post-decision force-off tail,
reads no trigger input. **Real change: a GPIOH PH0 input interlock
(`gpio_bit_read(0x50001C00,1)==1`) was REMOVED from every per-state dispatcher**
(`state_timer_0b/0c/0d/0e/12/13/14/15`, `bms_state_machine`, `can_transmit`, the
`FUN_063e0/6810/699c/6b28/6cb4/6e40/a794/a988` family) — 1.14.1 required PH0 high to
dispatch latched protection/fault handlers; 1.17.1 dispatches on the status/fault
bits alone, so the fuse can fire in more situations. `main_loop` (state 0x17/0x18
path) was never PH0-gated in either version (unchanged).

**PH0 identified (traced all three images):** GPIOH bit0 = digital INPUT, configured
no-pull EXTI-rising by BOTH apps (bootloader leaves it floating); sibling PH1 = output
driven HIGH at boot. In 1.14.1 PH0 is polled pervasively (`gpio_bit_read(0x50001C00,1)`
in ~15-20 per-state handlers + charge-balance `FUN_0800b364`) = the BMS **system-enable
/ pack-active** gate: PH0 HIGH ⇒ run protection/balance; PH0 LOW (debounced) ⇒ sleep
(`FUN_08007730`, sets `s_bms_cfg` 0x800). In 1.17.1 **every GPIOH read is removed**
(byte-sweep `00 1C 00 50`: 22 refs→3, all non-read); the surviving per-tick gate reads
PB11 (mode button) + RAM bits. PH0 is a system-enable input, NOT fuse-status feedback.
The fuse-reachability change is a side effect of deleting this global enable gate.
**Fuse inputs unchanged:** `s_prot_status` bit11 SET only by fg_scan watchdog
(value<2000, dwell>0x31), CLEARED only by `bms_init` wipe — byte-identical both versions;
`g_fault_flags` bit6/7 set by byte-identical `fg_*_oc_check`; `s_bms_cfg` bit15 same.
The handler force-off tail clears `s_bms_cfg` bit9 (1.14, via `FUN_0800ae38`, a
consumed action latch) → bit11 (1.17, the charge-permit flag) — post-decision, not a
trigger input. Physical net for PH0 / whether PB11 replaced it = unprovable from
firmware (no schematic). Full: `docs/compare-1.14.1/fuse.md`.

**2026-06-04 — fuse fault scope clarified (which faults arm PB7).** `g_fault_flags`
(`0x20002c44`) is the unified protection word; the six byte-identical `fg_*_check`
functions each OR one bit into it: UVP1=0, UVP2=1, **OVP1=2, OVP2=3**, threshold=4,
**dischg-OC=6, chg-OC=7**. The fuse handler reads **only bits 6/7**, so **only
over-current arms the heater — over-voltage and under-voltage do NOT**. OVP routes to
`state_handler_14` (bit2) / `15` (bit3) and (via `s_prot_status`) `09`/`0a`;
threshold→`16`; all of these + the UVP handlers do `charge_mosfet_off()` +
`bms_configure()` + a recoverable state and write only PB0(`0x1`)/PB9(`0x200`),
**never PB7**. `bms_state_machine`'s only `orrs` into `g_fault_flags` set bits 0/1/8/9
— it never sets 6/7, so the OC checks are the sole setters. OVP reaches the fuse only
*indirectly*: a charge FET that fails shorted keeps current flowing → charge-OC (bit7)
→ heater. Design intent = the pyro fuse is the FET-welded/can't-interrupt-current
backstop, sensed as persistent over-current, not a direct over-voltage trip. (The
FEDL5236 AFE's own autonomous OVP/FET protection is a separate hardware layer not
visible here.) Both versions identical. Added to `docs/compare-1.14.1/fuse.md`
§"What arms the fuse".

**2026-06-04 — hardware OVP→fuse path CONFIRMED (board level).** The open "is there a
hardware over-voltage→fuse path?" item is resolved: the pack carries a dedicated
**secondary over-voltage protection** stage — two **S-8215AAD-K8T2U** cell-overcharge
ICs (`U1005` = cells 1–5, `U1006` = cells 6–10; ABLIC, datasheet `reference/S8215A_E.pdf`)
that, on **any cell > 4.35 V for 2 s** (−AAD: 4.350 V detect, −0.250 V hyst, 2.0 s,
CMOS active-H CO), drive the **same** SCF9550 pyro fuse heater autonomously — no MCU.
Fuse-blow circuit: heater `R1074`(40 Ω) sunk by power NMOS `Q1016` (BSZ340N08NS3),
gated by PMOS `Q1015` (BSS84) whose gate (node **JN2**, idle-pulled to +BATT by `R1071`
100 k) is pulled low by EITHER (A) MCU `PB7 → Q1017 (2N7002) → R1076 → JN2` on
over-current, OR (B) each S-8215A `CO → 2N7002 (Q1020/Q1021) → 330 k/200 k → JN2` on
over-voltage. So **OV does blow the fuse — in hardware**, while the MCU only blows it on
persistent OC. Documented in `docs/hardware.md` §"Secondary protection & the pyro fuse"
(new) + PB7 row updated to confirmed; `fuse.md` hardware-layer caveat resolved.

**2026-06-04 — can firmware detect a fuse blow? NO (for the hardware/OVP path).**
No fuse-blown sense input to the MCU exists: no dedicated GPIO (inputs are only PB11
mode-button, PA10 service-UART, PC13/PB0 buttons, PC12 AFE-ready, the removed GPIOH
PH0), and no cell-vs-terminal comparison. The AFE exposes PSNS (`reg 0x34`) as a
pack-*terminal*/charger-voltage proxy, but firmware uses it only for charger-present +
the shipping power-down voltage check — not an open-fuse test. The only "fuse blown"
knowledge firmware has is **self-recorded**: when *it* blows the fuse on over-current it
persists state `0x17`/`0x18` ("MOS Failure Mode") to EEPROM `0x08080001`, reported next
boot. A **secondary-OVP (S-8215AAD/JN2) blow is invisible to firmware** — it sees the
cell over-voltage that triggered it (software OVP on cell readings) and reacts as a
recoverable OVP; once cells relax it "recovers" and reports healthy cell telemetry while
the pack terminal is permanently dead. (`bms_configure`'s `gpio_bit_read(GPIOC,0x2000)`
= PC13 power button, not a fuse sense; the `0x08009eea` region writes `g_boot_mode` from
a `'0'/'1'` set-mode path, not a sensor.)

**2026-06-04 — NEW DOC `docs/protection-config.md`** maps `cfg_blk` (`0x200028D0`, the
RAM protection-threshold block, distinct from `bms_ctx 0x200029A8`) with every default
from `config_init` (`FUN_08007368`): 5 cell-voltage window comparators (`g_fault_flags`
bits 0-4, byte thresholds 85/40/110/20/135 with trip/recover hysteresis, trip delay
3000/100=30 samples, recover 1500/100=15), 2 over-current comparators (bits 6/7,
threshold 500, delay 2000/100=20, direction-gated by `mode_flag` 0x20002870), the boot
OVP/UVP detect set (`+0x2a…0x46`, u16 mV 4250/4150/4300/3300/2800), and AFE-side
SC(150 mV)/WDT(2 s)/OV(off). Byte→mV and unit→A scales flagged as follow-up (set inside
`cell_balance_update`; cross-check the Modbus register map). cfg_blk byte readings
`0x20002589/8A` confirmed written by `cell_balance_update` (cell-voltage-derived, not
temperature).

**2026-06-04 — Ghidra readability pass (1.17.1 program).** Mapped two new memory blocks
so RAM/MMIO globals are first-class symbols instead of `DAT_` pool pointers: **`SRAM`
`0x20000000..0x20004FFF`** and **`GPIO` `0x50000000..0x50001FFF`** (both volatile, so RAM
reads aren't constant-folded). Labeled **36 globals** at their real addresses
(`g_fault_flags 0x20002c44`, `s_prot_status 0x2000286c`, `s_bms_cfg 0x20002c00`,
`cfg_blk 0x200028d0`, `bms_ctx 0x200029a8`, `s_state 0x20002b58`, `mode_flag 0x20002870`,
`cell_status 0x20002588`, `g_boot_mode 0x20002c48`, the 7 protection dwell counters,
`GPIOA/B/C/H`, …). To make the **decompiler** print the names it isn't enough to label
the target — the literal-pool slot that loads the address must be pointer-typed
(`apply_data_type "void *"`); then it renders `*(T *)PTR_<global>_<slot>`. Done for
**`bms_state_machine`** and **`state_handler_17_19`** (now fully legible). NOTE for the
next contributor: this is the [[CLAUDE.md]] "name every address" rule; full-image
propagation (~260 more pool slots) is best done by re-running Ghidra auto-analysis (it
pointer-types LDR-loaded addresses now that the blocks exist) or an inline script — both
need `GHIDRA_MCP_ALLOW_SCRIPTS=1` / an analysis pass that wasn't run here to avoid
churning the curated program. `bms_set_state` readability is gated on defining a
`bms_ctx`/`cfg_blk` **struct** (it's mostly `bms_ctx+offset` field stores) — flagged
follow-up. Program saved; re-dump the program JSON when convenient.
