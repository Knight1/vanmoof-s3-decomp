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

## What arms the fuse: over-current only — **not** over/under-voltage or threshold

Condition (2) reads **only `g_fault_flags` bit 6 / bit 7**. That word is the unified
protection-fault register; each protection check latches a *distinct* bit (all six
`fg_*_check` functions are byte-identical in both versions, and all OR into the same
word `0x20002c44`):

| `g_fault_flags` bit | set by | fault |
| --- | --- | --- |
| 0 (`0x01`) | `fg_uvp1_check` | cell under-voltage 1 |
| 1 (`0x02`) | `fg_uvp2_check` | cell under-voltage 2 |
| 2 (`0x04`) | `fg_ovp1_check` | **cell over-voltage 1** |
| 3 (`0x08`) | `fg_ovp2_check` | **cell over-voltage 2** |
| 4 (`0x10`) | `fg_threshold_check` | threshold guard |
| 6 (`0x40`) | `fg_discharge_oc_check` | discharge over-current |
| 7 (`0x80`) | `fg_charge_oc_check` | charge over-current |

So the heater fires **only** on a latched **discharge-OC (bit 6) or charge-OC
(bit 7)**. Over-voltage sets bits 2/3, under-voltage sets bits 0/1, threshold sets
bit 4 — the fuse handler reads **none** of them. Only the two OC checks additionally
stamp a trip-reason byte at `0x200029e0` (1 = dischg-OC, 2 = chg-OC); the
voltage/threshold checks don't.

**Where over/under-voltage actually go.** Both per-state dispatchers and
`bms_state_machine` route the non-OC bits to *recoverable* handlers, never to the
heater branch:

| fault | routed to | what that handler does |
| --- | --- | --- |
| OVP1 (`g_fault_flags` bit 2) | `state_handler_14` | `charge_mosfet_off()` + `bms_configure(0)` + recoverable state `0x14` |
| OVP2 (`g_fault_flags` bit 3) | `state_handler_15` | same → state `0x15` |
| threshold (`g_fault_flags` bit 4) | `state_handler_16` | same → state `0x16` |
| OVP via `s_prot_status` bit 2/3 | `state_handler_09` / `0a` | `charge_mosfet_off()` + `bms_configure(2)` + recoverable state `9`/`10` |
| OC region (`g_fault_flags` bit 5/6/7) | `state_handler_17_19` | the fuse handler — fires PB7 **only** if bit 6/7 |

`state_handler_09`/`0a`/`14`/`15`/`16` write only **PB0** (`0x1`) and **PB9**
(`0x200`) — **never PB7** (`0x80`). Over-voltage's firmware response is to **open the
charge FET and re-arm the AFE** (`bms_configure` re-pushes SMBus protection regs 3–9),
then sit in a recoverable state. No pyro fuse.

**Can over-voltage reach the fuse at all? Only indirectly.** Nothing other than the
two OC checks ever *sets* `g_fault_flags` bit 6/7 — verified: `bms_state_machine`'s
only `orrs` into that word touch bits 0/1/8/9, never 6/7. An over-voltage event where
the charge FET successfully opens stays recoverable. The fuse becomes reachable only
if the charge FET **fails shorted** — then charge current keeps flowing, the
**charge-OC** debounce trips bit 7, and *that* arms the heater. This is the design
intent: **the pyro fuse is the "FET welded / current can't be interrupted normally"
last resort, detected as a persistent over-current — not a direct over-voltage trip.**

**Hardware layer — over-voltage *does* blow the fuse, in hardware.** This is only the
*MCU firmware's* fuse logic; PB7 is an MCU GPIO. The board pairs it with a dedicated
**secondary over-voltage protection** stage: two **`S-8215AAD-K8T2U`** cell-overcharge
ICs (`U1005` = cells 1–5, `U1006` = cells 6–10) that, on **any cell > 4.35 V for 2 s**,
autonomously drive the **same** SCF9550 fuse heater via a shared gate node (JN2) —
independent of the FEDL5236 AFE and the MCU. So the complete picture is: **MCU/PB7
blows the fuse on persistent over-current; the S-8215AAD ICs blow it on over-voltage.**
The firmware half (this document) is unchanged between 1.14.1 and 1.17.1. Full
circuit: [`../hardware.md` → "Secondary protection & the pyro fuse"](../hardware.md#secondary-protection--the-pyro-fuse).

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
