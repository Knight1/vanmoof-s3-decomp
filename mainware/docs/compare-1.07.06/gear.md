# The `gear` console command — 1.07.06 → 1.08.02

The debug-console command **`gear`** (name string + help "set gear") is bound in
the 49-entry command table in both versions, but its handler was **completely
repurposed** — from a hidden SOC-override into a real shifter driver. The SOC-set
code path was deleted.

Command-table mapping (certain): 1.07.06 entry `0x0804F60C` maps `"set gear"`
(`0x0805501C`) → handler `console_soc_set` `@0x080425AC` (23 instr); 1.08.02 entry
`0x08044B58` maps `"set gear"` (`0x080524DC`) → handler `FUN_08031790`
`@0x08031790` (54 instr). Same calling shape (tokenise → `strtol` base-10 → ctx
deref at `+0x2DC`). `diff_functions` similarity **0.142** (17 equal / 35 added /
4 removed).

## Before (1.07.06 `console_soc_set`)

```c
/* the "gear" command was a disguised SOC override */
if (console_next_token(&tok)) {
    int v = strtol(tok, NULL, 10);
    *(uint8_t *)(ctx + 0x3D4) = (uint8_t)v;     /* SOC override byte */
    g_log_func("Set SOC %d\r\n", (uint16_t)v);
    announce_mark(2);
}
```

No gear, no Modbus, no shifter power, no reset.

## After (1.08.02 `FUN_08031790`)

```c
gpio_write(GPIOB_BASE, 1<<14, 1);               /* PB14 BSRR set — assert shifter power */
tok = next_token(...);
int v = strtol(tok, 0, 10);
if ((uint16_t)(v - 1) < 4) {                    /* gears 1..4 */
    ++*(uint8_t *)(ctx + 0x338);                /* shift counter */
    shifter_send_gear((char)v);                 /* Modbus: slave 0x20, func 6, reg 2, val=v */
} else if (v == 0) {                            /* NEW */
    g_log_func("Shifter off\r\n");
    gpio_write(GPIOB_BASE, 1<<14, 0);           /* PB14 BSRR reset — power shifter OFF */
} else if (v == 99) {                           /* NEW */
    g_log_func("Reset\r\n");
    do { } while (shifter_reset_sm_step() == 0); /* drive the shifter reset SM to completion */
} else {
    g_log_func("Gear 1..8, 99 and 0 (reset statemachine \r\n");   /* help */
}
```

(`shifter_send_gear` = the unchanged `0x08028458` PDU builder; the reset-SM poll
`FUN_08041A14` walks power-on → Modbus reg 4 → reset reg 0x15 → "SH off" clear
GPIO → re-init.)

## Concrete deltas

- **Removed:** the SOC byte write `strb [ctx+0x3D4]`; the `Set SOC %d\r\n` string
  (now entirely absent from the 1.08.02 image); the `announce_mark(2)` call.
- **Added:** the shifter power GPIO (PB14 via BSRR `0x40020418`, set on entry /
  reset on `0`); the value ladder `1..4` / `0` / `99` / default; the shift counter
  at `ctx+0x338`; calls to `shifter_send_gear` (apply gear), `FUN_08041A14`
  (reset-SM poll); the new strings `Shifter off\r\n`, `Reset\r\n`, and the help
  `Gear 1..8, 99 and 0 (reset statemachine \r\n`.
- **Quirk preserved from the OEM:** the help advertises gears **1..8**, but the
  fast path only handles **1..4** — values 5..8 (and anything else except `0`/`99`)
  fall through to the help print.

The adjacent `shifter_control_step` (`0x08028870` → `0x08041FEC`, sim 0.70) also
changed, but that is mostly relocation/recompilation; the genuine new behaviour is
this command handler.
