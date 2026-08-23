# batteryware — wake modes & the DET→TEST reset loop

How the battery module (STM32L072 + ML5236 AFE) is woken from its
powered-down / shipping state, and why one particular wake method —
**bridging DET to TEST** — puts the pack into a self-sustaining
watchdog-reset loop instead of booting cleanly.

Cross-refs: [`fedl5236.md`](fedl5236.md) (AFE register map, PUPIN /
POWER-down handshake), [`hardware.md`](hardware.md), and mainware's
[`docs/battery.md`](../../mainware/docs/battery.md) (the DET/reset pins
on the main-board side).

> **Provenance.** The reset-loop *mechanism* is verified against the
> decomp (`main.c`, `bms_setup.c`, `bms_init.c`, `reset.c`,
> `bmsboot/src/system.c`). The AFE pin behaviour is now verified against
> the **datasheet** (`batteryware/references/ML5236.pdf`, FEDL5236-10) —
> cited inline as *(DS pN)*. The only remaining inference is the
> **pad-name → chip-pin netlist** of the VanMoof pack (which physical pad
> the owner calls "DET" / "TEST"); the repo still has no board schematic,
> so that mapping is marked **[net-map inferred]**.

## Power baseline (datasheet)

The ML5236 AFE contains the **built-in 3.3 V regulator (`VREG`, pin 35)
that powers the external STM32L0** *(DS p4/p46 — "Can power the external
MCU"; App Circuit 2 = "MCU Power Supply = VREG")*. So AFE power state ==
MCU power state. Shipping / power-down is `POWER.PDWN=1` (register `0x0C`
← `0x10`), which "halts all circuit operations" *(DS p25)* and drops
`VREG` → the MCU is unpowered.

The AFE has exactly **two documented wake sources** *(DS p25/p33)*:

1. **`/PUPIN` (pin 37) driven "L"** — "Power-up trigger input. The device
   wakes up with the 'L' level input. Internally pulled up to VDD through
   a 1 MΩ resistor" *(DS p4)*. While `/PUPIN` is held "L" the part
   **cannot** re-enter power-down *(DS p25)* — a solid ground both wakes
   and *holds* it awake.
2. **`PSNS` (pin 38) ≥ ½VDD** — charger-presence detect; "the device is
   powered-up when the PSNS level becomes 1/2 VDD or higher during
   power-down" *(DS p4)*.

- **Nothing connected ⇒ the BMS stays powered down.** *(owner-confirmed,
  and datasheet-consistent)* `/PUPIN`'s internal 1 MΩ pull-up to VDD
  holds it "H", no charger on `PSNS` → the AFE keeps `VREG` off and the
  STM32L0 never runs. Correct idle state.

> Note: **pin 36 `TEST`** is a separate pin — "Device test enable input.
> **Should be fixed to GND**" *(DS p4)* — and is tied to GND in the
> datasheet's own application circuits. It is *not* a wake input. The
> wake the owner calls "TEST→GND" therefore corresponds to asserting the
> **`/PUPIN`** trigger "L" (the pad the owner labels "TEST" routes to the
> `/PUPIN` net) **[net-map inferred]**.

## The two intended wake paths

| Wake | Trigger (datasheet mechanism) | Result |
| --- | --- | --- |
| **"TEST → GND"** (owner's bench method) | pull the **`/PUPIN`** net to a solid GND ("L") | AFE powers up and is *held* awake → clean cold boot, all features (charge/discharge, telemetry, service UART). Works standalone, no main board. |
| **Normal (in-bike)** | mainware pulses **PB5** (BMS reset/power, GPIOB `0x40020400` mask `0x20`) — asserts the pack's wake net — then drives Modbus to slave `0xAA`; a connected **charger** also wakes it via `PSNS` | controlled bring-up gated by mainware (`battery.c`): detect on **PC10 (DET)** → `"BMS Pulse"` → `bms_modbus_read(0,1)` "ask ID" → enable discharge (reg 8/9) → telemetry |

Both land the STM32L0 in a real boot: `bmsboot` (`I am G5 VanMoof BL
V004 …`) → `batteryware` `main()`.

## The DET→TEST bridge = the reboot loop

**Observed** *(owner)*: bridging **DET to TEST** is what produced
yesterday's loop —

```
I am G5 VanMoof BL V004 2019-11-19
ResetStatus=0x0C000002      <- first: power-on reset (POR|PIN|LSIRDY)
I am G5 VanMoof BL V004 2019-11-19
ResetStatus=0x24020002      <- then, forever: IWDG reset (IWDGRSTF|PIN|RTCSEL=LSE|LSIRDY)
I am G5 VanMoof BL V004 2019-11-19
ResetStatus=0x24020002
…
```

(`ResetStatus` = the STM32L0 `RCC_CSR` reset-cause word, saved and
cleared each boot by `bmsboot` — `system.c:207-208`. Bit decode:
`0x0C000002` = PORRSTF+PINRSTF+LSIRDY = cold power-up; `0x24020002` =
IWDGRSTF+PINRSTF+RTCSEL(LSE)+LSIRDY = **independent-watchdog reset**.)

### Why it loops — firmware verified, datasheet-explained

1. **The bridge feeds the AFE a *chattering* wake, not a clean one.** The
   owner's DET→TEST jumper ties the **`/PUPIN`** wake net to **DET**,
   which is a **signal line, not a solid ground** — so `/PUPIN` never
   sees the clean "L" that a proper wake needs. The datasheet warns
   exactly about this: after power-on the part is normally in normal
   mode, *"but it may be the power-down state due to chattering noise
   during power-on sequence"* *(DS p33)*, and every `/PUPIN` assertion
   restarts the AFE's internal power-up sequence
   (`State: Resetting → Initial settings`, then a `VREG/VREF`
   stabilization time before measurements are valid — DS p33 timing
   diagram). A noisy `/PUPIN` keeps the AFE's control logic bouncing
   through that reset/settle cycle, so it never delivers a stable
   measurement result over SPI. `VREG` stays up *enough* to keep the MCU
   powered — which is why the loop is **IWDG**, not repeated POR — but
   the AFE never becomes usable. **[the DET-is-not-clean-GND step is
   net-map inferred; the AFE-chatters consequence is datasheet-cited]**

2. **`bmsboot` arms and starts the IWDG, then hands off.** The
   independent watchdog is initialised (`iwdg_hal_init`, reload `0x908`,
   `bmsboot/src/system.c:122-139`), kicked once (`system.c:213`), and
   left **running** when control passes to `batteryware`. From here the
   app *must* keep reloading it or the MCU resets.

3. **`batteryware` wedges in AFE init, on a path that never kicks the
   watchdog.** Boot order is `main()` → `batteryware_main()` →
   `bms_setup()` → **`bms_init()`** (`bms_setup.c:163`), all **before**
   the main super-loop. The only IWDG reloads in the app are:
   - the single `**(u32**)0x20002C10 = 0xAAAA` in `batteryware_main()`
     (`main.c:77`), and
   - `fg_watchdog_kick()` inside the per-state handlers — which only run
     *in the super-loop* (`state_handlers.c`).

   `bms_init()` sits between the two. Its **Phase 2** (status
   acquisition) and **Phase 3** (cell-voltage acquisition) are
   `do … while` loops that spin until the AFE returns valid data
   (`bms_init.c:122-153`, `158-198`), and `wait_for_chip_ready()` is a
   bare `while (gpio_bit_read(GPIOC, PC12))` busy-spin (`bms_init.c:77`).
   **None of these paths reload the IWDG** — they only call `smbus_*`,
   `uart_tx_flush`, `uart_resp_handler`, `uart_tx_isr`.

4. With the AFE stuck re-running its power-up/reset cycle, its
   measurement-complete never latches, so those SPI reads never succeed /
   PC12 BUSY never clears and `bms_init()` **never returns**. The IWDG
   (armed in step 2, reload `0x908`) times out in ~2–4 s and resets the
   MCU → `IWDGRSTF` (`0x24020002`).

5. On reset, `bmsboot` runs again, re-arms + re-kicks the IWDG, prints
   the banner, hands off — `batteryware` wedges in `bms_init()` again —
   IWDG fires again. **Self-sustaining loop.** The RTC/backup domain
   survives an IWDG reset, so `RTCSEL=LSE` stays latched and every repeat
   reads the identical `0x24020002` (only the very first, post-POR, read
   `0x0C000002`).

> **Takeaway.** DET→TEST is **not** a valid wake. It powers the MCU just
> enough to boot but not enough for the AFE to complete `bms_init`, and
> because `bms_init` runs before the super-loop on a path with no
> watchdog kick, the pack cannot escape — it re-boots every few seconds
> forever. Use **TEST→GND** (standalone full wake) or the **normal
> mainware PB5-pulse + Modbus** path instead.

### Firmware-hardening note (not a field fix)

The loop is only *self-sustaining* because `bms_init`'s acquisition
retries and `wait_for_chip_ready` have **no watchdog service and no
bounded retry / timeout**. A guarded build would either kick the IWDG
inside those loops or cap the retries and fall through to a fault state,
so a non-responsive AFE reports an error instead of watchdog-looping.
This matches the OEM behaviour as decompiled; it is recorded here as an
observation, not applied.

## Pin summary

### Main-board side (from the `mainware` decomp, by behaviour)

| name | main-board pin | role | direction (mainware) |
| --- | --- | --- | --- |
| **DET** | PC10 (GPIOC `0x40020800` mask `0x400`) | pack present / detect | input (read) |
| reset/wake pulse | PB5 (GPIOB `0x40020400` mask `0x20`) | wake/reset the pack | output (write) |
| sleep/FAULT sense | PD1 (GPIOD `0x40020C00` mask `0x2`) | BMS sleep / fault | input (read) |

### AFE side (ML5236 datasheet, verified)

| pin | name | I/O | role *(DS p4/p25/p33)* |
| --- | --- | --- | --- |
| 37 | **`/PUPIN`** | I | power-up trigger; wakes on "L"; 1 MΩ pull-up to VDD; holds part awake while "L" |
| 38 | **`PSNS`** | I | charger-presence wake (≥ ½VDD during power-down) |
| 36 | **`TEST`** | I | device test-enable; *must be tied to GND* (not a wake) |
| 35 | **`VREG`** | O | built-in 3.3 V regulator; powers the STM32L0 |

> The main-board names are derived from how `mainware` *drives* each
> line; the AFE names/behaviour are datasheet-verified. The open item is
> the **netlist between them** — which VanMoof pad ("DET" / "TEST") lands
> on `/PUPIN` vs `PSNS` vs a plain GND. A board schematic or a
> continuity check from the pads to ML5236 pins 36–38 would close it and
> let the **[net-map inferred]** tags above be dropped.
