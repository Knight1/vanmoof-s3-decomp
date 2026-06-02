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
| RCC | `0x40021000` | AHBENR `+0x14` (GPIO port clocks), APB2ENR `+0x18` (SYSCFG en, bit0), APB1ENR `+0x1c` (PWR en, bit28), BDCR `+0x20` (LSE/RTC) |
| RTC | `0x40002800` | HAL RTC handle instance |
| IWDG | `0x40003000` | `iwdg_init` KR `+0x00` ← `0xAAAA` (reload key) |
| USART1 | `0x40013800` | (F0 APB2) — Modbus link, TBC |
| DAC | `0x40007400` | (F0 APB1) — Vout regulation, TBC |
| GPIO | `0x48000000` | **confirmed** A=`…000` B=`…400` C=`…800` D=`…c00` E=`…1000`, 0x400 stride (gpio_pin_config port→EXTICR map) |

> **F0 vs L0 register-offset trap.** powerbankware (F091) and batteryware
> (L072) share the GPIO/EXTI *layout* but differ on RCC offsets: F0 puts
> `APB2ENR` at RCC+`0x18` where L0 uses `0x34`. Always resolve the literal
> pool from *this* image; do not copy register offsets from batteryware.

## Boot bring-up (clock / IWDG / board GPIO)

`hal_bringup` (`0x080114dc`) runs flash prefetch + vector→SRAM relocation, then
calls, in order: `hal_init` → `clock_rtc_init` → `tick_state_reset` →
`iwdg_init` → `flash_lock` → `board_init`.

**`clock_rtc_init` (`0x080136c0`) — clock tree + RTC.** Enables backup-domain
write access (`FUN_0801b51c` sets `PWR_CR.DBP`), sets `RCC_BDCR` (`+0x20`)
`|= 0x18` (LSEDRV = 0b11, max drive), runs `HAL_RCC_OscConfig` (LSE/LSI/HSE),
`HAL_RCCEx_PeriphCLKConfig` (RTC clock = LSE), `HAL_RCC_ClockConfig`, then
`RCC_BDCR |= 0x8000` (RTCEN) and `HAL_RTC_Init` on the handle at `0x200006f0`
(Instance = RTC `0x40002800`, `HourFormat`/cal fields {0, 0x7f, 0xff, 0,0,0}),
finally `SystemCoreClockUpdate`. The three HAL config structs are stack-local
and zeroed with `mem_set` (a `={0}` would pull in a libc `memset` this
`-nostdlib` build can't link).

**`iwdg_init` (`0x08013820`) — independent watchdog.** HAL handle at
**`0x200006ac`**: Instance = IWDG `0x40003000`, Prescaler = 4 (÷64), Reload =
`0x4e2`, Window = `0xfff` (disabled). After `HAL_IWDG_Init` it writes the reload
key `IWDG_KR ← 0xAAAA` through the handle's Instance pointer — the same cell
`delay_ms`/`ota.c` later dereference (`**0x200006ac`) to kick the dog.

**`tick_state_reset` (`0x08014ac8`).** Zeroes the software ms-tick flag **byte**
at `0x2000077c` (`strb`) and a 16-bit companion counter at **`0x20000778`**
(`strh`); the millisecond tick word remains `0x20002614`.

**`board_init` (`0x08011f2c`) — board GPIO + sub-inits.** Enables the GPIOA/B/C/F
port clocks (`RCC_AHBENR` bits 17/18/19/22), drives initial output levels
(GPIOA `0x8180` high / `0x1200` low, GPIOB `0x3001` high / `0x8e86` low), then
configures the board pins with a single reused `gpio_pin_cfg_t` (the OEM does
not re-zero it, so unset fields carry over between calls):

| Call | Port | Pins | Mode | Notes |
| --- | --- | --- | --- | --- |
| 1 | GPIOC | `0x2000` (PC13) | input | AFE INT line |
| 2 | GPIOA | `0x0c00` (PA10,11) | input | |
| 3 | GPIOA | `0x9380` (PA7,8,9,12,15) | output, speed 3 | |
| 4 | GPIOB | `0x4140` (PB6,8,14) | input | **inherits speed 3** from call 3 |
| 5 | GPIOB | `0xbe87` | output, speed 3 | |

Runs six peripheral sub-inits (`FUN_08012188`, `FUN_0801647c`, `FUN_08008804`,
`FUN_08010d90`, `FUN_0800e910`, `FUN_0800a310`), then an **I2C bus-recovery
loop**: while SDA (PB14, `0x4000`) reads low, pulse SCL (PB13, `0x2000`) ten
times at ~1 ms/edge (gated on the `0x2000077c` tick-flag bit0), re-checking SDA
after each burst. Finishes with `FUN_0800e32c` and `HAL_NVIC_SetPriority`/
`EnableIRQ` for **EXTI4_15 (IRQ 7)** at priority 3.

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

### RTC + measurement ADC

`rtc_timestamp_read` (`0x080121f0`) reads the RTC via the HAL handle at
**`0x200006f0`** (Instance = RTC base `0x40002800`; `RTC_TR` @+0 masked
`0x007F7F7F`, `RTC_DR` @+4 masked `0x00FFFF3F`), BCD-decodes the six fields with
`bcd_to_bin` (`(x&0xf)+(x>>4)*10`) through a scratch pair at `0x20000730` into an
8-byte stamp at **`0x20000718`** (sec, min, hour, 0, day, month&0x1F, year, 0).
The dispatcher copies that into its trace records.

`rtc_set_time` (`0x0801c288`) / `rtc_set_date` (`0x0801c410`) program `RTC_TR` /
`RTC_DR` through the same handle. Both follow the HAL write sequence: unlock with
`WPR`(Instance +0x24) ← `0xCA`,`0x53`; enter init mode (`RTC_EnterInitMode`); write
the masked register; clear `ISR.INIT` (Instance +0xc bit7) to leave init; if
`CR.BYPSHAD` (Instance +0x8 bit5) is clear, `RTC_WaitForSynchro`; re-lock `WPR` ←
`0xFF`. The handle's `Lock`/`State` bytes live at **+0x1c / +0x1d**. SetTime reads
`DayLightSaving`/`StoreOperation` at struct **+0xc / +0x10** even for a BIN-format
set, so callers must pass a word-aligned, fully-zeroed time struct.

### Power-on identity / version header

`bms_system_init` (`0x0801156c`) reads the image's version words straight from
flash: **`0x08007ff8`** (byte-reversed into a 3-char BL-version string) and
**`0x08008004`** → cached at **`0x200006e8`**. A match against the magic
**`0x011105b2`** (= firmware **v1.11.05**) gates the stored HW-id refresh; the
sub-fields are the byte at `0x200006e9` and the halfword at `0x200006ea`, mirrored
into the BMS record at +0x5d / +0x52. The 28-byte block at flash `0x08008000` is
copied to **`0x20000558`**. The wake reason is RTC backup register `BKP0R` (read
via `rtc_backup_read`) cached at **`0x20000724`**; bit24 set means "discharge empty".
The descending voltage→SOC table at **`0x0801e620`** maps the measurement at
`0x200003d2` to the wake-up SOC (`bms_soc_preset`).

### Channel-1 UART drain (`uart_flush_ch1`, `0x080161b4`)

The host UART HAL handle is at **`0x200007ac`**; its `gState` byte at **+0x69**
tracks transmit progress (`HAL_UART_STATE_RESET` 0, `READY` `0x20`, `BUSY_TX`
`0x21`). The ch-1 TX ring is a 20-byte buffer at **`0x20000784`** with tail
**`0x2000084a`** and head **`0x2000084e`**. `uart_ch1_tx_pump` (`0x08016110`), when
gState is READY and the ring is non-empty, pops `ring[tail]`, advances tail
(wrapping at 20), marks the handle BUSY_TX, writes the byte to USART `TDR`
(Instance +0x28) and sets `CR1.TXEIE` (bit7). Before an OTA/Modbus reset,
`uart_flush_ch1` calls it and spins until `gState` returns to READY so the final
reply leaves the wire.

### BMS record / error-log EEPROM layout

`bms_record_load` (`0x08013f80`) reads the 128-byte BMS record from the I2C EEPROM
(device `0xa0`, address `0xff80`, handle `0x20000434`) into the record at
**`0x200004d0`** and checks the stored CRC word at **+0x7c** against a CRC over the
first 0x1f words. `bms_errlog_load` (`0x08014140`) reads a 64-byte error-log record
at EEPROM offset **`index * 0x40`** into a scratch buffer, checks the CRC word at
**+0x3c** over the first 0xf words, and on success copies it to **`0x200005b0`**.
Both retry up to ten times on a read failure or CRC mismatch.

The 3-cell measurement ADC is started in interrupt mode by `adc_start_it`
(`0x080195d0` = HAL_ADC_Start_IT) over the HAL handle at **`0x200001b4`**
(Instance ISR @+0 ← `0x1C`, IER @+4 ← EOC/EOS/OVR, CR @+8 bit2 = ADSTART; handle
lock @+0x40, State @+0x44, ErrorCode @+0x48). `bms_measure_prime` calls it on the
first sample-window iteration.

When the handle's `NbrOfConversion` field isn't 1, `adc_start_it` first runs
`adc_enable` (`0x080198d0` = HAL `ADC_Enable`): sets `CR.ADEN` (bit0) only when no
calibrate/stop/convert/disable bit is in flight (`CR & 0x80000017 == 0`), spins a
stabilization delay of **`SystemCoreClock/1000000`** iterations, then polls
`ISR.ADRDY` (bit0) with a 2-tick timeout (`tick_get` @ `0x20002614`). The clock
figure is the OEM **`SystemCoreClock`** global at **`0x200000C0`** — the first word
of `.data` (`_sdata`, see `memory-map.md`); it is read, never written, by the ADC
path.

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

`state_persist_to_backup` (`0x08014280`) then mirrors the state across reset: the
identity/state word **`0x20000724`** is a packed u32 — byte 0 = 0, byte 1 =
current state, byte 2 = previous state, byte 3 = mode/balance flags (bit 24 is the
balance enable the state handlers toggle). On every transition its low three bytes
are repacked, the 32-bit complement is staged at **`0x20000698`**, and both words
are written to **RTC backup registers BKP0/BKP1** (RTC HAL handle `0x200006f0`,
`RTC+0x50`/`+0x54`) via `rtc_backup_write`. modbus.c/ota.c persist a single status
byte the same way; the cross-check word/complement guards against backup-domain
corruption.

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
