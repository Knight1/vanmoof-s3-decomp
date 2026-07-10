# Model / HW-config command — new in 1.09.01

1.09.01 adds a console command to configure the **bike model (ES3 / ES4)** and which
peripherals are fitted (eShifter, display), plus a sibling command to persist the
**hardware version**. This is the first sign the firmware line targets **two
hardware platforms** (ES3 and ES4).

**New strings (absent from 1.08.02):** `Set model ES3`, `Set model: model [3/4]
[1= shifter] [1= display]`, `Flash stored HWVER %d`, `Flash stored HWVER not set`,
`  ERR: set HWversion!`, ` Model %s`, ` Eshifter %s`, ` Display %s`, ` Motor %s`.

## `set model` — `FUN_080376F8`

Tokenises the command line (`FUN_08037454` token fetch + `FUN_0803F8B8` = `strtol`
base-10) and writes a **HW-config bitfield** into the device-state struct at offset
**`+0x145`** (struct base `*DAT_080377E8`):

| bit | meaning | set when |
| --- | --- | --- |
| `0x1` | model identity — **set = ES4, clear = ES3** | always set on the ES4 path; clear on ES3 |
| `0x2` | eShifter present | token 2 == 1 |
| `0x4` | display present | token 3 == 1 |

```
v = strtol(token1, 10);
if (v == 4) {                         /* ES4 */
    shifter = (token2 == 1);
    display = (token3 == 1);
    cfg = 1 | (shifter?2:0) | (display?4:0);
} else {                              /* ES3 */
    print "Set model ES3";
    cfg = 0x06;                       /* shifter+display, bit0 clear */
}
state[0x145] = cfg;
print " Model %s"    (cfg&1 ? "ES4"@0x0804D6C7 : "ES3"@0x0804D6CB);
print " Eshifter %s" (cfg&2 ? "On" : "Off");
print " Display %s"  (cfg&4 ? "On" : "Off");
```

### Consumers (boot)

`FUN_0803ED7C` (init) passes bit0 of `+0x145` (the model bit) as the model argument
to `FUN_0803ECD8`, which **copies one of six 24-byte (6-word) eShifter ratio/parameter
tables** (`DAT_0803ED64..0803ED78`) into the config block at `struct+0x2C6`, selected
by `(model bit, region/variant *(struct+0x109) = 1/3/other)`. Bit2 (display, tested
via the `<<0x1D` sign test) gates the display-init calls `FUN_0802F488` / `FUN_0802D17C`.

So the model/peripheral bitfield is load-bearing: it picks the drivetrain gear
ratios and whether the display is brought up.

## `Flash stored HWVER` — `FUN_080375F0`

A sibling command that parses one value, validates range **`0x10..0xFF`**, stores it
to `struct+0x147`, and reports `Flash stored HWVER %d` / `Flash stored HWVER not set`
(`0xFF` = unset). The boot routine `FUN_0803ED7C` emits `  ERR: set HWversion!`
(`0x0804F8FB`) when the stored HW version is missing.

## Notes

- The model bitfield at `+0x145` is **RAM device-state**; the persisted **hardware
  version** is the separate byte at `+0x147`. Neither handler writes a Modbus register
  — they configure in-RAM flags + the HW-version byte that drive eShifter-table
  selection and display init at startup.
- `Set model ES3` forces `0x06` (shifter + display, ES3) — i.e. the default/ES3 build
  still assumes both peripherals fitted; ES4 makes them individually selectable.
