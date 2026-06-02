# powerbankware — host/comms protocol

> Seed. The command/telemetry processor has not been decompiled yet; the
> structure below is the working hypothesis from strings + the batteryware
> sibling, to be confirmed against THIS binary.

## Expected: Modbus RTU (same family as batteryware)

powerbankware shares batteryware's BMS core, so the inter-module link is
expected to be **Modbus RTU** (CRC-16 poly 0xA001) with the same register model
— see `../batteryware/docs/protocol.md` and the authoritative host map in
[Knight1/vanmoof-bms]. The physical link is now confirmed from `uart_msp_init`
(FUN_0801647c): **USART2 @ 115200 8N1** (BRR built from `0xe1<<9`), TX/RX on
PA2/PA3 (AF1), with RXNEIE-driven receive — this is the `s_handle` (`0x20001a60`)
that `log_print`/`uart_flush` and the command processor use. (A second link sits
on USART1, handle `0x200007ac`.) The telemetry string

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

Now decompiled (`src/transitions.c` + `src/vout.c`): the power-path is driven
**autonomously** by the state machine — the bypass FET (PA12) and DAC set-point
are sequenced inside the per-state entry routines, not from host commands. The
charger/load strings are emitted by the state handlers as status logs.

## Periodic status frame (`status_frame_emit` @ `0x0800f02c`)

Beyond Modbus, the firmware emits a packed status frame each cycle (built in TX
scratch `0x2000048c` via the length-prefixed sub-block packer `FUN_0800ef70`,
big-endian fields) and logs the human-readable form:

```
Send SN = %x %x %x %x %x, Version = %w, SOC = %d, SOH = %d, CycleCount = %i, State = %x
```

Four sub-blocks (tags 0/1/2/3): serial-number bytes (record +0x66..+0x6b),
firmware version (`0x200006e8+2`) + SN tail (`0x200006e8+1..3`), SOC (record
+0x5a), and SOH (record +0x5b) + cycle count (record +0x50, u16) + state
(`0x200004b4`). Each sub-block is closed by `crc16_append` (OEM `FUN_0800ef70`):
the standard Modbus CRC-16 (init `0xFFFF`, reflected poly `0xA001`, no final XOR)
computed over the block and stored little-endian at its tail — the same algorithm
as `modbus_crc16` (`FUN_08019094`), but a distinct compute-and-append routine.

## OTA image header (`image_verify_ap` @ `0x0800fe48`)

A firmware image staged over the OTA channel (into staging `0x08024000`, then
copied to AP `0x08008000` / BL `0x08000000` by `image_copy`) carries a 40-byte
(`0x28`) VanMoof header ahead of its payload. `image_verify_ap` validates the
AP/staging form via the hardware CRC unit (handle `0x200006c0`):

| Off | Word | Field | Notes |
| --- | --- | --- | --- |
| `0x00` | `[0]` | magic | must be `0xAA55AA55`, else result 2 (bad header) |
| `0x04` | `[1]` | — | not checked; included in the CRC as-is |
| `0x08` | `[2]` | stored CRC | the value the computed CRC is compared to |
| `0x0c` | `[3]` | length (bytes) | must be `≤ 0x1c000` (the AP region size `0x08024000 − 0x08008000`), else result 2 |
| `0x10` | `[4..9]` | header tail | included in the CRC as-is |
| `0x28` | `[10..]` | payload | vector table + code (HAL bring-up remaps vectors from image+`0x28`) |

The CRC covers the **whole image with two header fields neutralised**: the
header (10 words) is copied out and its stored-CRC (word 2) and length (word 3)
are blanked to `0xFFFFFFFF` before being fed (`crc_accumulate` — reset + feed),
then the body — `(length − 0x28) / 4` words from `+0x28` — is folded on without a
reset (`crc_continue`). The result is compared to the stored CRC (word 2):
**0 = valid, 1 = CRC mismatch, 2 = bad header**.

The BL region instead uses the simpler raw check `image_verify` (`FUN_0800fed8`):
a CRC over the first `0x1fff` words compared to a value stored at `+0x7ffc`.
