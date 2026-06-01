# powerbankware — hardware notes

MCU: **STM32F091xC** (Cortex-M0, 256 KB flash / 32 KB SRAM). Identified from:
32 KB SRAM (SP 0x20008000), the **no-VTOR vector relocation** (copy vectors to
SRAM + `SYSCFG_CFGR1.MEM_MODE=3` ⇒ Cortex-M0, not the M0+ of batteryware), and
the peripheral bases below. The DAC (output regulation) and 32 KB SRAM rule out
the smaller F0 parts; F091xC is the fit.

## Peripheral bases (verified from init code)

| Peripheral | Base | Evidence |
| --- | --- | --- |
| FLASH | `0x40022000` | HAL init sets `+0x00 |= 0x10` (ACR) |
| SYSCFG | `0x40010000` | `+0x00 |= 3` → MEM_MODE=3 (SRAM remap); EXTICR1 @ `+0x08` |
| EXTI | `0x40010400` | IMR `+0x00` / EMR `+0x04` / RTSR `+0x08` / FTSR `+0x0C` (gpio_pin_config) |
| RCC | `0x40021000` | APB2ENR `+0x18` (SYSCFG en, bit0), AHBENR for GPIO clocks |
| RTC | `0x40002800` | HAL RTC handle instance |
| USART1 | `0x40013800` | (F0 APB2) — Modbus link, TBC |
| DAC | `0x40007400` | (F0 APB1) — Vout regulation, TBC |
| GPIO | `0x48000000` | **confirmed** A=`…000` B=`…400` C=`…800` D=`…c00` E=`…1000`, 0x400 stride (gpio_pin_config port→EXTICR map) |

> **F0 vs L0 register-offset trap.** powerbankware (F091) and batteryware
> (L072) share the GPIO/EXTI *layout* but differ on RCC offsets: F0 puts
> `APB2ENR` at RCC+`0x18` where L0 uses `0x34`. Always resolve the literal
> pool from *this* image; do not copy register offsets from batteryware.

## SRAM globals (from main @ `0x0800f52c`)

| Address | Role |
| --- | --- |
| `0x200006A0` | **mode / cfg word** (gated on bits &1 / &3>>1 / &7>>2, like batteryware `s_bms_cfg`) |
| `0x200005AC` | **current state** byte (jump-table index, `< 0x1c`) |
| `0x0801E6EC` | **state-handler jump table** (flash, 28 entries) |
| `0x20000724` | version / ID block (main reads `+1`, `+2`) |
| `0x20000218` | value/limit block (cal threshold checks: 0x51 / 0x2c / 99 / 0x18) |
| `0x200003CE` / `0x200006A0` | boot-delay counter vs `0x752F` threshold |

## FEDL5236 AFE wiring (confirmed from the SPI driver + init)

| Signal | Pin | Notes |
| --- | --- | --- |
| SPI CS | GPIOA **PA15** (0x8000) | driven low per transfer, raised in TxRxCpltCallback |
| (aux) | GPIOA **PA8** (0x100) | raised alongside CS in TxRxCpltCallback |
| AFE INT / busy | GPIOC **PC13** (0x2000) | held high while converting; init polls it |
| Wake pulse | GPIOB **PB2** (0x4) + **PB10** (0x400) | pulsed in `fedl5236_wakeup` |
| TS-fault output | GPIOB **PB12** (0x1000) | raised then cleared on the power-down/halt path |
| Power-on handshake | GPIOA **PA11** (0x800) | init waits for it high before clearing PB12 |

SPI HAL handle @ SRAM `0x20000634`; ms tick counter @ `0x20002614`; IWDG-kick
pointer cell @ `0x200006ac`; SysTick software flag byte @ `0x2000077c` (bit 0
= 1 ms, bit 2 = periodic). FEDL5236 frame CRC-8 is a runtime-installed routine
(pointer @ `0x200000c4`, populated by the reset `.data` copy).

Key BMS SRAM cells (from `fedl5236_initialize`): total voltage `0x20000418`,
cell-voltage table `0x20000380[10]`, cell sum `0x200003ce`, max/min
`0x200003a2`/`0x200003d2` (+u8 indices `0x20000430`/`0x200003c4`), charger
voltage `0x2000042c`, TS0/TS1 temps `0x20000219`/`0x2000021a`, current state
`0x200005ac`, mode word `0x200006a0`.

### BMS fault register `0x20000410` (u16) — from `bms_periodic_update`

The protection cascade debounces each fault into this bitfield. **The bit
layout differs from batteryware** (there bit0=UVP1/bit2=OVP1/bit10=imbalance) —
derive from *this* image, never reuse batteryware's names:

| Bit | Mask | Fault | Set when | Clear when | Debounce |
| --- | --- | --- | --- | --- | --- |
| 0 | `0x001` | OVP1 | max cell > 4249 mV | < 4150 mV | 60 set / 6 clr |
| 1 | `0x002` | OVP2 | max cell > 4299 mV | < 4150 mV | 6 / 6 |
| 2 | `0x004` | UVP1 | min cell ≤ 3000 mV | > 3299 mV | 60 / 6 |
| 3 | `0x008` | UVP2 | min cell < 2801 mV | > 3299 mV | 6 / 6 |
| 4 | `0x010` | COCP1 | charge I > 4499 (·mA) | I < 200, MOS<3 | 60 / 90 |
| 5 | `0x020` | COCP2 | charge I > 5999 | I < 200, MOS<3 | 6 / 90 |
| 6 | `0x040` | DOCP1 | discharge I > 7999 | I < 200 | 60 / 90 |
| 7 | `0x080` | DOCP2 | discharge I > 9999 | I < 200 | 6 / 90 |
| 10 | `0x400` | TS/temperature | AFE status rx[3]&2 | discharge I < 200 | — / 90 |
| 11 | `0x800` | cell imbalance | max>3599 & (max−min)≥501 mV | — | 100 |

Hard discharge (>199) auto-clears OVP1/2; hard charge (>199) auto-clears UVP1/2
(opposite-direction current relieves the voltage condition). SET branches reset
their debounce counter when they fire; CLEAR branches do not.

### Coulomb counter / current SRAM cells (`bms_periodic_update`)

Current = `(|AFE-reg-0x2e − offset| · 69000) >> 16`; offset cache `0x20000418`.
Sign in `0x2000039c` (neg=discharge), 4-deep moving average `0x200003ac[4]` →
signed avg `0x20000424`; discharge magnitude `0x20000420`, charge magnitude
`0x200003a8`. CHG cal `0x2000023c`→CFG+0x58, DSG cal `0x2000023e`→CFG+0x56
(**applied crossed**: CHG cal in the discharge branch, DSG cal in charge —
OEM quirk). 40-sample RSOC accumulator `0x20000250`, divisor `0x20000228`,
sample counter `0x2000022c`. Peak currents → CFG+0x74 (charge) / CFG+0x76
(discharge), `/10`, 20-tick debounce. Charger-detect round-robin array
`0x200003d4[15]` (u32) → charger voltage `0x2000042c` (held > 19999, dropped
after 900 ticks). Temp peaks → CFG+0x78/+0x79; TS cal `0x20000205`/`0x2000021b`.

### Fuel-gauge measurement cells (`bms_measure_update` @ `0x080089b0`)

3-cell measurement block from raw ADC `0x20000208` (u16 ×3: [0] cell1, [1] cell2,
[2] pack current). Scaled results: cell1 mV `0x200001ae`, cell2 mV `0x20000202`
(both i16). Cal multipliers at record+0x70 / +0x72 (u16, applied `/1000` when
non-zero). Pack current `(raw·0x325·10000)/(0x325aa0 − raw·0x325)` → **146-entry
descending SOC lookup** at flash `0x0801e820` → raw index in `0x20000218[0]`.
Signed SOC correction `0x2000020e` (i8), debounce `0x2000020f`; SOC-history
high-water at record+0x7a. Sample counter `0x200001ac` (window < 0x32);
sample-ready flag `0x20000204` bit0. `0x20000218`: **[0] SOC index, [1]/[2] temp
sensors** (byte; the alarm monitors and coulomb temp-derate read [1]/[2]).

### State-entry / protection GPIO map (`src/transitions.c`)

| Line | Pin | Role |
| --- | --- | --- |
| `0x80` on GPIOA | PA7 | dropped low at every state entry |
| `0x200` on GPIOA | PA9 | charge-path enable (boot tail) |
| `0x1000` on GPIOA | **PA12** | **bypass FET gate** (mode word bit10 mirror) |
| `1` on GPIOB | PB0 | FET/relay line |
| `0x40` on GPIOB | **PB6** | **PUPIN input** (alarm b5 debounce) |
| `0x80` on GPIOB | PB7 | charger-present indicator (OV2 path) |
| `0x200` on GPIOB | PB9 | FET/relay line |
| `0x800` on GPIOB | PB11 | FET/relay line |
| `0x8000` on GPIOB | PB15 | AFE chip-select used by `bms_measure_update` |

**FEDL5236 reg-9 FET-control shadow: `0x20000412`** (0 = both off, 1 = charge,
2 = discharge); written then pushed via `fedl5236_command_write(9, …)`. Balance
mask uses regs 10/11 (cell selector `0x20000430`, debounce `0x2000041c`, active
`0x20000224`).

### State-transition dispatcher (`bms_state_enter` @ `0x0800f7b4`)

Every entry above tail-branches here. On each transition it: clears mode bit 12
(`0x200006a0 &= 0xEFFF`), saves the old state byte to `0x200004b0` and latches the
new one to `0x200005ac`; zeroes ~20 soft counters / alarm debounces
(`0x20000594` u32, `0x200006e4`, `0x20000550`, `0x20000584`, `0x20000580`,
`0x200004ca`, `0x20000588`, `0x200004bc`, `0x200005ae`, `0x200004b8`,
`0x200006a2`, `0x2000041b`, `0x200006a8`, `0x2000069e`, `0x20000720`,
`0x20000710`, `0x20000728`, `0x2000069c`, `0x20000248` u32, `0x200006bc`); parks
the output (`vout_bypass_off`, **PA9** `0x200` driven high, clears mode bit 7,
and — when **mode bit 5** marks cell-balancing active — disables it via FEDL5236
regs 10/11). When the event log is enabled (`0x2000072c != 0`) it bumps a
per-state u16 event counter inside the BMS record and writes one **circular trace
entry** to the second record block at **`0x200005b0`** (~0x40 bytes: index +0x0c,
event code +0x0e, temps/SOC +0x2a..+0x2c, cell sum +0x26, current +0x00, caps
+0x04/+0x08, SOC/SOH +0x28/+0x29, cycle +0x10, 10 cell voltages from +0x12),
wrapping the index at `0xfde7` and committing it to the EEPROM error log
(`errlog_erase((idx-1)/1000)`). Its tail hook `afe_fet_status_refresh`
(`0x0800e1ac`) re-reads FEDL5236 reg 0x0D and caches the FET/balance status byte
(AFE buffer +2) to the **FET-status shadow `0x200003a5`** on each transition.

### Alarm / request word `0x200006e4` (`src/alarms.c`)

8 slow-tick debounce monitors latch bits 0-7; the state-1 slow tick maps them to
fault entries. Fault-type code written to **record+0x5c (`0x2000052c`)**: 1 =
charge OC, 2 = discharge OC, 3 = PUPIN. Per-monitor counters: b0 `0x20000550`,
b1 `0x20000580`, b2 `0x20000584`, b3 `0x200004ca`, b4 `0x20000588`, b5
`0x2000058d`, b6 `0x200004bc`, b7 `0x200005ae`. Over-current values are **u32**
(`0x200003a8` charge, `0x20000420` discharge; threshold 500); cal limits at
`0x20000218[1]/[2]` are bytes.

## Power-path layer (powerbank-specific — strings)

Output stage absent from batteryware: **bypass FET** (PA12, `ByPass On/Off`),
**charger/load detection** (`Charger In & Load Exist/Absent`, `No Load`),
**DAC-regulated Vout** ~20–30 V (`DAC_Value= %l mV`, `Vout <20V over 3Sec`,
`Vout <30V over 30min`, `DAC Over Range`), `Check_Charger_Voltage` / `Check_PUPIN`.
DAC/TIM HAL handle pointer at `0x20000258` (bit16 of its first control word =
channel enable); DAC compare shadows `0x20000254`/`0x2000026c`; mode bits 6/7
gate the output. Timer-start handle `0x20000738` (CR1.CEN + DIER.UIE).
Capacity counters: remaining-accumulator `0x20000238`, capacity-learning
`0x20000230`/`0x20000234`/`0x20000240`, full-cap limit `0x200005a4`, cycle
threshold `0x200005a8`; record fields +0x18 remaining mAh, +0x1c full cap,
+0x20 SOC scratch, +0x24 cycle accumulator, +0x50 cycle count, +0x5a SOC%,
+0x5b SOC2%. Min-cell/cap `0x200003d2` (u16), max-cell `0x200003a2` (u16).

## Shared BMS core (= batteryware)

FEDL5236 AFE (`FEDL5236_Initialize/Default_Setting/Zero_Current_Offset/
Total_Voltage_Check/Process/Command_Write/Read_Data/PowerDown`), SOC/SOH/cycle
telemetry, protections (COTP/CUTP/DOTP/DUTP/OV-2nd/MOS-failure), CHG/DSG
calibration, shipping mode, AP/BL/Shadow image copy. AP id: `"\nI am VM-BATT AP\r"`.
