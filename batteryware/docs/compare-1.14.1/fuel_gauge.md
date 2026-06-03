# fuel_gauge.c — 1.14.1 → 1.17.1

Per-function before/after for the changed fuel-gauge functions. The 1.17.1 side
is reconstructed in [`../../src/fuel_gauge.c`](../../src/fuel_gauge.c).

---

## `fg_watchdog_kick`

| | |
| --- | --- |
| 1.17.1 | `0x080031d8` (54 instr) — `src/fuel_gauge.c:533` |
| 1.14.1 | `0x08003be4` (61 instr) |
| Verdict | **logic-change** (high) |

**What changed.** Two things in the AFE-watchdog kick:

1. The recovery path no longer calls **`bms_configure(saved)`** after
   `bms_init()` (and the load of the saved register byte that fed it is gone too).
2. The `*watchdog = 0; smbus_write_reg(8, 0x91, 0xff)` block changed from an
   **unconditional tail** (1.14.1 fell through into it on every kick once the bus
   was ready) to a branch that runs **only when the recovery path is not taken** —
   the two are now mutually exclusive.

```c
// 1.14.1 (FUN_08003be4)
if (bus_ready && !( (reg[2]&0xf)==0xf && (reg[3]&2)==2 )) {   // fault not latched
    saved = *0x........;          // load saved config value
    delay_ms(5); bms_init(); delay_ms(5);
    bms_configure(saved);         // <-- re-apply saved config
}
*watchdog = 0;                    // falls through: runs on EVERY kick (bus ready)
smbus_write_reg(8, 0x91, 0xff);

// 1.17.1 (fg_watchdog_kick)
if (smbus_read(1,2)==0 || ( (reg[2]&0xf)==0xf && (reg[3]&2)==2 )) {  // fault path
    *watchdog = 0;
    smbus_write_reg(8, 0x91, 0xff);   // now only on this branch
} else {
    delay_ms(5); bms_init(); delay_ms(5);   // NO saved load, NO bms_configure
}
```

- **Removed call:** `bms_configure(saved)` (verified `0x08005dfc` writes the saved
  value via `smbus_write_reg(9, saved, …)`), plus its saved-value load.
- **Control flow:** clear+write moved out of the fall-through tail into the
  non-recovery branch. The underlying fault predicate
  `((reg[2]&0xf)==0xf && (reg[3]&2)==2)` is unchanged; the apparent polarity flip
  is just fall-through vs. explicit branch.
- Unchanged: `bus_ready_check`, `smbus_read(1,2)`, `smbus_write_reg(8,0x91,0xff)`,
  `delay_ms(5)`, `bms_init()` — same register numbers/commands.

> (Note: `bms_init` itself also changed — see [bms.md](bms.md#bms_init) — but that
> is inside the callee, not this function.)

---

## `fg_read_loop`

| | |
| --- | --- |
| 1.17.1 | `0x08004634` (79 instr) — `src/fuel_gauge.c:621` |
| 1.14.1 | `0x08005028` (71 instr) |
| Verdict | **logic-change** (high) |

**What changed.** A new `if (index == 9)` special case redirects that one cell's
value to a dedicated scalar global (`0x20002824`) instead of writing it into the
cell-voltage array slot. All other indices still write the array.

```c
// 1.14.1 — unconditional array store
cell_array[index] = value;        // base @0x20000704
index++;

// 1.17.1 — index 9 mirrored to a dedicated global
if (index == 9) *((u32*)0x20002824) = value;   // NEW
else            cell_array[index]   = value;    // base @0x20002830
index++;
```

- **Added:** `if (index == 9) → store to 0x20002824` (distinct from the array slot
  `array_base + 9*4`).
- Unchanged: entry gate `*reg&8`, `smbus_read(0x34, 2)`, ×19536 scale (`0x4c50`),
  `/1000`, the 16-element loop, jumptable dispatch, overflow `smbus_write_reg(8,0x91,0xff)`.

This is the same "cell index 9 → `0x20002824`" mirror that also appears in the
coulomb tail (below).

---

## `cell_balance_update`

| | |
| --- | --- |
| 1.17.1 | `0x08000880` (794 instr) — `src/fuel_gauge.c:931` |
| 1.14.1 | `0x0800083c` (788 instr) |
| Verdict | **logic-change** (high) |

**What changed.** 1.17.1 wraps the **entire** cell-balancing routine in a new
outer precondition: a RAM word at `0x200024f4` must equal the fixed constant
`0x40012400`, otherwise the function returns immediately and does no balancing.
1.14.1 has no such gate. Every inner balancing step is otherwise identical.

```c
// 1.14.1 (FUN_0800083c) — no gate
void cell_balance_update(void) {
    counter = (*p + 1); if (counter > 99) counter = 0;   // runs unconditionally
    ... balancing matrix / averaging / threshold updates ...
}

// 1.17.1 (cell_balance_update) — guarded by a magic word
void cell_balance_update(void) {
    if (*(u32*)0x200024f4 == 0x40012400) {               // NEW outer gate
        counter = (*p + 1); if (counter > 99) counter = 0;
        ... same balancing body ...
    }
}
```

- **Added:** outer gate `*(0x200024f4) == 0x40012400`. Verified disasm
  `0x08000886-0x08000890`: pointer literal = `0x200024f4`, compared constant =
  `0x40012400`.
- Body is instruction-for-instruction identical: 99-count divider, `==0` →
  `gpio_bit_write(…,0x8000,1)`, `==10` → `dma_stop`, debounced bit-0 clear, the
  5×3 matrix fill with the `0x91` LUT search, `/5` column averaging via
  `__aeabi_uidivmod`, signed offset corrections, and the `0x44/0x45/0x46`
  threshold updates (each 5-count debounced before `memcmp_verify`).

> `0x40012400` reads like an enable/identity key (it is *not* a RAM address in
> this part) — balancing is now gated on that word being set.

---

## `fg_scan` + `fg_coulomb_update`

| | |
| --- | --- |
| 1.17.1 | `fg_scan` `0x0800325c` (639) — `src/fuel_gauge.c:664`; `fg_coulomb_update` `0x080039c2` (1394) — `src/fuel_gauge.c:758` |
| 1.14.1 | both are halves of **one** function `FUN_08003c7c` `0x08003c7c` (2082 instr) |
| Verdict | **refactor-split** + **logic-change** (high) |

**Structural change (the split).** In 1.14.1 the alert/threshold-scan logic and
the coulomb/SOC/temperature-protection logic lived in a single 2082-instruction
function (`FUN_08003c7c`, sitting right after `fg_watchdog_kick`'s counterpart).
1.17.1 splits it: `fg_scan` keeps the front (scan) half and `fg_coulomb_update`
becomes the extracted back (coulomb/protection) half.

At the seam, `fg_scan` now **calls** `fg_coulomb_update()` where the old code
held that block inline, and the threshold-scan block is **no longer gated** by
the `flags & 1` test (it ran only under that guard in 1.14.1):

```c
// 1.14.1 (inside FUN_08003c7c)
if ((flags & 1) != 0) {
    if (state < 0xf) { jumptable[state](); return; }
    ... 5 threshold scans + 0xf-entry discharge loop ...
}
if ((flags & 2) == 0) { /* coulomb body, inline */ }

// 1.17.1 (fg_scan)
if ((flags & 1) == 0) { fg_coulomb_update(); }      // seam: call the split-out half
if (state < 0xf) { jumptable[state](); return; }    // now UNCONDITIONAL (guard removed)
... 5 threshold scans + discharge loop ...           // no longer under flags&1
```

**Logic changes (in the coulomb / temperature-protection half).** Beyond the
split, the temperature-protection path was retuned:

1. **First temperature gate widened** with two new OR terms:
   ```c
   // 1.14.1
   if (cur < reg[0x16] || level <= limit) { ...clear-side... }
   // 1.17.1 — two extra OR terms (status bits 10 and 9)
   if (cur < reg[0x16] || level <= limit
       || ((flags & 0x7ff) >> 10 != 0)      // NEW bit10
       || ((flags & 0x3ff) >>  9 != 0)) {   // NEW bit9
       ...clear-side...
   ```
2. **Flag auto-clear masks for bits 8/9/10 removed** on the temperature-OK
   (`else`) path — 1.14.1 also `AND`ed the flags word with `~0x400` (bit10),
   `~0x100` (bit8), `~0x200` (bit9); 1.17.1 clears only bits 6/7 (`& 0xffbf`,
   `& 0xff7f`). So those flags now latch instead of being auto-cleared (the logical
   complement of the two new OR terms above).
3. **Reg-`0x68` clear-side sub-scan removed.** 1.14.1 had a bit-8 clear path that
   read FEDL5236 register offset `0x68` under a retry counter (`*0x..5158 < 3`);
   1.17.1 no longer references reg `0x68` or that retry path.
4. **Cell-current tail gains the index-9 mirror** (same as `fg_read_loop`):
   ```c
   if (index == 9) *((u32*)0x20002824) = value;   // NEW
   else            cell_array[index]   = value;
   ```

- Unchanged: coulomb math (`smbus_read(0x2e,2)`, `__aeabi_lmul`, 4-sample `avg>>2`),
  SOC integration, the five threshold scans on reg offsets `0x2a–0x48`, the
  jumptable dispatch and discharge loop themselves.

> Front/back boundary between `fg_scan` and `fg_coulomb_update` is approximate
> (it was one function); the protection-logic deltas above belong to the
> coulomb/temperature half and were verified against both the 1.14.1 combined
> body and the two 1.17.1 halves.

---

## `fg_charge_status`

| | |
| --- | --- |
| 1.17.1 | `0x0800997c` (64 instr) — `src/fuel_gauge.c:439` |
| 1.14.1 | inline block in `FUN_0800d004` (the UART report cascade), record index 8 |
| Verdict | **logic-change** (high) |

**What changed.** The charge-status computation was extracted from the inline
report cascade into its own function **and** lost one clause: in the `status==3`
branch, 1.14.1 OR-ed bit 1 into the result when a flag byte's bit 1 was *not* set;
1.17.1 drops that entirely (the `status==3` result is just the threshold compare).

```c
// 1.14.1 (status==3 branch, inside FUN_0800d004)
result = (reg[0x16] <= threshold);          // struct+0x16 vs threshold (0x4e1f = 20015)
if ((*flag & 2) != 2) result |= 2;           // <-- conditional OR bit1 — REMOVED in 1.17
// then: if (level > 0x4e1f) result |= 4;

// 1.17.1 (fg_charge_status)
if (status == 3) { result = (reg[0x16] <= threshold); goto level_check; }  // no flag&2 clause
// then: if (level > 0x4e1f) result |= 4;
```

- **Removed:** the `status==3 && !(flag&2) → result |= 2` clause.
- Unchanged: enum dispatch (`==2 →1`, `>=7 →2`, `==3 →` threshold compare,
  else `0`), struct offset `0x16`, threshold `0x4e1f` (=20015), final
  `level > 0x4e1f → result |= 4`.
- The `flag & 2` meaning is now surfaced separately via `fg_status_flag2_get`.

---

## `fg_status_flag_get` + `fg_status_flag2_get`

| | |
| --- | --- |
| 1.17.1 | `fg_status_flag_get` `0x08009a10` (23) — `src/fuel_gauge.c:76`; `fg_status_flag2_get` `0x08009a44` (27) — `src/fuel_gauge.c:84` |
| 1.14.1 | inline blocks in `FUN_0800d004` (report record indices 9 and 0x1b) |
| Verdict | **refactor-split** (high) — no semantic delta |

**What changed.** Purely structural: two flag-byte tests that 1.14.1 inlined into
the UART report cascade became standalone boolean accessors in 1.17.1 (now called
by `bms_set_state`). The logic is identical.

```c
// 1.14.1: pushed into the report buffer inline
report_field(0, (*flag & 1) == 1);   // record index 9
report_field(0, (*flag & 2) == 2);   // record index 0x1b

// 1.17.1: standalone accessors
bool fg_status_flag_get(void)  { return (*flag & 1) == 1; }
bool fg_status_flag2_get(void) { return (*flag & 2) == 2; }
```

- Same flag global (1.14.1 `0x20000744` → 1.17.1 `0x20002870`, relocation), same
  masks (`0x01`, `0x02`), same `==` tests.
- Note the link to `fg_charge_status`: in 1.14.1 the `flag & 2` bit was *also*
  consumed inside the charge-status computation; that secondary use was dropped,
  leaving `flag & 2` reported only through `fg_status_flag2_get`.
