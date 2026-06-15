# mainware firmware: what changed 1.07.06 → 1.08.02

A function-level diff of the two OEM images `mainware_1.07.06.bin` and
`mainware_1.08.02.bin` (both STM32F413VGT6 Cortex-M4F, app region linked at
`0x08020000` — the VanMoof 0x28-byte application header sits at the image start,
the STM32 vector table at `0x08020200`). The 1.07.06 side is the reconstruction
already in [`../src/`](../src/); this diff says what the **next** OEM release
changed relative to it.

> **Headline:** a **broad release**, not a point fix — ~6 months apart
> (Nov 2021 → May 2022) and **+1 360 bytes (+0.6 %)**. By position-independent
> body-hash, **539 of 836** curated 1.07.06 function bodies survive byte-identical;
> **167 named functions changed** body (158 enough to be genuine logic change, not
> relocation), holding roughly half of the named instruction budget. The change is
> spread across the core state engine, the low-power/wake path, the BLE
> command/telemetry layer, the shifter/gear path, battery, modem, console and the
> boot/clock init — but three user-visible features stand out: an **expanded
> wake-reason taxonomy + wake counter**, the **`gear` console command repurposed
> into a real shifter driver**, and **off-road region removed from the BLE
> control surface**.

## Image header

| Field (file offset) | 1.07.06 | 1.08.02 |
| --- | --- | --- |
| Magic `0x00` | `55 AA 55 AA` | `55 AA 55 AA` |
| Version word `0x04` | `F4 06 07 01` (1.07.06, type 0xF4) | `F4 02 08 01` (1.08.02, type 0xF4) |
| CRC-32 `0x08` | `0xB4DA066F` | `0x4E0F9854` |
| Image size `0x0C` | `0x000356A0` = 218 784 B | `0x00035BF0` = 220 144 B (**+1 360 B**) |
| Build date `0x10` | `Nov  1 2021 10:25:04` | `May  9 2022 10:58:01` |
| Reset vector `0x204` | `0x08043E54` | `0x08044310` |

Both carry a real build date (~6 months apart), so they are ordinary sequential
release builds. The reset vector moved *up* by ~0x4BC as the image grew, i.e.
code was appended and shifted, not heavily reordered.

SHA-256:
- 1.07.06 `e041e66a7110a2bbf6882317f865bfb7d5ba293a4149470cf6367aeb2649b8a1`
- 1.08.02 `e837b5b2136b85acd125e669b2ee9458f25079f0c567d8eeffefdc1eb3de55c8`

## Method

A flat byte-diff is meaningless here — 1.08.02 is 1 360 bytes larger, so every
address past the first size-changing edit shifts and ~82 % of bytes differ.
Instead each function was reduced to a **position-independent body hash** (Ghidra
normalised hash — masks relocated branch/call targets and literal-pool pointers);
equal hash in both images ⇒ byte-identical body.

The 1.07.06 program is the **curated, hand-named** reconstruction (836 functions);
1.08.02 was freshly imported and auto-analysed (739 functions, rebased to
`0x08020000` so its vector table resolves). That curated-vs-fresh asymmetry means
the body-hash identical count is a **lower bound** — where curation split one OEM
function into several, the halves will not hash-match the whole. `bulk_fuzzy_match`
was used as a cross-check but is itself distorted by relocation (it penalises the
shifted absolute-address operands, so many byte-identical bodies score at its 0.7
floor and several genuinely-rewritten large functions fall *below* 0.7 and read as
"unmatched"). The **body-hash is therefore treated as authoritative**; for the
focus functions, `find_similar_functions_fuzzy` (threshold 0.3) recovered the true
1.08.02 counterpart + similarity, and `diff_functions` + side-by-side decompilation
confirmed each change. Per-function before/after is in
[`compare-1.07.06/`](compare-1.07.06/).

## Function-level summary

| Class | Count |
| --- | --- |
| Byte-identical body (position-independent hash match) | **539** (lower bound) |
| Changed body — genuine logic change (sim < 0.7) | ~147 |
| Changed body — tiny IRQ/exception trampolines (relocation-only literals) | ~20 |
| **Total named functions whose body changed** | **167** |

So at least **~64 %** of named function bodies are unchanged; the touched
functions hold ~23.3k of ~45.5k named instructions, but much of that is
relocation/recompilation rather than logic. The reliable facts are: 539 bodies
provably identical, and the genuine logic change concentrated in the subsystems
below.

## What is unchanged

The byte-identical set is the foundation — confirmed **not** touched:

- **HAL / CMSIS / newlib**: the bulk of the CubeF4 HAL (GPIO/I2C/UART/TIM/CRC/RTC
  register engines), the IRQ trampolines, newlib (`memcpy`/`mem*`/`str*`/printf
  engine/`malloc`) and libgcc — most only appear "changed" as relocation noise.
- **`SystemInit`**, `reset_reason_log_and_clear`, `region_speed_preset_table_load`.
- **App leaves verified identical**: `main` (the super-loop frame),
  `console_region_set` (the *console* region cmd — distinct from the BLE region
  path that did change), `console_cmd_setgear` (the command-table record),
  `shifter_send_gear` (the Modbus gear-6 PDU builder), `console_cmd_battery`,
  `modem_sim_state_machine`, `shifterstatus_dump_v201`, `stc3115_init_device`,
  `log_buffer_dump`, and most leaf getters/setters.

## What changed — by subsystem

> Three user-visible feature changes are documented in full in
> [`compare-1.07.06/`](compare-1.07.06/):
> [`wake.md`](compare-1.07.06/wake.md) (wake taxonomy + counter),
> [`gear.md`](compare-1.07.06/gear.md) (the `gear` command rewrite), and the
> behaviourally/regulatorily notable [`region.md`](compare-1.07.06/region.md)
> (**off-road region removed from BLE**).

- **Core state engine** — `status_process` (4882 instr, sim 0.55) changed; it also
  hosts the **new wake counter** (see wake.md).
- **Low-power / wake** — `enter_stop_mode` (sim 0.48) and `log_wake_reason`
  (sim 0.49) substantially rewritten: the wake-reason taxonomy went 9 → 13 classes
  with four new BLE/RTC-suppression sub-modes.
- **BLE command / telemetry** — `ble_cmd_dispatch` (region/off-road change lives
  here), `ble_telemetry_change_broadcast`, `subsystem_update_sm` (OTA),
  `display_mode_sm_step` all changed.
- **Shifter / gear / drivetrain** — the `gear` console handler was repurposed
  (`console_soc_set` → a real gear driver, sim 0.14), plus `shifter_control_step`
  (sim 0.70), `shifter_modbus_rtu_step`, `shifter_seq_status_poll_step`,
  `power_assist_gear_step`, `shifter_mode_command_dispatch`.
- **Battery** — `battery_telemetry_step`, `battery_state_process`,
  `battery_charge_complete_watchdog`.
- **Modem** — `modem_at_exec` plus ~30 `modem_handle_*` / `modem_build_*` helpers.
- **Boot / clock** — `mainware_boot_init_sequence`, `rcc_oscillator_config`,
  `rcc_clock_config` (+ `Cold boot clear log..` → `Cold boot`, new `Clear err 57`).
- **Console** — most `console_cmd_*` handlers recompiled.

## Caveat

This is a multi-version sequential release with pervasive relocation, so a
per-function line diff of *every* changed function is neither tractable nor
meaningful — most apparent change is recompilation. The findings above are what
the evidence supports with confidence: the exact unchanged core, the magnitude
(~64 %+ identical), the touched subsystems, and three user-visible features
characterised at the instruction level in `compare-1.07.06/`.

## Reproduce

Both binaries are in the `vanmoof` Ghidra project: `/mainware_1.07.06.bin` (curated,
named — the `src/` reconstruction) and `/diff/mainware_1.08.02.bin` (fresh
auto-analysis, rebased to `0x08020000`). Body hashes via `get_bulk_function_hashes`
on both; cross-version mapping via `bulk_fuzzy_match` / `find_similar_functions_fuzzy`;
per-function deltas via `diff_functions` + decompilation, adversarially re-verified.
