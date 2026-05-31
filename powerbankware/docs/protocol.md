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
