# Decompilation progress

Per-function tracker. Source of truth for "what's left to do."

> Companion docs: **`hardware.md`** for the GPIO pin map and the
> SRAM-globals table the OEM uses; **`protocol.md`** for the wire
> framing, command codes, and RX/TX paths.

Populate the **Functions** table by:

1. In Ghidra, run *Script Manager → VanMoof → DumpShifterProgram*. This
   writes `ghidra/exports/shifter_program.json`.
2. Append a row per function from the JSON, sorted by address.

Status legend:
- **pending** — not started
- **in-progress** — claimed by a contributor (note who in Notes)
- **named** — renamed in Ghidra, prototype set, but no C yet
- **decomp-c** — translated to C, builds, but bytes diverge from OEM
- **byte-eq** — translated to C and `make compare` reports zero diff
- **deferred** — intentionally skipped (e.g. unreachable, encrypted blob)

_Last refresh from `ghidra/exports/shifter_program.json`: 2026-05-18
(image base `0x08003000`, **131 functions**, 0 strings)._

## Summary

| Status | Count | % of bytes |
| --- | --- | --- |
| pending      |  34 | ~28% |
| in-progress  |   0 |   0 |
| named        |   1 | <1% |
| decomp-c     |  83 | ~62% |
| byte-eq      |   0 |   0 |
| deferred     |  13 | ~10% |

_Total functions: 130 (the `USART1_IRQHandler` at `0x0800450C` was
created in an earlier round; `main` at `0x080042D6` was created when
the call site `bl modbus_rx_poll` at 0x08004366 led us to its entry —
the byte immediately after `FUN_0800428E`'s `pop {r4,pc}` epilogue)._

Decomp'd so far:
- **crc** (`src/crc.c`): `crc_reset` (8 B), `crc32_word` (24 B), `crc32_words` (32 B)
- **util** (`src/util.c`): `memcpy` (36 B, non-standard `void` return)
- **image** (`src/image.c`): `image_verify_crc` (100 B), `image_apply` (92 B), `report_image_status` (64 B), `cmd_5c_write3` (24 B, OEM @ 0x08003B86 — short-form cmd 0x5C: splice 3 register bytes into status-emit slots and fire `report_image_status`), `emit_image_status_payload` (38 B, OEM @ 0x08003B9E — cmd_5c_write3 variant: caller supplies the two pkt bytes; the version byte is derived inline from `(RX[5]&0x7F)<<1`. Called from `cmd_5b_selftest`.)
- **flash_store** (`src/flash_store.c`): `flash_unlock` (12 B), `flash_lock` (14 B), `flash_clear_status` (6 B), `flash_erase_page` (32 B), `flash_erase_pages` (32 B), `flash_get_status` (54 B), `flash_busy_step` (26 B), `flash_wait_status` (44 B), `flash_do_page_erase` (72 B), `flash_program_halfword` (66 B, OEM @ 0x080049B2 — replaced the prior speculative shim; returns FLASH_ST_* status), `settings_set_halfword` (110 B, OEM @ 0x08003178 — RMW one halfword in `FLASH_SETTINGS_PAGE` at `0x08007800`; full 8-halfword read-erase-write per call), `flash_settings_commit` (118 B, OEM @ 0x080031E6 — persist G_STATE_FC + G_COUNTER (BE32) + G_5C_REGS[0..2] across 8 halfwords; clears G_5C_BUSY; called by cmd 0x5C long-form and by main's idle-reset/deferred-commit paths).
- **modbus** (`src/modbus.c`): `modbus_crc16_compute` (64 B), `modbus_send_bytes` (28 B), `modbus_tx_finalize` (54 B), `modbus_tick` (20 B), `modbus_reply_passthrough` (70 B)
- **modbus_dispatch** (`src/modbus_dispatch.c`): `modbus_dispatch_pdu` (278 B) — switch over cmd byte (0x0F / 0x14 / 0x5A / 0x5B / 0x5C / 0x81 / 0x82 / 0x95) with 1 case-helper stub awaiting its own decomp (cmd_5c_write3 lives in image.c; cmd_5b_selftest landed; cmd 0x5C long-form now calls `flash_settings_commit` in flash_store.c); `modbus_rx_poll` (366 B) — FSM that accepts 8-byte short / 45-byte long frames from the IRQ scratch at 0x200001B2, CRC-validates, and hands off to the dispatcher; `cmd_0f_report_u32` (50 B, OEM @ 0x08003C68 — case 0x0F: stage `(RX[5]&0x7F)<<1` and a BE32 value into the cmd-0x0F report slots) + helper `emit_counter_status_pdu` (76 B, OEM @ 0x08003C1C — emits the 9-byte response PDU); `cmd_5b_selftest` (88 B, OEM @ 0x08003BC4 — case 0x5B: encode {PA0,PA1} state as one of {0,0x32,0x64,0x96} and forward via `emit_image_status_payload`).
- **hal** (`src/hal.c`, new): `nvic_configure` (106 B, OEM @ 0x08004E74 — moved from `uart.c`), `nvic_set_priority` (110 B, OEM @ 0x080030F0 — CMSIS-style; system handlers via SCB->SHP for negative IRQs, NVIC->IPR for positive), `rcc_ahben_bits` (28 B, OEM @ 0x080051A8 — moved), `rcc_apb2en_bits` (28 B, OEM @ 0x080051C4 — moved), `rcc_apb1en_bits` (28 B, OEM @ 0x080051E0 — new), `rcc_apb1_reset_bits` (28 B, OEM @ 0x08005218 — APB1RSTR RMW), `rcc_apb2_reset_bits` (28 B, OEM @ 0x080051FC — APB2RSTR RMW), `rcc_reset_usart` (54 B, OEM @ 0x08005B9C — pulses APB1RSTR.USART2RST or APB2RSTR.USART1RST per base address; promoted from `named`), `rcc_get_clocks_freq` (160 B, OEM @ 0x08005108 — decodes RCC->CFGR.SWS → SYSCLK then derives HCLK/PCLK1/PCLK2 via APBAHBPrescTable; constants resolved from literal pool at 0x080052A8..0x080052D0).
- **runtime** (`src/runtime.c`, new): `__aeabi_uidiv` (44 B, OEM @ 0x08005D40 — Cortex-M0 unsigned 32-bit softdiv; GCC auto-emits calls for every `/` and `%` on `uint32_t`).
- **timer** (`src/timer.c`, augmented): five OEM TIM HAL helpers + the high-level wrapper — `tim_time_base_config_init` (18 B, OEM @ 0x080053E4), `tim_time_base_init` (74 B, OEM @ 0x0800539A — base-address-aware: special-cases TIM1/2/3 vs TIM1/15/16), `tim_clear_flag` (8 B, OEM @ 0x080059A0), `tim_dier_bits` (26 B, OEM @ 0x0800596E), `tim_enable` (26 B, OEM @ 0x08005498), `tim2_init_periodic` (96 B, OEM @ 0x08004048 — boot-only wrapper, called once with `(HCLK/100000 - 1, 99)` for a 1 kHz TIM2 update IRQ). The pre-existing speculative `systick_*` and `tim3_*` code stays in this file as gc-sectioned scaffolding.
- **uart** (`src/uart.c`): `USART1_IRQHandler` **rewritten** to match OEM exactly — appends bytes to `0x200001B2[*0x200000E4++]`, resets the end-of-frame wait counter at `0x200000DC`, caps at 45 bytes. The earlier speculative `s_rx_buf` ring + `uart1_rx_*` API is no longer on the OEM RX path; it stays as scaffolding for the (still-speculative) `protocol.c` and is gc-sections'd away from the final image. 🔎 **2026-05-18**: `usart_init` BRR computation now calls `rcc_get_clocks_freq` (hal.c) instead of hard-coding `USART_PCLK_HZ = 48000000u` — picks `clocks.pclk2` for USART1 (APB2) and `clocks.pclk1` for USART2 (APB1), matching the OEM byte sequence.
- **main** (`src/main.c`, replaces speculative version saved as `main.c.bak`): `main` (486 B, OEM @ 0x080042D6), `sched_pick_task` (74 B, OEM @ 0x080035BE — `min(G_STATE_FC, 6)`), `motor_drive_step` (74 B, OEM @ 0x080036D4 — H-bridge per `G_5A_TARGET`, brake+latch on motion-reached), `state_flags_reset` (26 B, OEM @ 0x0800315E — clears per-task flags at tail), `motor_h_bridge_set` (210 B, OEM @ 0x080032FA — PA9/PA10 drive table + stall timeout fallback that synthesizes `G_MOTION_REACHED`), `pos_encoder_tick` (86 B, OEM @ 0x080032A4 — advances `G_STATE_115` on PA0 edges in the direction set by `G_DRIVE_DIR`, and resets the stall-timer latch each counted edge), `drive_dir_code` (28 B, OEM @ 0x08003288 — tri-state decode of the H-bridge mask byte), `pa0_changed` (22 B, OEM @ 0x08003272 — PA0 vs `G_STATE_13B` edge detect), `sched_idle_reset` (26 B, OEM @ 0x080036BA — case-0 handler: promote `G_STATE_FC` out of 0, reset cmd-0x14 counter, latch gear-position back to home, drain pending 5C work), `sched_task_beta` (196 B, OEM @ 0x080033E2 — PA1-gated shifter task: on PA1↑ drive forward, after `G_MOTION_REACHED` schedule next state per gear (4→task 4, 2/3→task 5, 1→task 2/state 3); brake, bump `G_SHIFT_ATTEMPT_CTR`, demote to state 6 after 3 retries; on PA1↓ latch gear←1, clear retry, raise `G_FLAG_117`+`G_5C_BUSY`, arm state 2), `sched_task_alpha` (178 B, OEM @ 0x08003608 — round-robin "alpha" body: state 1 arms cmd-0x14 follow-up (`G_FLAG_117/13D=1, 13E=0`); state 2 re-energises the H-bridge per `G_DRIVE_DIR` (0x0F/0xF0), on `G_MOTION_REACHED` brakes and classifies by PA1 (low→task 6 + home gear, high→task 3), bumps `G_SHIFT_ATTEMPT_CTR`, demotes to state 6 after 5 retries; on position-match (`G_FLAG_116 == G_STATE_115`) hands off to a still-stubbed helper `sched_alpha_match_3538(gear, drive_dir)` @ 0x08003538), `syscfg_set_mem_mode` (24 B, OEM @ 0x080052E8 — RMW low 2 bits of `SYSCFG_CFGR1`; called once from main's boot prologue with value 3), `boot_init_systick` (72 B, OEM @ 0x0800428E — CMSIS-style `SysTick_Config(HCLK/1000)` for 1 kHz tick, trap-on-fail if `HCLK/1000 > 1<<24`; brackets the LOAD/VAL/CTRL writes with `nvic_set_priority(SysTick_IRQn, 3)` then `(SysTick_IRQn, 0)`), `boot_init_gpio` (88 B, OEM @ 0x080041C6 — enables GPIOA+GPIOB clocks, configures PA9/PA10 as output 50 MHz push-pull and parks them low via BRR, configures PA0/PA1 as input floating), `sched_task_extra` (60 B, OEM @ 0x080034A6 — 3-way gear compare → set `G_DRIVE_DIR` to 0x0F/0xF0/0 + raise `G_FLAG_13E` when driving), `sched_alpha_match_3538` (134 B, OEM @ 0x08003538 — per-(gear, direction) settling-time window after position match; brakes + clears state when the window elapses; introduces new SRAM globals `G_SETTLE_ARMED` @ 0x20000138 and `G_SETTLE_TICK_BASE` @ 0x20000134). 🎉 **main.c is now trap-stub-free.** Every helper called from `main` or its sched-task chain is a real decomp; the OEM boot + super-loop is fully translated. Last two retired this round:
- `sched_task_extra` @ 0x080034A6 (60 B) — 3-way compare of `G_FLAG_116` (target gear) vs `G_STATE_115` (current gear), sets `G_DRIVE_DIR` to 0x0F / 0xF0 / 0 + raises `G_FLAG_13E` when driving.
- `sched_alpha_match_3538` @ 0x08003538 (134 B) — per-gear settling-time window after position match (thresholds 23/30/35-or-25/40 ms at 1 kHz tick), brakes + clears state when elapsed.
- **gpio** (`src/gpio.c`): six OEM-confirmed helpers — `gpio_idr_test` (20 B, OEM @ 0x08004DBC), `input_pa0` (22 B, OEM @ 0x0800325C), `input_pa1` (22 B, OEM @ 0x080033CC), `gpio_bsrr_write` (4 B, OEM @ 0x08004DF4 — raw BSRR set), `gpio_brr_write` (4 B, OEM @ 0x08004DF8 — raw BRR clear), `gpio_pin_configure` (222 B, OEM @ 0x08004CCE — moved here from `uart.c` so `boot_init_gpio` can also use it; CRL/CRH 4-bit MODE+CNF + optional initial-level BSRR/BRR). All raw-byte-offset, bypassing the still-broken speculative `gpio_t` struct (plan-2).
- **uart** (`src/uart.c`): `uart1_send_byte` (28 B), `uart1_init` (150 B), `USART1_IRQHandler` (62 B); file-static helpers: `usart_write_data`, `usart_test_flag`, `usart_check_status`, `usart_read_data`, `usart_clear_flag`, `usart_init`, `usart_ier_bits`, `usart_set_enable`, `nvic_configure`, `rcc_apb2en_bits`, `rcc_ahben_bits`, `gpio_set_af`, `gpio_pin_configure`. Real wiring is **PB6/PB7 AF0**, not PA9/PA10 as the speculative original assumed.

Named (in Ghidra) but no C yet:
- `__gnu_thumb1_case_uqi` (`0x08005DB4`, 28 B) — libgcc compiler-runtime jump-table dispatcher for Thumb-1 unsigned-byte switches. Used by `modbus_dispatch_pdu`. Not project code; no C to write.

Deferred (renamed in Ghidra with `unused_` prefix; MM32 stdlib leftovers
the linker didn't GC — zero callers anywhere in the image, so no C
source was written):
- `unused_rcc_get_flag_status` (`0x08004FAA`, 48 B), `unused_rcc_wait_for_hse_startup` (`0x08004FDA`, 60 B), `unused_flash_erase_option_bytes` (`0x080047FA`, 70 B), `unused_flash_enable_readout_protection` (`0x08004840`, 150 B), `unused_flash_erase_option_bytes_v2` (`0x080048D6`, 120 B), `unused_flash_program_option_word` (`0x0800494E`, 100 B), `unused_flash_program_option_halfword` (`0x080049F2`, 74 B), `unused_flash_program_option_byte` (`0x08004A3C`, 100 B), `unused_flash_program_write_protection` (`0x08004AA0`, 182 B), `unused_flash_user_option_byte_config` (`0x08004B72`, 86 B), `unused_gpio_deinit` (`0x08004C64`, 106 B), `unused_rcc_reset_periph_by_base` (`0x08005364`, 54 B), `unused_gpio_pin_af_config` (`0x0800561A`, 86 B). Total **13** functions, **~1236 B** of stdlib bytes that the OEM image carries but never executes.

> 🛠 **Plan-1 complete (USART):** `usart_t` in `include/mm32f031.h`
> rewritten to the full MM32 layout (`TDR / RDR / SR / ISR / IER / ICR
> / CCR / FCR / BRR_INT / BRR_FRA`; F0-style original in `mm32f031.h.bak`).
> USART1 wired to **PB6/PB7 AF0**. `uart1_init` + `USART1_IRQHandler` +
> `uart1_send_byte` + their HAL chain (USART + NVIC + RCC + GPIO helpers,
> 14 file-static functions in `uart.c`) all decomp-c.
>
> ⚠️ `gpio_t` in `mm32f031.h` is still the speculative STM32F0 layout
> (separate MODER/OTYPER/OSPEEDR/PUPDR @ 2 bits/pin). The OEM image
> uses STM32F1-style GPIO (CRL/CRH @ 0x00/0x04, 4 bits/pin). Our new
> `gpio_set_af` and `gpio_pin_configure` helpers in `uart.c` access
> registers by raw offset to sidestep the wrong struct. Fixing
> `gpio_t` cleanly is "plan-2" and will cascade into `sensor.c`,
> `motor.c`, `gpio.c` etc. — out of scope here.

Update this section after each commit that changes a function's status.

## Important caveat — shifterware's vector table is decorative

Cortex-M0 base has no VTOR. The CPU's live interrupt vector table is
the one at `0x08000000` (shifterboot's), not shifterware's at
`0x08003028`. Shifterboot hands off by directly branching to
`Reset_Handler` at `0x08005E79`. Slots 2..47 of shifterware's vector
table therefore **do not designate ISRs** at runtime — they're inert
data the linker emitted. Whatever functions those slots point at are
just regular helpers; their semantics must be discovered by reading
the code, not assumed from the vector slot.

Two concrete examples already worked out:

- Slot 15 (would-be `SysTick_Handler`) points at `0x08005CF0` — this is
  actually `crc32_word(uint word)`, a CRC peripheral helper.
- Slot 31 (would-be `TIM2_IRQHandler`) points at `0x08005CBC`, a 2-byte
  thunk to `FUN_08005CC0` (also 2 bytes — probably a `b .` stub).

The only OEM-confirmed name we keep from the vector table is
`Reset_Handler` (slot 1, `0x08005E78`).

The dump captures all 48 CM0 vectors; slots 45/46/47 hold executable
bytes (not vector entries) because the OEM didn't use those slots and
the linker laid down code starting at `0x080030DC`.

## Vector table

Cortex-M0 numbering (#0 = initial SP, #1 = Reset, #15 = SysTick, #16+N =
IRQ N). Address of each entry = `0x08003028 + 4·N`. `Default_Handler` =
any target in `0x08005E78–0x08005EB1`.

| # | Cortex-M0 vector | OEM target | C handler | Status |
| --- | --- | --- | --- | --- |
| 0 | `Initial SP` | `0x20000400` | n/a | n/a |
| 1 | `Reset_Handler` | `0x08005E79` | `Reset_Handler` (startup_mm32f031.S) | pending |
| 2 | `NMI_Handler` | `0x08005E9F` | `Default_Handler` | default-handler |
| 3 | `HardFault_Handler` | `0x08005EA1` | `Default_Handler` | default-handler |
| 4 | _(reserved)_ | `0x08005EA3` | `Default_Handler` | default-handler |
| 5 | _(reserved)_ | `0x08005EA5` | `Default_Handler` | default-handler |
| 6 | _(reserved)_ | `0x08005EA7` | `Default_Handler` | default-handler |
| 7 | _(reserved)_ | `0x00000000` | n/a | unused |
| 8 | _(reserved)_ | `0x00000000` | n/a | unused |
| 9 | _(reserved)_ | `0x00000000` | n/a | unused |
| 10 | _(reserved)_ | `0x00000000` | n/a | unused |
| 11 | `SVC_Handler` | `0x08005EA9` | `Default_Handler` | default-handler |
| 12 | _(reserved)_ | `0x08005EAB` | `Default_Handler` | default-handler |
| 13 | _(reserved)_ | `0x00000000` | n/a | unused |
| 14 | `PendSV_Handler` | `0x08005EAD` | `Default_Handler` | default-handler |
| 15 | `SysTick_Handler` | `0x08005CF1` | `SysTick_Handler` | **pending** |
| 16 | `WWDG_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 17 | `PVD_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 18 | `RTC_IRQHandler` | `0x00000000` | n/a | unused |
| 19 | `FLASH_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 20 | `RCC_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 21 | `EXTI0_1_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 22 | `EXTI2_3_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 23 | `EXTI4_15_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 24 | _(reserved)_ | `0x00000000` | n/a | unused |
| 25 | `DMA1_Ch1_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 26 | `DMA1_Ch2_3_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 27 | `DMA1_Ch4_5_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 28 | `ADC_COMP_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 29 | `TIM1_BRK_UP_TRG_COM_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 30 | `TIM1_CC_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 31 | `TIM2_IRQHandler` | `0x08005CBD` | `TIM2_IRQHandler` | **pending** |
| 32 | `TIM3_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 33 | `TIM6_IRQHandler` | `0x00000000` | n/a | unused |
| 34 | _(reserved)_ | `0x00000000` | n/a | unused |
| 35 | `TIM14_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 36 | `TIM15_IRQHandler` | `0x00000000` | n/a | unused |
| 37 | `TIM16_IRQHandler` | `0x08005EB1` | `Default_Handler` | default-handler |
| 38 | `TIM17_IRQHandler` | _dump truncated_ | _pending_ | pending |
| 39 | `I2C1_IRQHandler` | _dump truncated_ | _pending_ | pending |
| 40 | `I2C2_IRQHandler` | _dump truncated_ | _pending_ | pending |
| 41 | `SPI1_IRQHandler` | _dump truncated_ | _pending_ | pending |
| 42 | `SPI2_IRQHandler` | _dump truncated_ | _pending_ | pending |
| 43 | `USART1_IRQHandler` | _dump truncated_ | _pending_ | pending |
| 44 | `USART2_IRQHandler` | _dump truncated_ | _pending_ | pending |
| 45 | _(reserved)_ | _dump truncated_ | n/a | pending |
| 46 | _(reserved)_ | _dump truncated_ | n/a | pending |
| 47 | _(reserved)_ | _dump truncated_ | n/a | pending |

Real (non-default) ISRs to attack: **`SysTick_Handler`** (`0x08005CF0`)
and **`TIM2_IRQHandler`** (`0x08005CBC`), plus whatever non-default
targets land in vectors 38–47 once the dump is fixed.

## Functions

| Address | Size | Name | Module | Status | Notes / Commit |
| --- | --- | --- | --- | --- | --- |
| `0x080030e4` |    4 | `FUN_080030e4` |  | pending |  |
| `0x080030f0` |  110 | `nvic_set_priority` | hal | decomp-c | CMSIS-style: negative irq → SCB->SHP, non-neg → NVIC->IPR; top 2 bits of priority byte honoured |
| `0x0800315e` |   26 | `state_flags_reset` | main | decomp-c | clears 6 shared task-flag bytes; called by 5 different state-tasks at their tails |
| `0x08003178` |  110 | `settings_set_halfword` | flash_store | decomp-c | RMW one halfword in settings page; full 8-HW read-erase-write per call |
| `0x080031e6` |  118 | `flash_settings_commit` | flash_store | decomp-c | persist G_STATE_FC + G_COUNTER (BE32) + G_5C_REGS[0..2] to `0x08007800`; clears G_5C_BUSY |
| `0x0800325c` |   22 | `input_pa0` | gpio | decomp-c | reads GPIOA IDR bit 0 |
| `0x08003272` |   22 | `pa0_changed` | main | decomp-c | PA0 vs `G_STATE_13B` mirror; edge predicate for `pos_encoder_tick` |
| `0x08003288` |   28 | `drive_dir_code` | main | decomp-c | decode `G_DRIVE_DIR` byte (`0xF0`/`0x0F`/else) to 0/1/2 |
| `0x080032a4` |   86 | `pos_encoder_tick` | main | decomp-c | bump `G_STATE_115` ±1 on PA0 edge per `G_DRIVE_DIR`; resets stall latch |
| `0x080032fa` |  210 | `motor_h_bridge_set` | main | decomp-c | PA9/PA10 H-bridge driver with stall-timeout fallback |
| `0x080033cc` |   22 | `input_pa1` | gpio | decomp-c | reads GPIOA IDR bit 1 |
| `0x080033e2` |  196 | `sched_task_beta` | main | decomp-c | PA1-gated shifter task; gear-driven state scheduling + retry counter + home-reset/commit branch |
| `0x080034a6` |   60 | `sched_task_extra` | main | decomp-c | 3-way compare G_STATE_115 vs G_FLAG_116; set G_DRIVE_DIR 0x0F/0xF0/0 + G_FLAG_13E |
| `0x08003538` |  134 | `sched_alpha_match_3538` | main | decomp-c | per-(gear,direction) settling-time window; thresholds 23/30/35-or-25/40 ms; brakes + clears state when elapsed |
| `0x080035be` |   74 | `sched_pick_task` | main | decomp-c | `return min(G_STATE_FC, 6)`; clamped state byte for round-robin |
| `0x08003608` |  178 | `sched_task_alpha` | main | decomp-c | state 1: arms cmd-0x14 follow-up; state 2: drives H-bridge per gear delta, on motion-reached brakes + bumps retry counter (≥5 → state 6), on position-match hands off to `sched_alpha_match_3538` @ 0x08003538 |
| `0x080036ba` |   26 | `sched_idle_reset` | main | decomp-c | case-0 epilogue: state machine "home" reset; only called by main's `sched_run_task` |
| `0x080036d4` |   74 | `motor_drive_step` | main | decomp-c | per-iteration motor servo: drive direction → poll motion-reached → brake/latch/report |
| `0x0800371e` |   28 | `uart1_send_byte` | uart | decomp-c | write byte then poll SR bit 0 for TX-complete |
| `0x0800373a` |   28 | `modbus_send_bytes` | modbus | decomp-c | loops `uart1_send_byte` |
| `0x08003756` |   54 | `modbus_tx_finalize` | modbus | decomp-c | send buf; if len==7 && img_ok, SYSRESETREQ |
| `0x0800378c` |   64 | `modbus_crc16_compute` | modbus | decomp-c | poly 0xA001, init 0xFFFF; stores lo/hi at `0x200000E7`/`E8` |
| `0x080037cc` |   70 | `modbus_reply_passthrough` | modbus | decomp-c | echo RX[0..5] + CRC over UART1, len=8 |
| `0x08003812` |   32 | `flash_erase_page` | flash_store | decomp-c | unlock + clear-flags + inner-erase + clear-EOP + lock |
| `0x08003852` |   70 | `FUN_08003852` |  | pending |  |
| `0x08003898` |   54 | `FUN_08003898` |  | pending |  |
| `0x080038ce` |  100 | `FUN_080038ce` |  | pending |  |
| `0x0800399e` |   72 | `FUN_0800399e` |  | pending |  |
| `0x080039e6` |  160 | `FUN_080039e6` |  | pending |  |
| `0x08003a86` |   64 | `report_image_status` | image | decomp-c | builds 7-byte Modbus PDU, CRC, send+maybe-reset |
| `0x08003832` |   32 | `flash_erase_pages` | flash_store | decomp-c | loops `flash_erase_page(addr+i*0x400)` |
| `0x08003ac6` |  100 | `image_verify_crc` | image | decomp-c | manifest at flash `0x08001800` |
| `0x08003b2a` |   92 | `image_apply` | image | decomp-c | validate+report+latch-or-erase; depends on weak stubs |
| `0x08003b86` |   24 | `cmd_5c_write3` | image | decomp-c | short-form cmd 0x5C: splice version + 2 packet bytes into emit slots, fire `report_image_status` |
| `0x08003b9e` |   38 | `emit_image_status_payload` | image | decomp-c | cmd_5c_write3 variant: derives version byte from `(RX[5]&0x7F)<<1`, caller supplies the two pkt bytes |
| `0x08003bc4` |   88 | `cmd_5b_selftest` | modbus_dispatch | decomp-c | case 0x5B: encode {PA0,PA1} as {0,0x32,0x64,0x96}, report via `emit_image_status_payload(0, code)` |
| `0x08003c1c` |   76 | `emit_counter_status_pdu` | modbus_dispatch | decomp-c | builds 9-byte cmd 0x0F response: echo RX[0..1], sub-id, BE32 value, CRC |
| `0x08003c68` |   50 | `cmd_0f_report_u32` | modbus_dispatch | decomp-c | case 0x0F: stage `(RX[5]&0x7F)<<1` + BE32 `value`, then emit |
| `0x08003c9a` |  278 | `modbus_dispatch_pdu` | modbus_dispatch | decomp-c | switch on cmd byte; 3 case helpers stubbed pending decomp |
| `0x08003eda` |  366 | `modbus_rx_poll` | modbus_dispatch | decomp-c | RX FSM: assembles short/long frames, CRC, hands off to dispatcher |
| `0x08004048` |   96 | `tim2_init_periodic` | timer | decomp-c | RCC TIM2EN on, build cfg struct, `tim_time_base_init(TIM2,&cfg)`, NVIC{irq=15,prio=1,en=1}, clear UIF, set UIE, enable CEN; called from `main`'s boot prologue |
| `0x080040a8` |   10 | `settings_read_halfword` | flash_store | decomp-c | `*(uint16_t*)(FLASH_SETTINGS_PAGE + offset)`; consumer of base ptr at 0x08004254 = 0x08007800 |
| `0x080040b2` |  126 | `settings_load` | flash_store | decomp-c | boot-time load of persisted settings: G_STATE_FC + G_COUNTER (BE32 across 4 halfwords) + G_5C_REGS[0..2]; halfword 0 == 0xFFFF means blank flash → all zeroes |
| `0x08004130` |  150 | `uart1_init` | uart | decomp-c | RCC + NVIC + USART config; GPIO pin-mode part TODO |
| `0x0800450c` |   62 | `USART1_IRQHandler` | uart | decomp-c | RX byte → ring buffer; created in this round |
| `0x080041c6` |   88 | `boot_init_gpio` | main | decomp-c | RCC AHBENR.IOPAEN+IOPBEN, configure PA9/PA10 output 50MHz PP + park low, configure PA0/PA1 input floating |
| `0x0800428e` |   72 | `boot_init_systick` | main | decomp-c | `SysTick_Config(HCLK/1000)`; trap-loop on overflow; sets SysTick priority to 3 during config then 0 |
| `0x080042d6` |  486 | `main` | main | decomp-c | boot + super-loop; 14 helper trap-stubs noted at OEM addresses. Ghidra bounds restored to 486 B (0x080042D6..0x080044BB) on 2026-05-18 — see 🔎 note above the table. |
| `0x080044bc` |   32 | `FUN_080044bc` |  | pending | created 2026-05-18 (see 🔎 note); orphan helper, zero static xrefs; increments G_COUNTER + sibling counter at `0x200000D4`, calls `FUN_080059ce(0x40000000)`; likely OEM dead code |
| `0x080044dc` |   20 | `modbus_tick` | modbus | decomp-c | decrement-if-nonzero on inter-byte timeout @ `0x200000C4` |
| `0x080045b8` |  106 | `FUN_080045b8` |  | pending |  |
| `0x08004622` |    8 | `FUN_08004622` |  | pending |  |
| `0x0800462a` |   66 | `FUN_0800462a` |  | pending |  |
| `0x0800471c` |   12 | `flash_unlock` | flash_store | decomp-c | KEY1, KEY2 → FLASH->KEYR; void |
| `0x08004728` |   14 | `flash_lock` | flash_store | decomp-c | sets LOCK bit in FLASH->CR |
| `0x08004736` |   54 | `flash_get_status` | flash_store | decomp-c | reads FLASH->SR; 1/2/3/4 codes |
| `0x0800476c` |   26 | `flash_busy_step` | flash_store | decomp-c | 255-count volatile delay |
| `0x08004786` |   44 | `flash_wait_status` | flash_store | decomp-c | poll until !busy with timeout |
| `0x080047b2` |   72 | `flash_do_page_erase` | flash_store | decomp-c | wait→PER→AR→STRT→wait→clear PER (mask `0x1FFD`) |
| `0x080047fa` |   70 | `unused_flash_erase_option_bytes` | (stdlib) | deferred | dead — MM32 stdlib leftover, zero callers |
| `0x08004840` |  150 | `unused_flash_enable_readout_protection` | (stdlib) | deferred | dead — RDP unlock pattern (0xA5), zero callers |
| `0x080048d6` |  120 | `unused_flash_erase_option_bytes_v2` | (stdlib) | deferred | dead — second variant; zero callers |
| `0x0800494e` |  100 | `unused_flash_program_option_word` | (stdlib) | deferred | dead — zero callers |
| `0x080049b2` |   66 | `flash_program_halfword` | flash_store | decomp-c | program one halfword; returns FLASH_ST_*; PG cleared via mask `0x1FFE` |
| `0x080049f2` |   74 | `unused_flash_program_option_halfword` | (stdlib) | deferred | dead — zero callers |
| `0x08004a3c` |  100 | `unused_flash_program_option_byte` | (stdlib) | deferred | dead — zero callers |
| `0x08004aa0` |  182 | `unused_flash_program_write_protection` | (stdlib) | deferred | dead — 4 WRP bytes; zero callers |
| `0x08004b72` |   86 | `unused_flash_user_option_byte_config` | (stdlib) | deferred | dead — zero callers |
| `0x08004c4a` |    6 | `flash_clear_status` | flash_store | decomp-c | write-1-to-clear bits in FLASH->SR |
| `0x08004c64` |  106 | `unused_gpio_deinit` | (stdlib) | deferred | dead — GPIO port reset via AHB; zero callers |
| `0x08004cce` |  222 | `gpio_pin_configure` | uart | decomp-c | CRL/CRH 4-bit MODE+CNF per pin, plus optional BSRR/BRR initial level |
| `0x08004dbc` |   20 | `gpio_idr_test` | gpio | decomp-c | `(*(uint32_t*)(port+8) & mask) != 0` — STM32F1-style IDR test |
| `0x08004df4` |    4 | `gpio_bsrr_write` | gpio | decomp-c | raw BSRR write at port+0x10 |
| `0x08004df8` |    4 | `gpio_brr_write` | gpio | decomp-c | raw BRR write at port+0x14 |
| `0x08004e22` |   70 | `gpio_set_af` | uart | decomp-c | AFRL/AFRH nibble write |
| `0x08004e74` |  106 | `nvic_configure` | uart | decomp-c | priority + ISER/ICER |
| `0x08004faa` |   48 | `FUN_08004faa` |  | pending |  |
| `0x08004fda` |   60 | `FUN_08004fda` |  | pending |  |
| `0x08005108` |  160 | `FUN_08005108` |  | pending |  |
| `0x080051a8` |   28 | `rcc_ahben_bits` | uart | decomp-c | set/clear bits in RCC->AHBENR |
| `0x080051c4` |   28 | `rcc_apb2en_bits` | uart | decomp-c | set/clear bits in RCC->APB2ENR |
| `0x080051e0` |   28 | `rcc_apb1en_bits` | hal | decomp-c | RMW bits in RCC->APB1ENR (offset 0x1C); peer of rcc_ahben_bits / rcc_apb2en_bits |
| `0x080051fc` |   28 | `rcc_apb2_reset_bits` | hal | decomp-c | RMW on RCC->APB2RSTR (offset 0x0C); called by `rcc_reset_usart` |
| `0x08005218` |   28 | `rcc_apb1_reset_bits` | hal | decomp-c | RMW on RCC->APB1RSTR (offset 0x10); called by `rcc_reset_usart` |
| `0x080052e8` |   24 | `syscfg_set_mem_mode` | main | decomp-c | RMW low 2 bits of `SYSCFG_CFGR1` (`0x40010000`); boot-prologue helper, called once with `mode=3` |
| `0x08005364` |   54 | `unused_rcc_reset_periph_by_base` | (stdlib) | deferred | dead — peripheral-reset wrapper keyed off base address; zero callers |
| `0x0800539a` |   74 | `tim_time_base_init` | timer | decomp-c | branch on TIMx base: TIM1/2/3 set CR1 counter-mode; TIM1/15/16 also write RCR. Then ARR, PSC, EGR=UG |
| `0x080053e4` |   18 | `tim_time_base_config_init` | timer | decomp-c | TIM_TimeBaseInitTypeDef default: PSC=0, ARR=0xFFFF, CKD=0, Mode=0, RCR=0 |
| `0x08005498` |   26 | `tim_enable` | timer | decomp-c | toggle TIMx->CR1.CEN (bit 0); disable uses `& 0xFFFE` (literal materialised as `0xFF8F+0x6F`) |
| `0x0800561a` |   86 | `unused_gpio_pin_af_config` | (stdlib) | deferred | dead — GPIO AFRL/AFRH nibble setter (`gpio_set_af` in uart.c is the live equivalent); zero callers |
| `0x08005838` |   20 | `FUN_08005838` |  | pending |  |
| `0x0800584c` |   72 | `FUN_0800584c` |  | pending |  |
| `0x08005894` |   18 | `FUN_08005894` |  | pending |  |
| `0x080058a6` |   58 | `FUN_080058a6` |  | pending |  |
| `0x080058e0` |   62 | `FUN_080058e0` |  | pending |  |
| `0x0800596e` |   26 | `tim_dier_bits` | timer | decomp-c | RMW bits in TIMx->DIER (offset 0x0C); disable path ANDs with `~mask & 0xFFFF` |
| `0x080059a0` |    8 | `tim_clear_flag` | timer | decomp-c | `TIMx->SR = ~flag` (SR is rc_w0; writing 0 clears the matching bit) |
| `0x080059a8` |   38 | `FUN_080059a8` |  | pending |  |
| `0x080059ce` |    8 | `FUN_080059ce` |  | pending |  |
| `0x08005a1e` |   24 | `FUN_08005a1e` |  | pending |  |
| `0x08005a36` |   24 | `FUN_08005a36` |  | pending |  |
| `0x08005a4e` |   58 | `FUN_08005a4e` |  | pending |  |
| `0x08005a88` |   26 | `FUN_08005a88` |  | pending |  |
| `0x08005aa2` |   50 | `FUN_08005aa2` |  | pending |  |
| `0x08005ad4` |   34 | `FUN_08005ad4` |  | pending |  |
| `0x08005b2c` |   66 | `FUN_08005b2c` |  | pending |  |
| `0x08005b9c` |   54 | `rcc_reset_usart` | hal | decomp-c | pulse APB1RSTR.USART2RST or APB2RSTR.USART1RST per base address; promoted from `named` after FAA-cluster dependencies landed |
| `0x08005bd2` |  116 | `usart_init` | uart | decomp-c | CR_1C + CR_18 framing + BRR from config struct |
| `0x08005c60` |   24 | `usart_set_enable` | uart | decomp-c | toggle UE (bit 0 of CR_18) |
| `0x08005c78` |   20 | `usart_cr10_bits` | uart | decomp-c | set/clear bits in CR_10 |
| `0x08005ca0` |    6 | `usart_write_data` | uart | decomp-c | static; `*(base+0) = byte` |
| `0x08005ca6` |    8 | `usart_read_data` | uart | decomp-c | read RDR @ offset 0x04 |
| `0x08005cae` |   16 | `usart_test_flag` | uart | decomp-c | static; `(*(base+8) & mask) != 0` |
| `0x08005cbc` |    2 | `thunk_FUN_08005cc0` |  | pending |  |
| `0x08005cc0` |    2 | `FUN_08005cc0` |  | pending |  |
| `0x08005cc4` |   20 | `usart_check_status` | uart | decomp-c | test bits in ISR @ offset 0x0C |
| `0x08005cd8` |    4 | `usart_clear_flag` | uart | decomp-c | write ICR @ offset 0x14 |
| `0x08005ce8` |    8 | `crc_reset` | crc | decomp-c | OEM `CRC->CR = RESET_Msk` |
| `0x08005cf0` |   24 | `crc32_word` | crc | decomp-c | OEM CRC step |
| `0x08005d08` |   32 | `crc32_words` | crc | decomp-c | OEM CRC loop (merged 0x08005D0C) |
| `0x08005d40` |   44 | `__aeabi_uidiv` | runtime | decomp-c | Cortex-M0 unsigned 32-bit softdiv (restoring 31-shift loop); GCC auto-emits calls for every `/` and `%` on uint32_t |
| `0x08005d6c` |   36 | `memcpy` | util | decomp-c | word-fast + byte tail; **void return** (non-POSIX) |
| `0x08005d90` |   36 | `FUN_08005d90` |  | pending |  |
| `0x08005db4` |   28 | `__gnu_thumb1_case_uqi` | libgcc | named | compiler-runtime jump-table dispatcher; not project code |
| `0x08005e2a` |   78 | `FUN_08005e2a` |  | pending |  |
| `0x08005e78` |   38 | `FUN_08005e78` |  | pending |  |
| `0x08005e9e` |    2 | `FUN_08005e9e` |  | pending |  |
| `0x08005ea0` |    2 | `FUN_08005ea0` |  | pending |  |
| `0x08005ea2` |    2 | `FUN_08005ea2` |  | pending |  |
| `0x08005ea4` |    2 | `FUN_08005ea4` |  | pending |  |
| `0x08005ea6` |    2 | `FUN_08005ea6` |  | pending |  |