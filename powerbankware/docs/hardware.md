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

## Power-path layer (powerbank-specific — strings)

Output stage absent from batteryware: **bypass FET** (`ByPass On/Off`),
**charger/load detection** (`Charger In & Load Exist/Absent`, `No Load`),
**DAC-regulated Vout** ~20–30 V (`DAC_Value= %l mV`, `Vout <20V over 3Sec`,
`Vout <30V over 30min`, `DAC Over Range`), `Check_Charger_Voltage` / `Check_PUPIN`.

## Shared BMS core (= batteryware)

FEDL5236 AFE (`FEDL5236_Initialize/Default_Setting/Zero_Current_Offset/
Total_Voltage_Check/Process/Command_Write/Read_Data/PowerDown`), SOC/SOH/cycle
telemetry, protections (COTP/CUTP/DOTP/DUTP/OV-2nd/MOS-failure), CHG/DSG
calibration, shipping mode, AP/BL/Shadow image copy. AP id: `"\nI am VM-BATT AP\r"`.
