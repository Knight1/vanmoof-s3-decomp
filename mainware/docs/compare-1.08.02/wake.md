# Wake counter / reason — expanded again in 1.09.01

The wake-reporting trend from [1.07.06 → 1.08.02](../compare-1.07.06/wake.md)
continues. Both prints live in the main bike state-machine loop `FUN_08026EB0`
(1.09.01) — the equivalent of 1.08.02's wake-counter site. The wake counter is the
byte at controller-state `+0x344`.

## Wake counter: `%d` → `%d of %d, type %d`

| | 1.08.02 | 1.09.01 |
| --- | --- | --- |
| string | `Wake counter %d\r\n` (1 arg) | **`Wake counter %d of %d, type %d\r\n`** (3 args) |
| counter clamp | `0xFF` (255) | **`0xC7` (199)** |
| args | counter only | counter `*(state+0x344)`, budget `0xA8` (168), **type** `*(0x20000063)` (a 1-byte wake-type code) |

1.09.01 site (~`0x08028B34`): `cmp r3,#0xC7; itt ls; add.ls r3,#1; strb.ls [r2,#0x344]`
then logs with the running counter, the constant **168** ("of %d"), and the global
wake-type byte. So the bare count became a **budgeted, typed** counter.

## New: SoC-gated BLE deep-sleep report

A brand-new branch (~`0x08027A40`, no equivalent in 1.08.02): when SoC is low
(`*(state+0x40C)` < `0x13` = 19) and the wake counter is high (> `0xA7` = 167), it
**resets the wake counter to `0xA8` = 168** (`movs r3,#0xA8; strb [r6,#0x344]`), sets
a flag `*(state+0x333)=1`, and logs `BleDeepSleep soc %d wakeups %d\r\n`
(`0x08040FFA`) with the SoC (`+0x40C`) and the wake count. This pairs with the 168
"budget" above — a low battery after many wakes drives a deeper BLE sleep and is
reported.

## New: wake-reason token table

The init/banner `FUN_08031E70` gained a `\r\nWake Reason: ` prefix (`0x08042AB3`)
plus a table of `WAKE_SRC_*` tokens (`0x08042AC3..0x08042B40`): **BUTTON_1 / BUTTON_2
/ BLE / MEMS / CHG / WHEEL / KEY_IN / KICKLOCK / RTC** — versus 1.08.02's single
combined `Wake Reason: %s\r\n`. So the wake source is now reported from an enumerated
token set.

## Net

1.09.01 turns the wake counter into a **budgeted (199/168) + typed** counter, adds a
**SoC-aware BLE deep-sleep** entry that re-arms the counter at 168 and reports
SoC + wakeups, and broadens boot-time wake-reason logging to a token table — a
continuation of the low-power observability work begun in 1.08.02 (which itself had
expanded `enter_stop_mode`'s reason taxonomy 9 → 13).
