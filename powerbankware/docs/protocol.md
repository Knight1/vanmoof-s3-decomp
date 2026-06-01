# powerbankware — host/comms protocol

> Seed. The command/telemetry processor has not been decompiled yet; the
> structure below is the working hypothesis from strings + the batteryware
> sibling, to be confirmed against THIS binary.

## Expected: Modbus RTU (same family as batteryware)

powerbankware shares batteryware's BMS core, so the inter-module link is
expected to be **Modbus RTU** (9600 8N1, CRC-16 poly 0xA001) with the same
register model — see `../batteryware/docs/protocol.md` and the authoritative
host map in [Knight1/vanmoof-bms]. The telemetry string

```
\nSend SN = %x %x %x %x %x, Version = %w, SOC = %d, SOH = %d, CycleCount = %i, State = %x\r
```

confirms the report carries SN / version / SOC / SOH / cycle-count / state — the
same fields batteryware exposes. Slave address, function codes (3/6/0x10) and
the register layout are **TBC** (decompile the command processor; cross-check
against the batteryware register map but verify each against this image).

## FEDL5236 AFE link — hardware SPI (confirmed from this image)

The battery-AFE side channel is **hardware SPI**, not the bit-banged SMBus
batteryware uses. CS is GPIOA **PA15** (bit `0x8000`), driven low before each
transfer; the HAL SPI handle lives at SRAM `0x20000634`. Framing
(`src/fedl5236.c`):

| Op | TX bytes | Notes |
| --- | --- | --- |
| Command write | `(reg<<1)\|0x80`, `val`, `crc8` | 3-byte frame; reg 9 also cached to a shadow byte `0x20000412` |
| Read data | `(reg<<1)\|0x81`, `count`, `0xff` | reads `count+3` back; CRC-8 is the trailing byte, checked against `crc8(rx[0..count+1])` |

Bit 0 of the command byte selects write(0)/read(1); bit 7 is set on both. The
CRC-8 is computed by a runtime-installed veneer (`0x0801db48`) — polynomial TBC
when that routine is located. Each transfer phase is bounded to ~10 ms of
1 ms software ticks (flag byte `0x2000077c` bit 0) before giving up.

Key registers (from `fedl5236_initialize` / `fedl5236_powerdown`):

| Reg | Use |
| --- | --- |
| 0x03 | status (bit0 = cell data ready, bit1 = total-voltage ready, low nibble = balance) |
| 0x05/0x06/0x08 | conversion triggers (total-voltage / zero-offset / charger) |
| 0x07 | TS select (0x81 = TS0, 0x83 = TS1, 0 = off) |
| 0x09..0x11 | default-setting register block |
| 0x0C | POWER_DOWN: read bit7 = PUPIN ready; write 0x10 = enter power-down |
| 0x1A | 10 cell voltages (20 bytes, raw·5000/4095 mV) |
| 0x2E | total voltage (2 bytes) |
| 0x30/0x32 | TS0/TS1 NTC ADC |
| 0x34 | charger voltage (raw·19536/1000 mV) |

### Extend_IO LED-bar (second device on the SPI bus)

A separate I/O expander shares the SPI bus (handle/buffers) with CS on **PA8**.
`extend_io_update` writes a single level byte (`0xFC/0xF4/0xE4/0xC4/0x84/0x00`
for the 5-segment bar, `0xFA` = all-on/fault) derived from BMS state + SOC,
folding mode bit 12 into bit 0. Same retry/timeout framing as the AFE writes;
errors logged as "Extend_IO_Process()".

## Calibration / test entry (from main)

A boot-mode pre-check (before the super-loop) branches on a stored selector:
- `0x17`/`0x18` → calibration/test paths
- `0x12` → `FUN_080093ac` (guarded by limit 0x51)
- `0x13` → `FUN_08009598` (limit 0x2c)
- `0x14` → `FUN_0800aa98` (limit 99)
- `0x15` → `FUN_0800ac44` (limit 0x18)

These align with the `CHG CAL` / `DSG CAL` / temperature-cal strings.

## Power-path control (powerbank-specific — TBC)

Not present in batteryware. Strings imply a control surface for:
- **Bypass FET**: `ByPass On` / `ByPass Off` (+ "Battery Low" gating)
- **Charger/load detection**: `Charger In & Load Exist → ByPass On`,
  `Charger In & Load Absent → ByPass Off`, `No Load`, `Charger Absent`
- **Output (Vout) regulation via DAC**: `DAC_Value= %l mV`, `DAC_Stop()`,
  `DAC Over Range`, `Vout <20V over 3Sec`, `Vout <30V over 30min`
- `Check_Charger_Voltage` / `Check_PUPIN`

Whether these are host-commandable (Modbus registers) or purely autonomous is
to be determined when the power-path module is decompiled.
