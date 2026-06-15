# Wake / sleep reporting — 1.07.06 → 1.08.02

Two concrete additions in the low-power path: the **wake-reason taxonomy was
expanded 9 → 13 classes** (`enter_stop_mode`), and a **new boot-survivable wake
counter** was added (in `status_process`).

## `enter_stop_mode` — wake-reason taxonomy 9 → 13 (sim 0.48)

| | 1.07.06 (`enter_stop_mode` `0x080382D0`, 580 instr) | 1.08.02 (`0x0803EFA8`, 743 instr) |
| --- | --- | --- |
| Reason-switch bound | `cmp r6,#8` (reasons 0–8) ×2 | `cmp r6,#0xC` (reasons 0–12) ×2 |
| 0 / 1 | `Wake:All` | `Wake:All` (DAT_0803f264) |
| 2 | `Wake:NoMems` | **`Wake:noBLE`** `@0x08054EE4` *(NEW)* |
| 3 | `Wake:RST` | **`Wake:noBLEnoRTC`** `@0x08054EF4` *(NEW)* |
| 4 | `Wake:RST` | `Wake:NoMems` (DAT_0803f234) |
| 5 | `Wake:RST` | **`Wake:NoMemsnoBLE`** `@0x08054EAC` *(NEW)* |
| 6 | `Wake:Shipping` | **`Wake:NoMemsnoBLEnoRTC`** `@0x08054EC0` *(NEW)* |
| 7 / 8 / 9 | 7 `Wake:No bat`, 8 `Wake:ERROR` | `Wake:RST` (DAT_0803f274) |
| 10 | — | `Wake:Shipping` |
| 0xB | — | `Wake:No bat` |
| 0xC | — | `Wake:ERROR` — now via a **distinct fn-ptr pair** (`  ERR2 LIS3DH` / `Wake:ERROR` `@0x08054F44`), not the shared logger path 1.07.06 used |

`diff_functions`: similarity **0.4839**, body_equal 553, body_added 188,
body_removed 25; `strings_only_in_b` = the four new `Wake:*` sub-mode strings.

So 1.08.02 distinguishes wake-arm configurations that suppress the **BLE** and/or
**RTC** wake sources (on top of the existing no-accelerometer "NoMems" case): a
wake can now be reported as plain, no-BLE, no-BLE+no-RTC, no-MEMS, no-MEMS+no-BLE,
or no-MEMS+no-BLE+no-RTC. The existing reasons (All / NoMems / RST / Shipping /
No bat / ERROR) are retained but renumbered to make room, and the LIS3DH-fail
ERROR path got its own logging call (the `  ERR2 LIS3DH` accel-powerdown-fail
string itself is unchanged).

## New wake counter (`status_process`)

A new format string **`Wake counter %d\r\n`** `@0x0805032C` exists only in 1.08.02
and is referenced by the status state machine (`FUN_08029E18`, the 1.08.02
`status_process`) at `0x0802B428` — **not** by `enter_stop_mode`.

It maintains a **saturating `uint8` wake count** in the per-session sub-struct at
offset **`+0x344`** (the struct pointed to by `*(ctx+8)`; ctx base
`DAT_0802B574`). At `0x0802B40A`–`0x0802B42A`:

```c
uint8_t n = sub->wake_count;          /* *(u8 *)(sub + 0x344) */
if (n != 0xFF) {                      /* saturate at 255 */
    n++;
    sub->wake_count = n;
}
log_print_timestamp_prefix();
printf("Wake counter %d\r\n", sub->wake_count);
```

No such field or string exists in 1.07.06 — so 1.08.02 counts and reports how many
times the bike has woken (capped at 255), in addition to *why* it woke.
