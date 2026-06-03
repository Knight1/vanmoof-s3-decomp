# charge.c — 1.14.1 → 1.17.1

## `charge_mosfet_set`

| | |
| --- | --- |
| 1.17.1 | `0x08002d50` (51 instr) — `src/charge.c:12` |
| 1.14.1 | `0x08003774` (41 instr) |
| Verdict | **logic-change** (high confidence) |

**What changed.** 1.14.1's `charge_mosfet_set` calls two helpers
(`FUN_0800ae0c` / `FUN_0800ae38`) that, besides driving the charge-MOSFET GPIO
pin, also maintained two extra bits in the charge-MOSFET status word (the same
global `0x20000a84`): they **set** bit `0x20000` on enable and **cleared** bit
`0x200` on disable. 1.17.1 inlines just the `gpio_bit_write` and **drops both of
those secondary status-word bit operations** — only the bit-`0x40` flag update
and the pin-`0x200` GPIO write remain.

```c
// 1.14.1 (FUN_08003774) — g = charge-MOSFET status word @0x20000a84
if (on) {
    if (((*g & 0x7f) >> 6) == 0) { *g |= 0x40; FUN_0800ae0c(); }   // helper also: *g |= 0x20000
} else if (((*g & 0x7f) >> 6) != 0) { *g &= ~0x40; FUN_0800ae38(); } // helper also: *g &= ~0x200
//   FUN_0800ae0c:  *g |= 0x20000;     gpio_bit_write(GPIOB, 0x200, 1);
//   FUN_0800ae38:  *g &= ~0x200;      gpio_bit_write(GPIOB, 0x200, 0);

// 1.17.1 (charge_mosfet_set) — GPIO write inlined, the extra bit-ops are gone
if (on) {
    if (((*g & 0x7f) >> 6) == 0) { *g |= 0x40;  gpio_bit_write(GPIOB, 0x200, 1); }  // no *g |= 0x20000
} else if (((*g & 0x7f) >> 6) != 0) { *g &= ~0x40; gpio_bit_write(GPIOB, 0x200, 0); } // no *g &= ~0x200
```

- **Removed (enable):** `*status |= 0x20000`.
- **Removed (disable):** `*status &= ~0x200`.
- Preserved & identical: the `(*status & 0x7f) >> 6` guard, the bit-`0x40`
  set/clear, GPIO base `0x50000400` (GPIOB) pin mask `0x200`, write value 1/0.

**Verification notes.** In 1.14.1 the three literals feeding the status word
(`DAT_080037cc`, `DAT_0800ae30`, `DAT_0800ae5c`) all resolve to `0x20000a84`, so
the helpers genuinely applied those asymmetric bit ops to the charge-MOSFET
status word. Excluded as non-changes: the status global relocating
`0x20000a84 → 0x20002c00`, the differing `gpio_bit_write` call address
(`0x080113be → 0x0800fcde`, same behaviour), and the source-level if/else branch
ordering (identical machine semantics).
