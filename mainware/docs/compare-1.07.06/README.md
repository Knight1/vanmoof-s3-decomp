# mainware 1.07.06 → 1.08.02 — per-function comparison

Focused before/after for the user-visible feature changes between the two OEM
images. The release-level diff (magnitude, method, unchanged core) is in
[`../changelog-1.07.06-to-1.08.02.md`](../changelog-1.07.06-to-1.08.02.md); this
folder decodes the **actual code** change for each flagged function.

The "before" (1.07.06) side is the reconstruction already in
[`../../src/`](../../src/); the "after" (1.08.02) side is the fresh OEM image
imported at `/diff/mainware_1.08.02.bin` (base `0x08020000`). Addresses are Ghidra
addresses in each program's own space.

> ### ⚠ Behavioural / regulatory: off-road region removed → [region.md](region.md)
> The BLE region-set path (cmd `0x5535`/`0x553c`) **deleted the off-road option**:
> the `Region 'off road' is not allowed` guard is gone, region code 3
> (`REGION_OFFROAD`) is no longer selectable, and the legacy `0xFF` off-road
> sentinel is now **coerced to region 0 (EU)**. So 1.08.02 cannot be put into
> off-road (speed-cap-lifted) mode over BLE. Full analysis in [region.md](region.md).

## How this was produced

1. Position-independent body hashing over both images flagged which functions
   changed vs. are byte-identical (see the changelog's Method).
2. For each flagged function the 1.08.02 counterpart was located by
   `find_similar_functions_fuzzy` and/or by string-xref anchoring, both versions
   decompiled, and `diff_functions` run.
3. Every claimed change was **adversarially re-verified** — a second pass re-read
   both disassemblies and discarded relocation / literal-pool / stack-frame /
   register-allocation artifacts. Only genuine semantic deltas are listed.

## Index of changed functions (the feature set)

| Function (1.07.06) | 1.07.06 | 1.08.02 | sim | One-line | Doc |
| --- | --- | --- | --- | --- | --- |
| `enter_stop_mode` | `080382d0` | `0803efa8` | 0.48 | wake-reason taxonomy 9 → 13 classes; +4 `noBLE`/`noRTC` sub-modes; ERROR path split out | [wake.md](wake.md) |
| `log_wake_reason` | `0803da3c` | `0803e88c` | 0.49 | reworked alongside the expanded taxonomy | [wake.md](wake.md) |
| `status_process` | `0802aaf8` | `08029e18` | 0.55 | hosts the **new saturating wake counter** (`Wake counter %d`); broad per-state churn | [wake.md](wake.md) |
| `console_soc_set` → gear cmd | `080425ac` | `08031790` | 0.14 | **`gear` cmd repurposed**: SOC-override deleted; now gears 1–4, `0`=shifter off, `99`=reset SM | [gear.md](gear.md) |
| `ble_cmd_dispatch` (region path) | `08033970` | `0803309c` | 0.57 | **off-road removed** from BLE region-set; off-road→EU coercion; range tightened | [region.md](region.md) |
| `shifter_control_step` | `08028870` | `08041fec` | 0.70 | drivetrain SM recompiled (relocation-heavy; gear plumbing aligns with the new cmd) | [gear.md](gear.md) |

(Plus broad recompilation across `subsystem_update_sm`, `ble_telemetry_change_broadcast`,
`display_mode_sm_step`, `battery_telemetry_step`/`battery_state_process`, the modem
AT layer, the boot/clock init, and most `console_cmd_*` — see the changelog.)

## Cross-cutting themes

1. **Power/wake observability up.** The wake-reason enum gains BLE- and
   RTC-suppression variants (`Wake:noBLE`, `Wake:noBLEnoRTC`, `Wake:NoMemsnoBLE`,
   `Wake:NoMemsnoBLEnoRTC`) and a new boot-survivable **wake counter** — the sleep
   path now reports *why* and *how often* the bike woke. The LIS3DH-fail "ERROR"
   wake gets its own logging path (`  ERR2 LIS3DH` / `Wake:ERROR`).
2. **The `gear` debug command became a real shifter tool.** What was a hidden
   SOC-override (`Set SOC %d`, removed) is now a gear driver that powers the
   shifter (GPIO PE14), commands gears 1–4 over Modbus, powers it off (`0`), or
   drives the reset state machine (`99`).
3. **Off-road de-listed.** Off-road (the region that lifts the speed cap) is no
   longer reachable from the BLE control surface — the option, its reject message,
   and its region code were removed and the sentinel folded into EU.
4. **Logging rationalised.** `Cold boot clear log..` → `Cold boot`; new
   `Clear err 57`, `Shifter off`, `4weeks`. Most of the apparent churn elsewhere is
   recompilation/relocation, not logic.

## Note for the `src/` reconstruction

`src/` reconstructs **1.07.06**, so the "before" columns here equal the current
source. If 1.08.02 ever becomes a target, the six functions above (plus the modem
AT layer and the console handlers) are the ones whose `src/` bodies would need
re-derivation; the other ~539 are byte-identical and port directly.

> Decompiler caveat: snippets are Ghidra pseudo-C. `DAT_xxxx` are literal-pool
> pointers; differing `DAT_`/absolute-RAM addresses between versions are
> relocation and are **not** treated as changes.
