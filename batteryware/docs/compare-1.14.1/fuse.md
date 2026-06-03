# Secondary-fuse path — 1.14.1 → 1.17.1 (safety-critical)

`state_handler_17_19` is the **only** code path that blows the pack's one-shot
secondary fuse: it drives **GPIOB PB7 (mask `0x80`) HIGH**, energizing a resistive
heater that melts a pyro/thermal fuse and **permanently** severs the pack
(MOS-failure last resort). See `hardware.md` (PB7 row) and
[fuel_gauge.md](fuel_gauge.md) / [bms.md](bms.md) for the surrounding handlers.

This note answers one question exhaustively: **did anything that decides whether
or when the fuse fires change between 1.14.1 and 1.17.1?** Every fuse-handler call
site in both images was decompiled and the result independently re-verified.

## TL;DR

| Aspect | Verdict |
| --- | --- |
| Fuse **trigger condition** (what fires PB7) | **unchanged** — byte-identical |
| Fuse **blow write** (`gpio_bit_write(GPIOB, 0x80, 1)`) | **unchanged** |
| Handler **body** | one edit, in the *post-decision* force-off tail (`s_bms_cfg` bit `0x200`→`0x800`) — does **not** read any trigger input |
| **When the handler is reached** (the dispatchers) | **CHANGED** — a hardware interlock (GPIOH **PH0** must be HIGH) was **removed from every per-state dispatcher** |

Net: 1.17.1 can reach the fuse blow in **more** situations than 1.14.1, because a
physical-pin precondition that previously had to be satisfied was deleted. The
fuse's own three firing conditions did not change.

## 1. The trigger is unchanged

The PB7-high decision still requires all three, identical in both versions:

```c
if (((*s_prot_status >> 11) & 1) == 0)                 /* (1) not the recoverable bit-11 class */
    if (((*g_fault_flags >> 6) & 1) || ((*g_fault_flags >> 7) & 1))  /* (2) discharge-OC or charge-OC latched */
        if (((*s_bms_cfg >> 15) & 1) == 0)             /* (3) not mid telemetry/EEPROM update */
            gpio_bit_write(0x50000400, 0x80, 1);       /* PB7 HIGH — irreversible */
```

Same masks (`0xfff/0x7f/0xff/0xffff`), same shifts (11/6/7/15), same pin (`0x80`),
same value. The globals relocated (`s_bms_cfg` `0x20000a84`→`0x20002c00`,
`s_prot_status` `0x20000740`→`0x2000286c`, `g_fault_flags` `0x20000ac8`→`0x20002c44`)
but the logic is identical.

## 2. The one body edit is downstream of the decision (harmless to the fuse)

After the trigger decision, the handler force-opens everything on **every** entry.
One instruction in that tail changed:

```c
// 1.14.1 (via helper FUN_0800ae38):  ... gpio_bit_write(GPIOB,0x200,0); *s_bms_cfg &= ~0x200;  (clear bit 9)
// 1.17.1 (inlined):                  ... gpio_bit_write(GPIOB,0x200,0); *s_bms_cfg &= ~0x800;  (clear bit 11)
```

It runs **after** the PB7 decision and reads **none** of the three trigger inputs
(it touches `s_bms_cfg` bits 9/11; the trigger reads `s_bms_cfg` bit **15**), so it
cannot change whether the fuse fires. Resolved (traced both bits):

- `s_bms_cfg` **bit 9** (`0x200`) — a separately set/consumed action latch
  (consumed+cleared in `bms_set_state` via `smbus_write_reg(10,0,0xff)`/`(0xB,0,0xff)`;
  also cleared by the charge-off helper `FUN_0800ae38`). 1.14.1's fuse tail cleared
  this bit (inside `FUN_0800ae38`).
- `s_bms_cfg` **bit 11** (`0x800`) — the **charge-permit flag**: set by
  `state_handler_07` / init, **read by the charge-FET gate** in `fg_coulomb_update`,
  `FUN_080063e0`, `state_handler_01` (it is *not* dead). 1.17.1's fuse tail clears
  this instead — i.e. the force-off now explicitly **revokes the charge permit**.

So the tail edit is a real (if minor) behavioural refinement of the force-off
cleanup — but it is downstream of the PB7 decision and touches neither `s_bms_cfg`
bit 15 nor any other trigger input, so **it cannot change whether the fuse fires**.
(`charge_mosfet_set` separately stopped maintaining `s_bms_cfg` bits `0x20000`/`0x200`
— see [charge.md](charge.md).)

## 3. The PH0 hardware interlock was removed from every dispatcher

This is the real change, and it is broader than just the fuse: in **1.14.1**, the
**whole protection + cell-balance machine** was gated on a **GPIOH PH0 input read**.
Every per-state dispatcher that can reach the fuse handler guarded each reach on
that read (and the fuse handler is only a subset of what the gate protected):

```c
// 1.14.1 — pattern in EVERY dispatcher (both fuse call sites)
if (gpio_bit_read(0x50001C00, 1) == 1 /* PH0 HIGH */ && <status/fault bits>) {
    ...
    FUN_08006524();   /* fuse handler */
}
```

`0x50001C00` is **GPIOH**; `gpio_bit_read(base, mask)` returns
`(*(base + 0x10 /*IDR*/) & mask) != 0`, so this reads input pin **PH0**. In
**1.17.1** that `gpio_bit_read(GPIOH,1)==1` test is **deleted** from all of them;
the fuse handler is reached on the status/fault bits alone.

Verified call sites (each dispatcher calls the fuse handler twice — a "block A"
`s_prot_status` bit-11 slot and a "block B" `g_fault_flags` bit 5/6/7 slot — both
lost the PH0 gate):

| 1.17.1 dispatcher | 1.14.1 counterpart | PH0 gate 1.14.1 | PH0 gate 1.17.1 |
| --- | --- | --- | --- |
| `bms_state_machine` `08002194` | `FUN_08002b70` | yes (both blocks) | **removed** |
| `state_timer_0b` `080010b4` | `FUN_08001138` | yes | **removed** |
| `state_timer_0c` `0800122c` | `FUN_0800138c` | yes | **removed** |
| `state_timer_0d` `08001bb4` | `FUN_080024fc` | yes | **removed** |
| `state_timer_0e` `08001db0` | `FUN_08002730` | yes | **removed** |
| `state_timer_12` `08001434` | `FUN_08001620` | yes | **removed** |
| `state_timer_13` `0800163c` | `FUN_080018b4` | yes | **removed** |
| `state_timer_14` `08001fa4` | `FUN_08002954` | yes | **removed** |
| `state_timer_15` `080019b8` | `FUN_080022c8` | yes | **removed** |
| `FUN_080063e0` | `FUN_080065e8` | yes | **removed** |
| `FUN_08006810` | `FUN_080077ec` | yes | **removed** |
| `FUN_0800699c` | `FUN_0800799c` | yes | **removed** |
| `FUN_08006b28` | `FUN_080065e8`* | yes | **removed** |
| `FUN_08006cb4` | `FUN_080065e8`* | yes | **removed** |
| `FUN_08006e40` | `FUN_080018b4`* | yes | **removed** |
| `FUN_0800a794` | `FUN_0800c574` | yes | **removed** |
| `FUN_0800a988` | `FUN_0800c784` | yes | **removed** |
| `can_transmit` `080055a8` | `FUN_080065e8` | yes (both blocks) | **removed** |

\* a few 1.14.1 counterparts in the `FUN_06xxx` family share dispatch shape so the
exact pairing is approximate, but the PH0-gate removal is uniform and verified on
each 1.17.1 function regardless of which twin it pairs to.

**Exception — unchanged:** `main_loop` (`080057b0`, 1.14.1 `FUN_0800681c`) reaches
the fuse handler when the requested-state byte is `0x17`/`0x18` (with `g_fault_flags`
bit6 for `0x17` and `s_bms_cfg` bit15 set just before) — this path was **never**
PH0-gated in *either* version, so it is identical.

GPIOH itself is still used in 1.17.1 (boot drives PH1 high; `0x50001C00` appears in
GPIO init/EXTI config), but there is **no PH0 read anywhere** — the interlock is
gone, not relocated.

## What PH0 is (traced exhaustively)

PH0 (GPIOH bit0) is a **digital INPUT**, never written. Both app versions configure
it **identically** — `gpio_pin_config(GPIOH, pin0, mode-word 0x10210000)` decodes to
**no-pull, EXTI-rising-capable input** (the bootloader leaves it a plain floating
input). The sibling **PH1** (bit1) is a push-pull **output driven HIGH** at boot in
all three images; firmware drives PH1 and reads PH0 but never correlates them, so a
PH1→PH0 loopback is possible but unproven.

In **1.14.1, PH0 is the BMS's system-enable / pack-active gate, polled pervasively**:
`gpio_bit_read(GPIOH, 1)` appears at the top of fault dispatch in a whole family of
~15–20 per-state handlers (and in the charge-balance helper `FUN_0800b364`).
Semantics are uniform: **PH0 HIGH ⇒ run protection / balancing; PH0 LOW** (held for a
debounce) ⇒ route into `FUN_08007730` = **sleep/shipping** (which sets `s_bms_cfg`
bit `0x800`). The fuse-handler reach is just one consumer of this global gate.

In **1.17.1, every GPIOH read is gone** — a byte-pattern sweep of the GPIOH base
literal (`00 1C 00 50`) finds **22 occurrences in 1.14.1** (most feeding
`gpio_bit_read`) vs **3 in 1.17.1, all non-read** (the init write/config + two
generic GPIO-library lookup-table slots). There is **no `gpio_bit_read` of GPIOH
anywhere in 1.17.1**. The per-tick enable gate that remains in 1.17.1 `main_loop`
reads **PB11** (GPIOB `0x800`, the mode-select button) + RAM status words instead.

So PH0 is a **system-enable/pack-active input** — *not* fuse-status feedback (the
fuse state lives only in the RAM words). This matches the "it's a function, not a
fuse line" reading: PH0 was the global "is the pack awake/enabled" interlock, and
the fuse-reachability change is a **side effect of deleting that whole gate**.

## The fuse's own inputs are set under unchanged logic

To be sure the trigger can't now be *armed* under different circumstances, all three
inputs were traced — none changed:

- **`s_prot_status` bit 11** (condition 1): **SET only** by the `fg_scan` watchdog
  (a measured value `< 2000` for a dwell counter `> 0x31`), **CLEARED only** by the
  whole-word `bms_init` wipe (power-cycle). Byte-identical in both versions. (Note
  bit 11 *set* routes to recoverable `0x19` and **suppresses** the fuse; bit 11
  *clear* is fuse-eligible.)
- **`g_fault_flags` bit 6 / bit 7** (condition 2): set by `fg_discharge_oc_check` /
  `fg_charge_oc_check`, which are **byte-identical** between versions.
- **`s_bms_cfg` bit 15** (condition 3): the same "update-busy" interlock in both.

## What it means

- In **1.14.1**, a latched hard over-current (bit11 clear, bit15 clear) reached the
  fuse via a dispatcher only while **PH0 was HIGH**; with PH0 low the BMS went to
  **sleep** instead of dispatching protection, so only the never-gated `main_loop`
  state-0x17/0x18 path could reach the fuse.
- In **1.17.1**, the dispatchers reach the fuse handler **regardless of PH0** (PH0 is
  no longer read at all), so the fuse can fire whenever the three (unchanged) entry
  conditions hold.

**Net:** the *firing rule* is identical; what changed is that a hardware
"pack-enabled" precondition on *running protection at all* was removed. The
real-world effect depends on what PH0 physically is and when it is low — two readings
remain, both consistent with the code (and the firmware cannot decide between them):

- PH0 was a genuine **enable/interlock** (only protect while awake/enabled) →
  1.17.1 **relaxes** it, so the fuse can fire in states 1.14.1 would have slept
  through.
- PH0 was a strap that, when low in the field, **wrongly suppressed protection** (and
  could have stopped the fuse on a real MOS failure) → 1.17.1 **fixes** that by always
  protecting.

The schematic net for PH0 (and whether 1.17.1's PB11 gate is its replacement) is the
one thing the binaries can't tell us.

## How verified

Both fuse-handler bodies decompiled and diffed; every call site in both images
enumerated via xrefs and decompiled; each finding re-checked by an independent
pass that discarded relocation/`DATA_EXT`/register/stack artifacts and confirmed
the PH0 read (`gpio_bit_read(0x50001C00,1)`) literal (`DAT` → `0x50001C00`) at each
1.14.1 site and its absence in 1.17.1.
