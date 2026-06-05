# powerbankware — LED bar / Extend_IO driver

`src/extend_io.c` (OEM `FUN_08014350`) is the PowerBank's **5-segment LED bar**
driver. It maps the BMS state byte + the displayed SOC % to a one-byte bar
pattern and pushes that byte to an **SPI I/O-expander** that physically drives
the LEDs. There is no batteryware analogue — this is PowerBank-specific.

This note covers, in order: the hardware path, the exact output-byte format,
**when/where** the driver runs, the SOC→bar mapping, the per-state behaviour,
and — the focus of this document — the **complete error/fault pattern**.

## Hardware path

| | |
| --- | --- |
| Transport | the **same SPI1 bus** as the FEDL5236 AFE (HAL handle `0x20000634`) |
| Expander chip-select | **GPIOA PA8** (`0x100`) — driven low per byte (`EXTIO_CS_PIN`) |
| Payload | a **single byte** per update |
| Change-gating | the byte is sent **only when it differs** from the last one (shadow `0x200006e5`) |

The expander shares the AFE's SPI handle, so the LED write and an AFE transfer
are mutually exclusive on the bus; `extend_io_commit` waits for the handle to be
idle (`spi_get_state == 1`) before driving CS and transmitting.

## Output byte format (the decode)

`extend_io_commit` (OEM `LAB_0801482c`) composes the final byte from the level
pattern `s_led_level` (`0x200006a4`) and the mode word:

```c
s_led_out = (mode & 0x1000) ? 1 : 0;        /* bit0 = mode bit12 */
s_led_out |= (s_led_level & 0xfe);          /* level supplies bits 1..7 */
```

So the byte clocked into the expander is:

| bit | mask | meaning |
| --- | --- | --- |
| 7 | `0x80` | bar segment 1 (lit ≥ 1 %) |
| 6 | `0x40` | bar segment 2 (lit ≥ 25 %) |
| 5 | `0x20` | bar segment 3 (lit ≥ 50 %) |
| 4 | `0x10` | bar segment 4 (lit ≥ 75 %) |
| 3 | `0x08` | bar segment 5 (lit ≥ 90 %) |
| 2 | `0x04` | **normal "display-on" marker** (set in every non-empty SOC pattern) |
| 1 | `0x02` | **fault marker** (set *only* by the all-on/fault pattern `0xFA`) |
| 0 | `0x01` | mode bit 12 mirror — bypass/recovery active (charging-into-AP) |

This is why the magic level bytes look the way they do — read them in binary:

| `s_led_level` | binary | segments | marker |
| --- | --- | --- | --- |
| `0x00` | `0000_0000` | none | empty |
| `0x84` | `1000_0100` | 1 | on (bit2) |
| `0xC4` | `1100_0100` | 2 | on (bit2) |
| `0xE4` | `1110_0100` | 3 | on (bit2) |
| `0xF4` | `1111_0100` | 4 | on (bit2) |
| `0xFC` | `1111_1100` | 5 | on (bit2) |
| **`0xFA`** | `1111_1010` | **5 (all)** | **fault (bit1)** |

The **fault pattern `0xFA` lights all five segments but swaps the bit2
"display-on" marker for the bit1 "fault" marker** — i.e. the expander shows a
visibly distinct full-bar *error* indication (a different colour / blink on the
hardware side) rather than a normal full bar (`0xFC`). That single bit is the
whole error signal. `s_led_level` bit0 is always masked off (`& 0xfe`) so that
bit0 can carry the mode-bit-12 flag independently.

## When / where it runs

`extend_io_update()` is the entry point. It is called:

- **at boot** — `bms_system_init` (`src/bms.c`) right after the record load, and
  `boot_mode_enter` when entering upgrade/charge modes;
- **on the AFE temperature-fault / power-down path** — `fedl5236_initialize`
  (`src/fedl5236.c`) just before it halts;
- **once per slow tick in essentially every state handler** — every
  `bms_state_N` in `src/states.c` and the fault/shipping handlers in
  `src/state_handlers.c` call it on the slow-tick branch (bit 1 of the tick-flag
  byte `0x2000077c`). So in steady operation the bar is recomputed at the slow
  cadence and re-sent only when the pattern actually changes.

On entry, if **mode bit 11** (`0x800`) is set it first calls `extend_io_aux`
(OEM `FUN_080149bc`) — an empty/no-op hook in this image.

## Inputs

| Cell | Addr | Role |
| --- | --- | --- |
| state | `0x200005ac` | current BMS state (the switch selector) |
| SOC % | `0x2000052a` | displayed SOC — this is BMS-record byte **+0x5A** (the adjusted SOC% `bms_config_reset`/`bms_system_init` maintain) |
| mode | `0x200006a0` | bit12 → output bit0; bit11 gates `extend_io_aux` |
| blink | `0x20000728` | u16 animation phase counter |
| discharge mag | `0x20000420` | state-3 steady-vs-blink threshold (`< 200`) |
| charge accum | `0x200003c8` | state-2 "actively charging" gate (`> 299`) |
| led_level | `0x200006a4` | computed bar pattern (pre-commit) |
| led_out | `0x200006ec` | composed output byte |
| led_shadow | `0x200006e5` | last byte actually sent (change-gate) |

## SOC → bar level

`soc_bar(soc)` (steady), with a one-segment-dimmer twin `soc_dim` used by the
state-3 blink:

| SOC | `soc_bar` | `soc_dim` |
| --- | --- | --- |
| ≥ 90 % (`0x5A`) | `0xFC` (5) | `0xF4` (4) |
| ≥ 75 % (`0x4B`) | `0xF4` (4) | `0xE4` (3) |
| ≥ 50 % (`0x32`) | `0xE4` (3) | `0xC4` (2) |
| ≥ 25 % (`0x19`) | `0xC4` (2) | `0x84` (1) |
| 1 – 24 % | `0x84` (1) | `0x00` (0) |
| 0 % | `0x00` (0) | `0x00` (0) |

## Per-state behaviour

| State(s) | Meaning | LED behaviour |
| --- | --- | --- |
| **7 – 0x1B, and 0xFF** | **all protection / fault / shipping-wait / power-down states** | **`0xFA` all-on FAULT pattern** (see below) |
| 2 | normal operation / charging | **charging animation** — sweeping fill (below) |
| 3 | idle hold | **pulse** — full bar, alternating `soc_bar`↔`soc_dim` every blink cycle |
| 5, 6 | boot / charge-window | **bar off** (`0x00`) and clear mode bit12 |
| 0, 1, 4, 0x1C – 0xFE | idle / operating / boot / spare | **steady SOC bar** (`soc_bar`) |

### Charging animation (state 2)

If actively charging (`SOC < 0x5F` or charge accum `0x200003c8 > 299`), the bar
*fills up to the current SOC level in steps*, keyed on the blink phase
`0x20000728`; the number of steps and their phase offsets scale with the SOC
band (e.g. in the 90–94 % band it walks `0 → 0x84 → 0xC4 → 0xE4 → 0xF4 → 0xFC`
at blink phases `0,4,8,0xC,0x10,0x14`, wrapping at `0x17`). Otherwise (full /
not charging) it shows a steady `0xFC`.

## The error / fault pattern (focus)

```c
/* extend_io_update(), src/extend_io.c */
if ((state >= 7 && state <= 0x18) ||
    (state >= 0x19 && (state < 0x1c || state == 0xff))) {
    *s_led_level = 0xfa;     /* all 5 segments + bit1 fault marker */
    *s_blink     = 0;        /* steady, no animation */
    extend_io_commit();
    return;
}
```

**Coverage.** The two clauses together select **every state in `7 … 0x1B`
inclusive, plus the local power-down self-test state `0xFF`**:

- `state >= 7 && state <= 0x18` → `7 … 0x18`
- `state >= 0x19 && (state < 0x1c || state == 0xff)` → `0x19, 0x1A, 0x1B`, and `0xFF`

Mapped to what those states mean (see `docs/fuse.md`, `docs/hardware.md`,
`src/transitions.c`):

| State | Fault | LED |
| --- | --- | --- |
| 7 | OVP1 (over-voltage, 1st) | `0xFA` |
| 8 | OVP2 (over-voltage, 2nd) | `0xFA` |
| 9 | UVP1 (under-voltage, 1st) | `0xFA` |
| 0x0A | UVP2 (under-voltage, 2nd) | `0xFA` |
| 0x11 | AFE hard-fault latch | `0xFA` |
| 0x12 | COTP (charge over-temp) | `0xFA` |
| 0x13 | CUTP (charge under-temp) | `0xFA` |
| 0x14 | DOTP (discharge over-temp) | `0xFA` |
| 0x15 | DUTP (discharge under-temp) | `0xFA` |
| 0x16 | state-16 protection hold | `0xFA` |
| 0x17 | **MOS failure** (welded FET) | `0xFA` |
| 0x18 | **OV 2nd** (record) | `0xFA` |
| 0x19 | cell imbalance | `0xFA` |
| 0x1A | generic fault hold (`bms_state_fault`) | `0xFA` |
| 0x1B | shipping-wait | `0xFA` |
| 0xFF | local power-down self-test | `0xFA` |

> Note the gaps `0x0B–0x10` and `0x1C–0xFE` fall through to the **default**
> branch (steady SOC bar), not the fault branch — they are operating/charge-hold
> states, not protections. State `0x1C` is specifically excluded from the fault
> band (`state < 0x1c`).

**What the user sees.** For the entire protection/fault band the bar is forced
to **all five segments lit with the bit1 fault marker set and bit2 (normal-on)
clear** — a steady full-bar *error* display, with the animation counter zeroed
so it does not blink or sweep. Because `extend_io_commit` only transmits on a
change, the fault byte is sent once on entry and then held (re-sent only if the
mode-bit-12 bit0 flips). The fault indication therefore persists for as long as
the BMS sits in the latched fault/shipping/power-down state — which, for the
terminal second-level protections (`0x17`/`0x18`/`0x19`) and shipping, is until
the pack is physically reset or runs flat.

## Commit / SPI transaction

`extend_io_commit`:

1. Compose `s_led_out` = `(mode bit12 → bit0) | (s_led_level & 0xfe)`.
2. If it equals the shadow `0x200006e5`, **return** (nothing changed).
3. Otherwise wait (≤ 10 tick-gated retries) for the shared SPI handle to read
   idle (`spi_get_state == 1`); on persistent busy, log `Extend_IO` busy and
   return.
4. Latch the shadow, drive **CS PA8 low**, load the byte into the AFE TX buffer
   (`0x200005f4`), and `spi_transmit_receive` one byte. On HAL error: log
   `Extend_IO` retry and `spi_error_reset`.
5. Wait (≤ 10 retries) for completion; on timeout log `Extend_IO` timeout.

The busy/retry/timeout banners are the `s_extio_*` strings in `src/strings.c`.
