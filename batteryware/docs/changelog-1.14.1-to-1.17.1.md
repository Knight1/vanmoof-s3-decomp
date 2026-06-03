# batteryware firmware: what changed 1.14.1 → 1.17.1

A function-level diff of the two OEM images `batteryware_1.14.1.bin` and
`batteryware_1.17.1.bin` (both STM32L072-class Cortex-M0, app region linked at
`0x08005000`; loaded in Ghidra at base `0x08000000` so the 0x28-byte VanMoof
header sits at the image start).

> **Headline:** unlike the PowerBank 1.11.01→1.11.05 patch (a surgical
> +604-byte release where 11 functions changed for one feature), batteryware
> 1.14.1→1.17.1 is a **broad release** — three minor versions and **+3628 bytes
> (+4.3 %)**. Under symmetric analysis only ~41 % of function bodies are
> byte-identical; the rest were relocated, restructured, or added. The change is
> spread across the BMS state machine, the fuel-gauge/coulomb path, the UART
> command/telemetry processor, and init/HAL bring-up — not one isolated feature.

## Image header

| Field (file offset) | 1.14.1 | 1.17.1 |
| --- | --- | --- |
| Magic `0x00` | `55 AA 55 AA` | `55 AA 55 AA` |
| Version word `0x04` | `B1 01 14 01` (1.14.1, type 0xB1) | `B1 01 17 01` (1.17.1, type 0xB1) |
| CRC-32 `0x08` | `0x27F3DE41` | `0x2E0150DA` |
| Image size `0x0C` | `0x000147E4` = 83 940 B | `0x00015610` = 87 568 B (**+3 628 B**) |
| Build date `0x10` | `Nov  1 2021 14:32:13` | **(blank — zeroed)** |
| Reset vector `0x2C` | `0x08014E6D` | `0x080131F9` |

The build-date/time field is all-zero in 1.17.1 (1.14.1 carries
`Nov 1 2021 14:32:13`), so the two were produced by different build setups. The
reset vector moved *down* by ~0x1C00 even though the image grew — i.e. functions
were heavily reordered, not just appended.

SHA-256:
- 1.14.1 `6d05e57abcc4951c1ff05c9f6b083269f70b0195aadb0f9a82b3f4c921903953`
- 1.17.1 `da0ad7eaee060dfe967a1eea97c6202636b303555c8a2e166860c99eddf86fe8`

## Method

A flat byte-diff is meaningless: 1.17.1 is 3 628 bytes larger and functions are
reordered, so every address past the first size-changing edit shifts. Each
function was reduced to a **position-independent body hash** (Ghidra normalised
hash — masks relocated branch/call targets and literal-pool pointers); equal
hash in both images ⇒ byte-identical body.

The first attempt compared the curated 1.17.1 program (286 functions, refined by
hand over many sessions) against a fresh auto-analysed 1.14.1 (243 functions).
That produced misleading noise because the two were carved with **different
function boundaries** — e.g. 1.14.1 auto-analysis carves the whole UART command
processor as one 3 427-instruction function (`FUN_0800d004`), while the curated
1.17.1 had it split into named sub-blocks. Boundary mismatch makes equal code
hash differently.

To remove that asymmetry, **both binaries were re-imported fresh and analysed
identically** (pure auto-analysis, base `0x08000000`), giving 226 (1.17.1) and
231 (1.14.1) symmetric functions. The hash multiset of those two sets is the
basis for the counts below; instruction-count multiset triage then separates
relocation-only churn from genuine size changes (the same technique used for the
PowerBank diff). Representative changes were confirmed with `diff_functions`
and side-by-side decompilation. (Function names below come from the curated
1.17.1 program, mapped by entry-point address.)

## Function-level summary (symmetric auto-analysis: 226 vs 231)

| Class | Count |
| --- | --- |
| Byte-identical body (position-independent hash match) | **93** |
| Differing hash but **same instruction count** — relocation-only, no logic change | ~35 |
| Differing hash **and** size — genuinely changed, restructured, or new | ~100 |

So roughly **41 % of function bodies are unchanged**, ~15 % moved without logic
change, and ~44 % were touched. (Contrast: the PowerBank patch left ~74 %
byte-identical.) The exact split of the last bucket is fuzzy — it includes a
handful of extra RAM trampoline *veneers* present in 1.17.1 (more code copied to
RAM) that are individually unchanged, and the instruction-count triage
mis-pairs some small same-size helpers. The reliable, defensible facts are: 93
bodies provably identical, and the changed work is concentrated in the
subsystems listed below.

## What is unchanged (the leaf / driver / protection layer)

The byte-identical set is the low-level foundation — these are confirmed
**not** touched between the two versions:

- **libc / math runtime**: `__aeabi_uidivmod`/`uidiv`/`ldivmod`/`lmul`,
  `__clzsi2`/`__clz64`, `memcpy`, `memcpy_byte`, `memcpy_halfword`,
  `memset_byte_*`.
- **HAL leaf drivers**: `gpio_bit_read`/`gpio_bit_write`, `flash_op_start`,
  `flash_program_start`, `flash_enable_prefetch`, `flash_dma_start`,
  `flash_write_verify`, `dma_deinit`/`dma_stop`/`dma_channel_reset`,
  `tick_get`/`tick_ms_get`/…, `nvic_reconfigure`, `EXTI0_1_IRQHandler`,
  `EXTI4_15_IRQHandler`, `main_clock_setup`.
- **Fuel-gauge protection checks**: `fg_ovp1/ovp2/uvp1/uvp2_check`,
  `fg_charge_oc_check`, `fg_discharge_oc_check`, `fg_threshold_check`,
  `fg_alert_monitor`, `fg_clear_status`, `fg_read_field_8/11`.
- **Misc BMS/IO**: `charge_mosfet_on`/`off`, `discharge_mosfet_set`,
  `bms_configure`, `rsoc_lookup`, `rsoc_set`, `capacity_decrement`,
  `config_resend_all`, `led_flash`, `crc8_calc`, `crc16_calc`,
  `temp_offset_send`, `state_flags_handler`, `uart_printf`, `uart_puthex_16`,
  `uart_tx_isr`, `modem_config`, `nibble_to_hex`/`hex_to_nibble`,
  `atoi_hex_offset1`, `ymodem_send_byte`.

## What changed — by subsystem

The touched functions cluster into a few areas:

- **BMS state machine & per-state handlers** — `bms_state_machine` (1107 vs
  1121 instr), `bms_set_state` (395 vs 369), `bms_init` (389), `bms_setup`
  (434), and the bulk of the `state_handler_*` / `state_timer_*` family
  (states 01,02,07–16,17_19) all differ.
- **Fuel gauge / coulomb counting** — `fg_coulomb_update`, `fg_scan`,
  `cell_balance_update`, `fg_watchdog_kick`, `fg_read_loop`,
  `fg_status_flag_get`.
- **UART command / telemetry processor** — the monolithic processor and its
  helper tree (`FUN_0800afa4` and the `cmd_*` / report blocks) were
  restructured (see below).
- **Init / bring-up & HAL config** — `peripheral_init`, `dma_init`,
  `rcc_reconfigure`, `clock_prescaler_val`, `dma_usart_init`, `usart1_dma_setup`,
  `uart_set_config`, `hal_uart_init`, `smbus_transmit`, the flash erase/program
  helpers, plus several `nvic_enable_irq*` variants.
- **CAN** — `can_transmit` differs.

### `bms_set_state` — telemetry/history record extended (369 → 395 instr)

`bms_set_state` records each state transition and snapshots a telemetry record
into the history ring. 1.17.1 **widened that record from 0x30 (48) to 0x38 (56)
bytes** and appended new fields:

| | 1.14.1 (`FUN_080069cc`) | 1.17.1 (`bms_set_state`) |
| --- | --- | --- |
| Record size / ring stride | `0x30` (48 B), stride `idx*0x30` | **`0x38`** (56 B), stride `idx*0x38` |
| Status fields stored | two raw bytes | **`fg_charge_status()`, `fg_status_flag_get()`, `fg_status_flag2_get()`** appended (charge state + 2 protection-flag words) |
| On transition | clears status, returns | additionally **sets a flag byte and emits `uart_printf(...)`** |

So each history entry now carries the charge state and protection-flag snapshot,
and every transition is logged over UART. The +8 record bytes, +3 fields, and
the log call account for the +26 instructions.

### UART command processor — recompiled frame + refactored helpers

`diff_functions` on the processor (1.17.1 `FUN_0800afa4`, 3454 instr ↔ 1.14.1
`FUN_0800d004`, 3427 instr) reports similarity 0.55 with **1534 instructions
equal**. The bulk of the diff is *not* logic:

- **Relocation churn** — shifted `REL+N` branch targets and `DATA_EXT`
  literal-pool accesses throughout.
- **A recompiled stack frame** — 1.14.1 builds a 0x11c-byte frame with `r7` at
  `sp+0x18` and saves `r8`/`r9`; 1.17.1 uses a 0x84-byte frame with `r7` at
  `sp+0`. Every local-variable offset immediate shifts accordingly
  (`208/204/200/220` → `100/96/92/…`).

The genuine structural change is the **helper call tree**: 1.17.1's processor
delegates to two large new helpers `FUN_0800ce9e` (774 instr) and `FUN_0800d8f0`
(1044 instr), where 1.14.1 used `FUN_0800f4c0` (971) and `FUN_0800ef2c` (290).
The Modbus jump-table dispatch core itself is preserved. Combined with the
`bms_set_state` change, the theme is an **expanded telemetry/reporting path**.

## Caveat

Because this is a multi-version release with pervasive reorganisation, a
PowerBank-style exhaustive per-function line diff of *every* changed function is
neither tractable nor especially meaningful — much of the apparent change is
relocation and recompilation. The findings above are what the evidence supports
with confidence: the exact unchanged core, the magnitude (~41 % identical), the
subsystems that were touched, and two representative changes characterised at
the instruction level.

## Reproduce

Both binaries are in the `vanmoof` Ghidra project. The curated programs are
`/batteryware_1.17.1.bin` (named) and `/batteryware_1.14.1.bin` (rebased to
`0x08000000`). Symmetric pure-auto copies used for the counts were imported
under `/diff/`. `/tmp/bwdiff/f17.txt` and `/tmp/bwdiff/f14.txt` hold the
`addr hash instr_count` triples; the multiset + instruction-count triage script
is alongside them.
