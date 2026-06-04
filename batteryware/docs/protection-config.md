# BMS protection configuration — `cfg_blk` (`0x200028D0`)

Companion to `hardware.md`, `fedl5236.md`, and `compare-1.14.1/fuse.md`. This
file documents **every protection threshold the firmware uses**, where it lives,
its default value, and how a trip turns into an action.

> All addresses/values are from the curated **1.17.1** image. The protection
> *logic* (the `fg_*_check` functions and the fuse handler) is **byte-identical
> in 1.14.1**, so these thresholds apply to both releases.

## 1. What `cfg_blk` is

`cfg_blk` (Ghidra `s_ctx`) is a **184-byte (`0xb8`) RAM config block at
`0x200028D0`**. It is **not** the same as `bms_ctx`/`cfg` at `0x200029A8` (the
SOC/capacity/cycle block loaded from data-EEPROM `0x08080C00` — see `eeprom.md`).
`cfg_blk` holds the **live protection thresholds** the per-cell and per-current
checkers read every cycle.

It is built once at boot by **`config_init` (`FUN_08007368`, called from
`bms_setup`)**, which:

1. zeroes the block, writes a version stamp (`+0x00 = 0x0117`),
2. fills **most fields from hard-coded defaults baked into the firmware**
   (the values in §2–§4 below), and
3. validates a handful of fields against **data-EEPROM** (`config_init` reloads
   them from `0x0808xxxx`, falling back to the firmware default if the cell is
   `0` or the FW-magic at `0x0808002E` ≠ `0x011701B1`).

So unless a service tool has written different values over Modbus (and they
persisted to EEPROM), **the defaults below are what runs.**

## 2. Runtime protection comparators (the important table)

Every checker runs the same shape: a measured value is compared to a threshold;
while the trip condition holds, a **dwell counter** increments and the fault
**latches** once the counter reaches `delay ÷ 100` consecutive samples; when the
condition clears the counter resets. Each sets one bit in **`g_fault_flags`
(`0x20002C44`)**.

| `g_fault_flags` bit | checker (`fg_*`) | trips when | trip thr (offset → default) | trip delay (offset → default ÷100) | recover thr (offset → default) | measured value |
| --- | --- | --- | --- | --- | --- | --- |
| 0 (`0x01`) | `fg_uvp1_check` | cell byte **≥** thr | `+0x6e` → **85** (`0x55`) | `+0x70` → 3000 (**30**) | `+0x72` → 82 | `0x20002589/8A` |
| 1 (`0x02`) | `fg_uvp2_check` | cell byte **≤** thr | `+0x76` → **40** (`0x28`) | `+0x78` → 3000 (**30**) | `+0x7a` → 43 | `0x20002589/8A` |
| 2 (`0x04`) | `fg_ovp1_check` | cell byte **≥** thr | `+0x7e` → **110** (`0x6e`) | `+0x80` → 3000 (**30**) | `+0x82` → 100 | `0x20002589/8A` |
| 3 (`0x08`) | `fg_ovp2_check` | cell byte **≤** thr | `+0x86` → **20** (`0x14`) | `+0x88` → 3000 (**30**) | `+0x8a` → 23 | `0x20002589/8A` |
| 4 (`0x10`) | `fg_threshold_check` | byte[0] **≥** thr | `+0x8e` → **135** (`0x87`) | `+0x90` → 3000 (**30**) | `+0x92` → 125 | `0x20002588` |
| 6 (`0x40`) | `fg_discharge_oc_check` | \|I\| **≥** thr (while not charging) | `+0x98` → **500** | `+0x96` → 2000 (**20**) | — | `0x20002800` (discharge current) |
| 7 (`0x80`) | `fg_charge_oc_check` | \|I\| **≥** thr (while not discharging) | `+0x9a` → **500** | `+0x96` → 2000 (**20**) | — | `0x200028A0` (charge current) |

Notes:

- **Trip vs recover hysteresis.** The five voltage comparators store a
  *trip/recover pair* (`[u8 trip][u16 trip-delay][u8 recover][u16 recover-delay]`,
  recover-delay default 1500 → 15 samples at `+0x74/0x7c/0x84/0x8c/0x94`). The
  `fg_*_check` functions read only the **trip** side; the recover side is
  consumed by the recovery paths (state handlers / `recover_cnt 0x20002C06`).
- **Two cell readings.** The voltage comparators test **both** `0x20002589` and
  `0x2000258A` (a max/min cell pair, written by `cell_balance_update` from the
  ADC) — bits 0/2 (high-side) and 1/3 (low-side) trip if *either* reading is
  out of range; the window is effectively `[40,110]` with an inner warn at 85
  and a critical guard at 135.
- **OC direction gating.** `fg_discharge_oc_check` only accumulates when
  `mode_flag` (`0x20002870`) bit 1 is clear; `fg_charge_oc_check` only when bit 0
  is clear — so the two never trip on the same current sample. Each also writes a
  **trip-reason byte** at `0x200029E0` (1 = discharge-OC, 2 = charge-OC).
- **Per-comparator dwell counters (RAM):** uvp1 `0x20002A80`, uvp2 `0x20002A8C`,
  ovp1 `0x20002A94`, ovp2 `0x20002A52`, threshold `0x20002ABA`,
  discharge-OC `0x20002A2E`, charge-OC `0x20002B5A`.

> **Scaling caveat (flagged).** The five voltage thresholds and the two cell
> readings are **single bytes** in `cell_balance_update`'s internal scale, *not*
> millivolts — the mV-per-LSB conversion is set inside `cell_balance_update` and
> is **not yet decoded**, so the trip values above are given as raw bytes. The
> over-current threshold `500` and the currents at `0x20002800`/`0x200028A0` are
> likewise in the firmware's internal current units (the ML5236 reads I across a
> 1 mΩ shunt; see `fedl5236.md`). Pinning the byte→mV and unit→A scales is a
> follow-up; cross-check against the authoritative Modbus register map.
> The curated `fg_uvp*/ovp*` names are approximate — read the *comparison column*
> above for the actual polarity (bits 0/2/4 are high-side, 1/3 are low-side).

## 3. What a trip does (and what reaches the fuse)

The fault bits drive the state machine; see
[`compare-1.14.1/fuse.md`](compare-1.14.1/fuse.md) for the full routing. Summary:

| Fault | `g_fault_flags` bit | dispatched handler | action | reaches pyro fuse? |
| --- | --- | --- | --- | --- |
| high-side cell (warn/ovp/guard) | 2, 4 | `state_handler_14`/`16` | open charge FET, recoverable | **no** |
| low-side cell | 1, 3 | `state_handler_15` and `09`/`0a` | open charge FET, recoverable | **no** |
| (escalation tier) | 5 | `state_handler_17_19` | enters fuse handler but **does not** fire PB7 | no |
| **discharge over-current** | **6** | `state_handler_17_19` | **PB7 → blow fuse** | **YES** |
| **charge over-current** | **7** | `state_handler_17_19` | **PB7 → blow fuse** | **YES** |

So in firmware the irreversible fuse is **over-current only** (bits 6/7);
over/under-voltage is recoverable (open the charge FET). Cell **over-voltage is
caught irreversibly in hardware instead**, by the two `S-8215AAD` secondary ICs
(4.35 V/cell, 2 s) that blow the same fuse autonomously — see
[`hardware.md` → Secondary protection & the pyro fuse](hardware.md#secondary-protection--the-pyro-fuse).

## 4. Power-on detection thresholds (`+0x2a…+0x46`, u16)

A second threshold set, read **only at boot** by `main_loop` to emit the
"Power On … Mode" report and pick the entry state. These are 16-bit and look
like **millivolts** (cell-voltage range), unlike the runtime byte comparators:

| offset | default (mV) | offset | default (mV) |
| --- | --- | --- | --- |
| `+0x2a` | 4250 | `+0x3a` | 3000 |
| `+0x2e` | 4150 | `+0x3e` | 3300 |
| `+0x32` | 4300 | `+0x42` | 2800 |
| `+0x36` | 4150 | `+0x46` | 3300 |

`main_loop` compares the two boot cell metrics (`0x20002B…`) against these to
route to `state_handler_07/08/09/0a` or normal (`state_handler_01`). (The exact
min/max-cell mapping and the OVP/UVP-vs-state correspondence are not fully pinned
— flagged.)

## 5. AFE-side (hardware) protections

Independent of `cfg_blk`, programmed into the ML5236 AFE by `bms_init` (see
`fedl5236.md` §3):

| Protection | Setting | Reg |
| --- | --- | --- |
| Short-circuit | **150 mV** across the shunt (≈150 A @ 1 mΩ), autonomous FET cutoff | `SCWDT 0x0E = 0x9A` |
| Communication watchdog | **2 s** | `SCWDT 0x0E` |
| AFE cell over-voltage | **disabled** (`ENOV=0`, threshold maxed `0xFFF`) — OV is handled in MCU firmware + the S-8215AAD hardware stage instead | `SETOV 0x0F = 0x00`, `OVDET = 0xFFF` |

## 6. Other notable `cfg_blk` fields (non-protection)

Set by `config_init`; listed for completeness (offsets are byte offsets):

| Offset | Default | Note |
| --- | --- | --- |
| `+0x00` | `0x0117` | version stamp (1.17) |
| `+0x02` | `0x0310` | EEPROM-validated (`0x08080006`) |
| `+0x0e` | `0x0D01` | EEPROM-validated (`0x08080032`) |
| `+0x10` | `7` | EEPROM-validated low byte (`0x08080034`) |
| `+0x12` | `0x0C4E` | EEPROM-validated (`0x08080036`) |
| `+0x16` | `100` | pre-charge window value (read in `bms_state_machine`) |
| `+0x18` | `4000` | — |
| `+0x1c` | `25000` (u32) | — |
| `+0x24` | `10000` (u32) | — |

## 7. Source of truth / how to change a threshold

- **Firmware defaults:** `config_init` (`FUN_08007368`), the constants in its
  literal pool (`0x08007760…0x080077ac`).
- **Per-pack overrides:** written over Modbus (func `0x06`/`0x10`) and persisted
  in data-EEPROM; `config_init` reloads the EEPROM-validated subset at boot. The
  bulk of the protection thresholds (§2) are **firmware defaults** — they are not
  among the EEPROM-validated fields, so changing them means changing the image
  (or whatever runtime path writes `cfg_blk`).
- **Authoritative external map:** the VanMoof BMS Modbus register map documents
  the register↔meaning↔scaling; use it to resolve the byte→mV and unit→A scales
  flagged in §2.
