# PowerBank firmware: what changed 1.11.01 → 1.11.05

A function-level diff of the two OEM images
`powerbank_firmware_1.11.01.bin` and `powerbank_firmware_1.11.05.bin`
(both STM32F091xC, app linked at `0x08008000`, whole image incl. the
0x28-byte VanMoof header loaded at that base in Ghidra).

## Image header

| Field (file offset) | 1.11.01 | 1.11.05 |
| --- | --- | --- |
| Magic `0x00` | `55 AA 55 AA` | `55 AA 55 AA` |
| Version word `0x04` | `B2 01 11 01` (1.11.01, type 0xB2) | `B2 05 11 01` (1.11.05, type 0xB2) |
| CRC-32 `0x08` | `0x603C5605` | `0x1691DE64` |
| Image size `0x0C` | `0x17000` = 94 208 B | `0x1725C` = 94 812 B (**+604 B**) |
| Build date `0x10` | `Mar 12 2021` | `Apr 12 2021` |
| Build time `0x1C` | `16:02:35` | `10:19:14` |
| Reset vector `0x2C` | `0x08019065` | `0x08019271` |

SHA-256:
- 1.11.01 `fa5cd7b088350b982eec282fe5cbee12b9022eb1be883b92f7af2ce2b4242215`
- 1.11.05 `a07d1f43a40675d1f6530127d268585e162295b1905026a20a9af3dc41ef2712`

## Method

A flat byte-diff is meaningless here: 1.11.05 is 604 bytes larger, so every
address past the first size-changing edit shifts and "everything differs"
downstream. Instead each function was reduced to a **position-independent
body hash** (Ghidra normalised hash, masks relocated branch/call targets and
literal-pool pointers). Same hash in both images ⇒ identical body; differing
hash ⇒ candidate change. Candidates were then triaged by **instruction
count** and confirmed with structured per-function diffs / decompilation.

1.11.01 was imported, rebased `0x00000000 → 0x08008000`, and auto-analysed
(280 functions) to match 1.11.05's 285.

## Function-level summary

| Class | Count |
| --- | --- |
| Identical body (hash match, relocated only) | 210 |
| Differing hash but **same instruction count** — relocation/disassembly-rendering only, no logic change | 60 |
| Differing hash **and** instruction-count change | 16 |
| — of those: startup/runtime fns present in both, only carved separately by 1.11.05's heavier analysis (not real changes) | 5 |
| — of those: **genuine logic changes** | 11 |
| Functions removed | 0 |

The "relocation/rendering only" class was verified on representative
functions (`gpio_pin_config`, `bms_core_update`): identical instruction
count, identical prologue/epilogue, and the only "diff" lines are Ghidra
rendering a relocated pointer access as `ldr r3,[r3,r2]` vs `ldr r3,DATA_EXT`.
No behavioural difference.

The 5 false "new" functions (`Reset_Handler` `0x08019270`,
`__libc_init_array_lite` `0x0801dac4`, `__aeabi_uldiv_helper` `0x08008110`,
`ADC1_COMP_IRQHandler` `0x08008688`, stub `0x0801db3c`) exist in 1.11.01 too;
they only appear "added" because the lighter auto-analysis of the freshly
imported 1.11.01 didn't split them out as separate functions.

## Main change: temperature-offset (NTC calibration) hardening

The three largest logic changes are one feature: the three stored
**temperature-offset / NTC-calibration bytes** (BMS config struct offsets
`+8`, `+9`, `+10`) gained a uniform validation regime. A valid offset is a
small signed value — byte in `0x00..0x14` (0..+20) or `0xEB..0xFF` (−21..−1);
the middle range `0x15..0xEA` is rejected, and the defaults are `(0, 3, 3)`.

| Function | 1.11.01 → 1.11.05 | Change |
| --- | --- | --- |
| `bms_config_reset` (`0x08011b08`, +70 instr) | 290 → 360 | Now writes the three offset bytes from their globals with the range check (reset to `0/3/3` if in the invalid middle range) instead of just zeroing struct `+8`/`+0xc`; and **adds a `fedl5236_record_save()`** at the end so the reset config is persisted. |
| `bms_system_init` (`0x0801156c`, +81 instr) | 490 → 571 | At boot, loads the three offset bytes into globals and **range-checks them**; on a device-identity match (`DAT_080119b4`) mismatch or an out-of-range offset it forces `0/3/3` and calls `bms_config_reset()`. 1.11.01 only validated them loosely inside the hardware-version-change branch. |
| `modbus_write_single` (`0x08018410`, +30 instr) | 378 → 408 | The three Modbus registers that write these offset bytes changed their accept test from "value ≠ 0" to "value `< 0x14` or `> 0xEB`" (accept only a valid small signed offset), and now also mirror the accepted value into RAM shadow globals (`0x…87d4/d8/dc`). Still persists via `fedl5236_record_save()`. |

Supporting / smaller changes:

| Function | Δinstr | Change |
| --- | --- | --- |
| `bms_state_6` (`0x08010f50`) | +10 | Charger-detect (PUPIN, PA8) handling reworked — see below. |
| `bms_state_11` (`0x08008e88`) | +1 | slow-cadence block: **added `alarm_scan_b6()`** after `alarm_scan_b5()`. |
| `bms_state_12` (`0x08009058`) | +1 | same: **added `alarm_scan_b6()`**. |
| `bms_state_18` (`0x08009228`) | −1 | slow-cadence block: **removed `alarm_scan_b7()`** (was `b5;b6;b7`, now `b5;b6`). |
| `bms_state_21` (`0x0800ab00`) | +1 | slow-cadence block: **added `alarm_scan_b6()`** (was `b5;b7`, now `b5;b6;b7`). |
| `FUN_0801879e` (`0x0801879e`) | +6 | Modbus helper in the `modbus_write_single` jump-table region; diff dominated by jump-table disassembly, only real change a trailing constant `#4 → #12`. |

### BMS state alarm-scan retuning (states 11/12/18/21)

`alarm_scan_b0..b7` (`0x08013a78`..`0x08013e64`) are the per-byte alarm/flag
scanners run in each state's slow-cadence (`flags & 2`) block. 1.11.05 retunes
*which* scans run in *which* state — the only change in these four handlers
(everything else is byte-identical):

| State | 1.11.01 runs | 1.11.05 runs | Δ |
| --- | --- | --- | --- |
| 11 | `b5` | `b5, b6` | +`b6` |
| 12 | `b5` | `b5, b6` | +`b6` |
| 18 | `b5, b6, b7` | `b5, b6` | −`b7` |
| 21 | `b5, b7` | `b5, b6, b7` | +`b6` |

Net effect: `alarm_scan_b6` is now evaluated in states 11/12/21 (it was not),
and `alarm_scan_b7` is no longer evaluated in state 18. No thresholds or other
logic in these handlers changed.

### `bms_state_6` — charger-detect (PUPIN) rework

The charger-present pin (read via `gpio_bit_read(PA8, 0x100)`) drives the
decision to enter charge mode (`boot_mode_enter(1)`). 1.11.05 reworked it:

| | 1.11.01 | 1.11.05 |
| --- | --- | --- |
| Where the pin is sampled | fast block (`flags & 1`, every conversion-ready tick) | slow block (`flags & 2`, after `alarm_scan_b5`) |
| Debounce before `boot_mode_enter(1)` | `> 0x13` (≈20 samples) | `> 3` (4 samples) |
| Guard on the trigger | config byte `+0x5c == 0` | runtime status bit 8 clear (`(flags2 & 0x1ff) >> 8 == 0`) |
| Pin-high (charger removed) branch | reset counter only | reset counter **and** clear status bit 8 if it was latched |
| On trigger | set bit 8, `boot_mode_enter(1)` | additionally `log_print` + `uart_flush` before entering |
| Low-charge `boot_mode_enter(3)` path | gated on config byte `+0x5c == 0` | gate removed — taken unconditionally (with `uart_flush` added) |

So the charger-detect debounce moved to the slower cadence, was shortened from
~20 to 4 samples, switched its gate from a stored config byte (`+0x5c`) to a
runtime status bit, and now logs the transition. The two `boot_mode_enter`
paths no longer depend on config byte `+0x5c` at all.

## Not changed

The entire HAL/driver layer (GPIO, I²C, SPI, UART, RTC, ADC, timers, flash,
CRC), the FEDL5236 AFE core read/command path, the Modbus framing/CRC, the
coulomb counter, the power-path / Vout / bypass logic, `main`, and the rest of
the BMS state machine are **byte-identical** modulo relocation. 1.11.05 is a
narrow maintenance release: it tightens validation and persistence of the
NTC temperature-offset calibration and makes a few small BMS state tweaks.

## Reproduce

Both programs are in the `vanmoof` Ghidra project at base `0x08008000`.
`/tmp/pbdiff/diff.py` and `/tmp/pbdiff/icount.py` hold the hash and
instruction-count diff used above.
