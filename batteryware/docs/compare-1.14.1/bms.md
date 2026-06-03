# bms_init.c / state_handlers.c — 1.14.1 → 1.17.1

---

## `bms_init`

| | |
| --- | --- |
| 1.17.1 | `0x08004d04` (389 instr) — `src/bms_init.c:93` |
| 1.14.1 | `0x08005720` (412 instr) |
| Verdict | **logic-change** (high) |

**What changed.** Two things in the FEDL5236 bring-up:

1. **FEDL5236 register `0x0e` init value `0x0a → 0x9a`** (the `smbus_write_reg(0x0e, …)`
   in the default-register block). Bit 7 is now set in addition to the low nibble.
2. **6 of 7 `uart_printf` debug calls removed.** 1.14.1 sprinkled 7 `uart_printf`
   calls through init; 1.17.1 keeps only the function-entry banner. Each removed
   print is replaced by nothing (the `uart_tx_flush` calls remain).

```c
// register write — value change
smbus_write_reg(0x0e, 0x0a, 0xff);   // 1.14.1
smbus_write_reg(0x0e, 0x9a, 0xff);   // 1.17.1

// debug prints (1.14.1 had 7; 1.17.1 has 1)
//  #1 entry banner ............................. KEPT
//  #2 after reg 0/9 writes ...................... REMOVED  (1.14 @08005758)
//  #3 before first poll loop .................... REMOVED  (1.14 @080057e2)
//  #4 inside first poll loop status-clear ....... REMOVED  (1.14 @080058d8)
//  #5 before second poll loop ................... REMOVED  (1.14 @080058fa)
//  #6 inside second poll loop status-clear ...... REMOVED  (1.14 @08005a86)
//  #7 after second loop (took loop-exit value) .. REMOVED  (1.14 @08005aac)
```

- **Changed:** reg `0x0e` value `0x0a → 0x9a`.
- **Removed:** 6 `uart_printf` calls (status-clear branches now just clear the
  `0x20` bit via `bics #0x20`; the trailing prints become bare `uart_tx_flush`).
- Unchanged: the full `smbus_write_reg` register sequence
  (`0,0,9,10,0xb,0xc,0xe,0x10,0x11,0xf,1,2,3,4,8`), both two-stage poll loops
  (reg 6=`0x92` / reg 5=`0x99`, polling reg 3, then reg `0x2e` mask 2 / reg `0x1a`
  mask 1 with the cell-scale loop), status-bit handling (`0x3f>>5`, clear `0x20`),
  and the 7 trailing global zeroings.

---

## `bms_state_machine`

| | |
| --- | --- |
| 1.17.1 | `0x08002194` (1107 instr) — `src/state_handlers.c:970` |
| 1.14.1 | `0x08002b70` (1121 instr) |
| Verdict | **logic-change** (high) |

**What changed.** Three behavioural deltas in the 28-state dispatcher:

1. **Host/comms enable gate removed.** 1.14.1 wrapped both protection-dispatch
   blocks (and a standalone finalizer) in `FUN_08011384(ctx, 1)`, which returns
   `(*(u32*)(ctx+0x10) & 1) != 0` — a "host/comms enabled" check. All three calls
   are gone in 1.17.1; protection dispatch now runs purely on its own status-register
   bits, with no enable precondition.
2. **Fault-recovery bit-4 set→clear.** On entry to fault-recovery states `0xb` and
   `0xc`, 1.14.1 did `*reg |= 0x10`; 1.17.1 does `*reg &= ~0x10` — a set→clear
   inversion of bit 4 (the adjacent `*reg |= 0x80` is unchanged).
3. **State-4 recharge timeout widened.** The recharge control counter changed from
   8-bit (`ldrb/strb`) to 16-bit (`ldrh/strh`), and its middle timeout threshold
   was raised `0x31 → 0x12c` — i.e. fires at **≥300 ticks** instead of **≥50**.

```c
// 1.14.1
if (FUN_08011384(ctx,1)==1 && ((*s & 0x3f)>>5)!=0) { ...protection handlers... }
...
if (FUN_08011384(ctx,1)==1 && *s2 != 0)            { ...handlers 14/15/16/17_19... }
...
if (FUN_08011384(ctx,1)!=1) { finalizer(); }
*reg |= 0x10;  ... bms_set_state(0xb /* or 0xc */);          // bit-4 SET
cnt8 = *p + 1; *p = cnt8;  if (cnt8 > 0x31) { *p=0; charge_mosfet_set(0); }   // >=50, byte

// 1.17.1 — enable gate gone, bit-4 cleared, 16-bit counter, threshold 300
if (((*s & 0x3f)>>5)!=0) { ...protection handlers... }     // unconditional
...
if (*s2 != 0)            { ...handlers 14/15/16/17_19... }  // unconditional
...
*reg &= ~0x10; ... bms_set_state(0xb /* or 0xc */);          // bit-4 CLEAR
cnt16 = *p + 1; *p = cnt16; if (cnt16 > 0x12c) { *p=0; charge_mosfet_set(0); } // >=300, halfword
```

- **Removed:** all 3 `FUN_08011384(ctx,1)` enable gates (incl. the
  `if (!enabled) finalizer()` branch).
- **Inverted:** bit-4 handling on state `0xb`/`0xc` entry (`|0x10` → `&~0x10`).
- **Retuned:** state-4 recharge counter byte→halfword, timeout `50 → 300` ticks.
- The other two state-4 thresholds (`0x13`=19, `9`) are unchanged.

---

## `bms_set_state`

| | |
| --- | --- |
| 1.17.1 | `0x08005b34` (395 instr) — `src/state_handlers.c` |
| 1.14.1 | `0x080069cc` (369 instr) |
| Verdict | **logic-change** (high) |

**What changed.** The per-transition telemetry/history record was **widened
`0x30 → 0x38` bytes** and gained three fuel-gauge status fields, plus two new tail
side-effects. (This is the change first spotted at the release level; verified
here in detail.)

```c
// 1.14.1 (FUN_080069cc): two trailing fields are BYTES; record size 0x30
rec[+0x2d] = src0;  rec[+0x2e] = src1;            // strb / strb
store(history,            0x30, rec);             // size 0x30
store(ring + idx*0x30,    0x30, rec);             // stride 0x30
fg_clear_status();
return;                                           // no +5 write, no printf

// 1.17.1 (bms_set_state): widened fields + 3 new fg-status halfwords; record size 0x38
rec[+0x2e] = src0;  rec[+0x30] = src1;            // now strh / strh (widened, shifted)
rec[+0x32] = fg_charge_status();                  // NEW halfword
rec[+0x34] = fg_status_flag_get();                // NEW halfword
rec[+0x36] = fg_status_flag2_get();               // NEW halfword
store(history,            0x38, rec);             // size 0x38
store(ring + idx*0x38,    0x38, rec);             // stride 0x38
fg_clear_status();
*((u8*)status + 5) = 1;                            // NEW status byte @+5
uart_printf(...);                                  // NEW per-transition print
return;
```

- **Record size & ring stride `0x30 → 0x38`** (all stores; verified at the
  `movs r1,#0x30 → #0x38` immediates and the stride math `(2x+x)<<4 → (8x−x)<<3`).
- **Two pre-existing trailing fields widened byte→halfword** and shifted
  `+0x2d/+0x2e → +0x2e/+0x30`.
- **Three new halfword fields** appended at `+0x32/+0x34/+0x36`:
  `fg_charge_status()`, `fg_status_flag_get()`, `fg_status_flag2_get()` — none of
  these calls exist in 1.14.1 (and they are exactly the accessors that were
  refactored out of the old report cascade — see
  [fuel_gauge.md](fuel_gauge.md#fg_charge_status)).
- **New tail side-effects:** `*(status+5) = 1` and a per-transition
  `uart_printf(...)`.
- Unchanged: `smbus_read(10,2)`, the bit-9 fault clear, the bit-15 gate, the
  `0x1a`/`0x7` jumptable dispatch, the `/100` ring index, ring wrap.
