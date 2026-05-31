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
| SYSCFG | `0x40010000` | `+0x00 |= 3` → MEM_MODE=3 (SRAM remap) |
| RCC | `0x40021000` | `+0x18`, `+0x1C` clock enables |
| RTC | `0x40002800` | HAL RTC handle instance |
| USART1 | `0x40013800` | (F0 APB2) — Modbus link, TBC |
| DAC | `0x40007400` | (F0 APB1) — Vout regulation, TBC |
| GPIO | `0x48000000`? | F0 AHB convention — **confirm against pin code** |

## SRAM globals (from main @ `0x0800f52c`)

| Address | Role |
| --- | --- |
| `0x200006A0` | **mode / cfg word** (gated on bits &1 / &3>>1 / &7>>2, like batteryware `s_bms_cfg`) |
| `0x200005AC` | **current state** byte (jump-table index, `< 0x1c`) |
| `0x0801E6EC` | **state-handler jump table** (flash, 28 entries) |
| `0x20000724` | version / ID block (main reads `+1`, `+2`) |
| `0x20000218` | value/limit block (cal threshold checks: 0x51 / 0x2c / 99 / 0x18) |
| `0x200003CE` / `0x200006A0` | boot-delay counter vs `0x752F` threshold |

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
