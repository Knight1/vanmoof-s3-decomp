# ⚠ Off-road region removed from BLE — 1.07.06 → 1.08.02

**Behaviourally and regulatorily the most significant change in this release.**
The BLE region/speed-set path (`ble_cmd_dispatch`, cmd `0x5535` region+speed /
`0x553C` region-lock) **deleted the off-road option**: the rejection guard and its
log string are gone, the off-road region code is no longer selectable, and the
legacy off-road sentinel is silently folded into EU. After 1.08.02 the bike cannot
be put into off-road (speed-cap-lifted) mode from the app/BLE surface.

- before: `ble_cmd_dispatch` `@0x08033970` (off-road reject at `0x08033F74`–`0x08033F80`)
- after: `FUN_0803309C` `@0x0803309C` (region-set sub-handler at `0x080335EE`)
- `diff_functions` similarity **0.57**; `strings_only_in_a` = `["Region 'off road' is not allowed"]`, `strings_only_in_b` = `[]`.

(Note: the *console* `region` command `console_region_set` is **byte-identical**
between versions — this change is purely in the **BLE** region path. The
`Region (off road disabled): ` console string survives there.)

## 1) The reject message and its branch are excised

1.07.06 dispatched on the region-lock mode byte `ctx+0x144`:

```c
if (ctx[0x144] == 1) {                 /* off-road-locked mode */
    if (region != 0xFF) { ... }
    g_log_func("Region 'off road' is not allowed\r\n");   /* str @0x08051A3C */
    return;
} else if (ctx[0x144] == 2) {
    g_log_func("Region is locked\r\n");                    /* str @0x08051A60 */
    ...
}
```

1.08.02 keeps **only** the `== 2` ("Region is locked", str `@0x08052B54`) arm. The
`cmp #1 / beq off-road-handler` dispatch and the whole `cmp region,#0xFF / bne /
print / return` reject block are removed (`diff_functions` marks them as removed
instructions). The `Region 'off road' is not allowed` string is gone from the
image entirely.

## 2) Off-road region code remapped (load-bearing)

When the requested region byte is the off-road sentinel `0xFF`:

| | 1.07.06 | 1.08.02 |
| --- | --- | --- |
| stored region | `movs r2,#3; strb r2,[ctx+0x109]` → **REGION_OFFROAD (3)** | `movs r2,#0; strb r2,[ctx+0x109]` → **REGION_EU (0)** |

`diff_functions` shows exactly `{removed: "movs r2,#3"}` → `{added: "movs r2,#0"}`
immediately before the `strb [ctx+0x109]`. So an app request that previously
selected off-road is now coerced to EU.

## 3) Valid-region range tightened

| | 1.07.06 | 1.08.02 |
| --- | --- | --- |
| accept test | `subs r3,region,#4; uxtb; cmp r3,#0xFA; bls → "Invalid region"` (accepts 0,1,2,3 + the 0xFF special-case) | `cmp region,#2; bhi → "Invalid region"` (accepts only 0,1,2 + the 0xFF special-case) |

So **region code 3 (`REGION_OFFROAD`) now fails the range check** and hits
`Invalid region` (string moved `0x08051A0C` → `0x08052B24`); EU/US/JP (0/1/2)
remain valid.

## Net effect

Off-road is **de-listed from the BLE control surface** in 1.08.02: the option, its
"not allowed" guard + message, and its selectable region code (3) were all removed,
and the `0xFF` off-road sentinel now resolves to **EU** instead of off-road. This
is consistent with a regulatory tightening (off-road lifts the e-bike speed cap).
