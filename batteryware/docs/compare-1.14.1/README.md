# batteryware 1.14.1 → 1.17.1 — per-function comparison

Focused before/after for the functions in the "interesting" set
(`memcpy* / fg_* / *_mosfet_* / bms_configure / rsoc_lookup / bms_* /
cell_balance_update`). The broader release-level diff is in
[`../changelog-1.14.1-to-1.17.1.md`](../changelog-1.14.1-to-1.17.1.md); this
folder zooms into the BMS / fuel-gauge / charge-path functions and decodes the
**actual code** change for each.

Of the 29 functions matching that set, **12 changed** and **17 are
byte-identical** (position-independent body-hash match). The 1.17.1 side ("after")
is the reconstruction already in [`../../src/`](../../src/); this set documents
what the 1.14.1 side ("before") looked like and exactly what changed.

## How this was produced

1. Position-independent body hashing over both images (symmetric pure-auto Ghidra
   analysis) flagged which of these functions changed vs. are identical.
2. For each changed function, both versions were decompiled from Ghidra
   (`/batteryware_1.17.1.bin` = named "after"; `/batteryware_1.14.1.bin` = "before"),
   the 1.14.1 counterpart located by hint or by register/constant content-matching.
3. Every claimed change was then **adversarially re-verified** by an independent
   pass that re-read both disassemblies and discarded anything that was only a
   relocation / `DATA_EXT` / stack-frame / register-allocation artifact. Only
   genuine semantic deltas are listed. Where the verifier caught the first pass
   under- or over-counting, the corrected result is what appears here.

Addresses are Ghidra addresses (both images loaded at base `0x08000000`).

## Index of changed functions

| Function | 1.17.1 | 1.14.1 | Verdict | One-line | Doc |
| --- | --- | --- | --- | --- | --- |
| `charge_mosfet_set` | `08002d50` | `08003774` | logic-change | drops two status-word bit ops (0x20000 / 0x200); only bit-0x40 + GPIO survive | [charge.md](charge.md) |
| `fg_watchdog_kick` | `080031d8` | `08003be4` | logic-change | recovery path drops `bms_configure(saved)`; clear+write block made mutually-exclusive | [fuel_gauge.md](fuel_gauge.md#fg_watchdog_kick) |
| `fg_read_loop` | `08004634` | `08005028` | logic-change | new `if(idx==9)` → store to dedicated global `0x20002824` instead of array slot | [fuel_gauge.md](fuel_gauge.md#fg_read_loop) |
| `cell_balance_update` | `08000880` | `0800083c` | logic-change | new outer gate: RAM `0x200024f4` must equal `0x40012400` or the whole routine is a no-op | [fuel_gauge.md](fuel_gauge.md#cell_balance_update) |
| `fg_scan` | `0800325c` | split of `08003c7c` | refactor-split | front half of the old combined scan/coulomb fn; threshold-scan guard removed | [fuel_gauge.md](fuel_gauge.md#fg_scan--fg_coulomb_update) |
| `fg_coulomb_update` | `080039c2` | split of `08003c7c` | refactor-split + logic | extracted coulomb/temp-protection half; temp gate widened, mask-clears dropped, reg-0x68 scan removed, idx==9 store | [fuel_gauge.md](fuel_gauge.md#fg_scan--fg_coulomb_update) |
| `fg_charge_status` | `0800997c` | inline in `0800d004` | logic-change | extracted from the UART report cascade; dropped the `status==3 && !flag&2 → set bit1` clause | [fuel_gauge.md](fuel_gauge.md#fg_charge_status) |
| `fg_status_flag_get` | `08009a10` | inline in `0800d004` | refactor-split | extracted flag-byte bit0 test into a standalone accessor (no logic delta) | [fuel_gauge.md](fuel_gauge.md#fg_status_flag_get--fg_status_flag2_get) |
| `fg_status_flag2_get` | `08009a44` | inline in `0800d004` | refactor-split | extracted flag-byte bit1 test into a standalone accessor (no logic delta) | [fuel_gauge.md](fuel_gauge.md#fg_status_flag_get--fg_status_flag2_get) |
| `bms_init` | `08004d04` | `08005720` | logic-change | FEDL5236 reg `0x0e` init `0x0a → 0x9a`; 6 of 7 debug `uart_printf` removed | [bms.md](bms.md#bms_init) |
| `bms_state_machine` | `08002194` | `08002b70` | logic-change | host-enable gate removed (protection dispatch now unconditional); bit-4 set→clear on state 0xb/0xc; state-4 recharge timeout 50→300 ticks | [bms.md](bms.md#bms_state_machine) |
| `bms_set_state` | `08005b34` | `080069cc` | logic-change | telemetry record `0x30→0x38`; +3 fg-status fields; +`uart_printf` per transition | [bms.md](bms.md#bms_set_state) |

## Cross-cutting themes

These individual deltas line up into a few coherent release intentions:

1. **Logging rationalised.** `bms_init` removes 6 of its 7 `uart_printf` debug
   prints (only the entry banner survives), while `bms_set_state` *adds* a
   per-transition `uart_printf` plus three structured status fields — i.e. noisy
   boot prints out, structured per-state telemetry in.
2. **Cell index 9 → dedicated global `0x20002824`.** Both `fg_read_loop` and the
   coulomb cell-current tail gain the same `if (index == 9) *0x20002824 = value;`
   special case, mirroring that one channel out of the cell array.
3. **Protection gating restructured.** `bms_state_machine` drops the host/comms
   enable gate so protection dispatch runs unconditionally; `fg_scan` drops the
   `flags&1` guard so its threshold scans run unconditionally; `fg_watchdog_kick`
   moves its clear+write into a branch mutually-exclusive with the recovery path.
4. **Temperature protection widened.** The coulomb temp gate gains two OR terms
   (status bits 9 and 10) and stops auto-clearing bits 8/9/10 — those flags now
   latch — and the reg-`0x68` retry sub-scan is removed.
5. **AFE config tweak.** FEDL5236 register `0x0e` boot value changes `0x0a → 0x9a`.
6. **Status-word bit maintenance changed.** `charge_mosfet_set` stops maintaining
   status bits `0x20000`/`0x200`; `bms_state_machine` inverts bit-4 (set→clear) on
   fault-recovery state `0xb`/`0xc` entry.
7. **New safety/identity gate on balancing.** `cell_balance_update` becomes a
   no-op unless RAM word `0x200024f4 == 0x40012400`.
8. **Telemetry/status accessors refactored** out of the inline UART report cascade
   (`FUN_0800d004`) into standalone functions; `fg_charge_status` drops its
   `flag&2` OR-bit1 clause (that bit is now reported only via `fg_status_flag2_get`).

## Byte-identical in this set (no change)

Confirmed unchanged (position-independent hash match); listed for completeness so
the "before" of these equals the current `src/`:

| Function | 1.17.1 | 1.14.1 |
| --- | --- | --- |
| `memcpy` | `08009412` | `0800a40c` |
| `memcpy_byte` | `08007cf8` | `08008e58` |
| `memcpy_halfword` | `0800f11a` | `080109dc` |
| `discharge_mosfet_set` | `08002cb8` | `080036dc` |
| `charge_mosfet_on` | `080094ec` | `0800ada0` |
| `charge_mosfet_off` | `08009520` | `0800add4` |
| `bms_configure` | `080052d8` | `08005dfc` |
| `rsoc_lookup` | `08001920` | (curated) |
| `fg_ovp1_check` | `08009650` | `0800af60` |
| `fg_ovp2_check` | `080096cc` | `0800afdc` |
| `fg_uvp1_check` | `08009558` | `0800ae68` |
| `fg_uvp2_check` | `080095d4` | `0800aee4` |
| `fg_threshold_check` | `08009748` | `0800b058` |
| `fg_alert_monitor` | `080097b0` | `0800b0c0` |
| `fg_discharge_oc_check` | `0800980c` | `0800b11c` |
| `fg_charge_oc_check` | `0800989c` | `0800b1ac` |
| `fg_clear_status` | `08009a80` | `0800b57c` |
| `fg_read_field_8` | `08010944` | `08011f9c` |

> Decompiler caveat: snippets are Ghidra-rendered pseudo-C. `DAT_xxxx` are
> literal-pool pointers; the comments resolve them to the relevant RAM globals /
> FEDL5236 register numbers / constants. Differing `DAT_` addresses and absolute
> RAM globals between versions are relocation and are **not** treated as changes.
