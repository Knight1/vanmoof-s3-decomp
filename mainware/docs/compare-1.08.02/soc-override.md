# Manual / custom SOC override — new in 1.09.01

1.09.01 adds a manual state-of-charge override: two new console commands that force
the displayed SOC, applied by overwriting the BMS-reported RSOC inside the Modbus
read path. None of this exists in 1.08.02 (which only had generic SOC strings like
`SOC %d saved %d`, `RSOC %d`).

**New strings:** ` >> Warning manual SOC is set <<`, `Custom SOC set to %d`,
`Custom SOC     %d`, `Custom SOC: %d`, `Manual BMS SOC %d`, ` Overrule SOC %d`,
`Out of range use 10..100`, `manual SOC %d (-1 to disable)`.

Struct base pointer `*DAT_080376E4` = `0x200034A0` → the BMS state struct (RAM base
`0x20000A00`); the logger fn-ptr table is at `0x2000348C`. The BMS Modbus holding-
register array is at `struct+0x402` (SOC = Modbus register index 5 → `struct+0x40C`).

## Console commands (table @ `0x0804E284`, 12-byte `{name, help, handler}` records)

### `customsoc` → `FUN_08037674`
```
v = strtol(arg, 0, 10);            /* sign-extended to short */
if (v >= 0x65) { print "Out of range use 10..100"; return; }   /* <= 100 */
state[0x146] = (uint8_t)v;          /* custom-SOC byte */
print "Custom SOC set to %d", v;
```
With no argument it prints the current `Custom SOC     %d` from `+0x146`.

### `overrule soc` → `FUN_080380F8`
```
v = strtol(arg, 0, 10);            /* SIGN-extended to signed byte (sxtb); -1 = disable */
state[0x332]  = (int8_t)v;          /* manual-override field */
state[0x40C]  = (short)v;           /* mirror into the live SOC halfword */
print "Manual BMS SOC %d", state[0x40C];
print "manual SOC %d (-1 to disable)", (int8_t)state[0x332];
```
With no argument it prints the current `+0x40C` / `+0x332` values.

## How the override takes effect — `FUN_08031B98` (in the BMS Modbus parser `FUN_08032748`)

When a Modbus **function-3 "read holding registers"** response from the BMS completes
(register-type 3), `FUN_08031B98` copies the received register block into
`struct+0x402`. For the **SOC register (index 5 → `struct+0x40C`)** it checks the
manual-override byte:

```
if (state[0x332] != -1) {                 /* override active */
    state[0x40C] = (short)(int8_t)state[0x332];   /* replace the real BMS RSOC */
    print " Overrule SOC %d", state[0x40C];
}
state[0x315] = state[0x40C];               /* also mirrored to +0x315 */
```

So while `+0x332 != -1`, the manual value **silently replaces the genuine BMS-reported
RSOC** for every downstream consumer; `-1` disables and lets the real value through. A
separate status path prints ` >> Warning manual SOC is set <<` while the override is
active.

## Summary of offsets

| Field | Offset | Type | Set by |
| --- | --- | --- | --- |
| custom SOC | `+0x146` | u8 | `customsoc` (10..100) |
| manual-override | `+0x332` | s8 (`-1`=off) | `overrule soc` |
| live BMS RSOC | `+0x40C` (also `+0x315`) | s16 | BMS Modbus reg 5, overwritten when override active |

This is a **new dedicated override path**, not a repurpose of the old `Set SOC` /
gear command (which 1.08.02 had turned into a shifter driver).
