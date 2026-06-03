# batteryware — Modbus RTU register interface

The BMS is a **Modbus RTU slave** on the USART1 line: **9600 baud, 8N1**,
slave address **`0xAA`**, CRC-16 (poly 0xA001). The host (main module /
service tool) reads and writes 16-bit holding registers.

The firmware side is `uart_protocol_handler` (FUN_0800afa4), fed byte-by-byte
from the RX ring by `uart_resp_handler`. The RX state machine accepts a frame
only when the function-code byte is one of **{3, 6, 0x10}** (mask `0x10048`),
i.e. the three Modbus functions the BMS implements:

| Func | Modbus operation | firmware path |
| --- | --- | --- |
| `0x03` | Read Holding Registers | cmd-3 telemetry cascade |
| `0x06` | Write Single Register | `modem_command_dispatch` (cw = register) |
| `0x10` | Write Multiple Registers | `flash_stream_handler` (calibration / OTA) |

## Frame format (RTU)

```
Read  (0x03) req:  AA 03 regHi regLo  qtyHi qtyLo   CRClo CRChi
Read  (0x03) rsp:  AA 03 byteCount  <reg data, 2B each, big-endian>  CRClo CRChi
Write (0x06) req:  AA 06 regHi regLo  valHi valLo   CRClo CRChi
```

CRC-16 is the standard Modbus CRC (poly 0xA001), appended little-endian. In
the read response, `byteCount = qty*2`. The firmware emits register *r* only
while the request's start register `< r+1`, so a read of `[start, start+qty)`
streams those registers back in order — read `start=0, qty=45` for the full
live snapshot (what the host's live monitor does).

## Holding register map (function 0x03 — read)

Voltages are in **mV**; temperatures use `°C = (int16(reg) − 2731) / 10`
(register is deci-Kelvin); current is **signed**, `mA = int16(reg) × 10`.

| Reg | Hex | Field | Scaling | Firmware source |
| ---: | --- | --- | --- | --- |
| 2  | 0x02 | **Protection / shutdown status** | bitfield (see below) | composed from `g_fault_flags` `0x20002C44` |
| 3  | 0x03 | Battery temperature | (i16−2731)/10 °C | hottest of the two pack NTCs |
| 4  | 0x04 | **Pack voltage** | mV | Σ cell voltages `0x2000281C` |
| 5  | 0x05 | RSOC (relative state of charge) | % | `bms_ctx+0x36` (`0x200029DE`) |
| 6  | 0x06 | **Current** | i16×10 mA (− = discharge) | coulomb accumulator `0x200028C0` |
| 7  | 0x07 | Charging status | bitfield | `fg_charge_status()` |
| 8  | 0x08 | Discharging status | bitfield | `fg_status_flag_get()` |
| 9  | 0x09 | Test/Debug mode | flag | `cfg_blk+5` (`0x200028D5`) |
| 10 | 0x0A | Hardware version | hex | `cfg_blk+2` |
| 11 | 0x0B | Software version | hex | `cfg_blk+0` |
| 12–18 | 0x0C–0x12 | **ESN** (14-byte ASCII serial) | string | EEPROM `0x0808000F`+ |
| 19–20 | 0x13–0x14 | Manufacture date | bytes Y/M/D | EEPROM (read-back) |
| 21 | 0x15 | Design / normal capacity | mAh | `cfg_blk+6` |
| 22 | 0x16 | Full-charge capacity | mAh | `bms_ctx+0x28` |
| 23 | 0x17 | Remaining capacity | mAh | `bms_ctx+0x2C` |
| 24 | 0x18 | Absolute SOC | % | `bms_ctx+0x37` |
| 25 | 0x19 | Cycle count | count | `bms_ctx+0x34` |
| 26 | 0x1A | MOS control / status | flag | bit 1 of `mode_flag` `0x20002870` |
| **27–36** | **0x1B–0x24** | **Cell 1–10 voltage** | mV | `0x200028A4 + 2·(reg−27)` |
| 37 | 0x25 | Temperature sensor 1 | (i16−2731)/10 °C | NTC ch1 `0x20002589` |
| 38 | 0x26 | Temperature sensor 2 | °C | NTC ch2 `0x2000258A` |
| 39 | 0x27 | Discharge-MOSFET temperature | °C | `0x20002588` |
| 40 | 0x28 | Warnings | bitfield | `0x20002C0A` |
| 41 | 0x29 | Maximum cell voltage | mV | `0x200027FA` |
| 42 | 0x2A | Minimum cell voltage | mV | `0x2000282A` |
| 43 | 0x2B | Cell-balance mask | bits | `0x20002820` |
| 44 | 0x2C | Bootloader version | hex | bootloader flash `0x08004FE4` (see note) |
| 71–86 | 0x47–0x56 | Protection trip thresholds (DOTP…SCP, see below) | per-protection | `bms_ctx`-region |

> **Reg 0x2C (bootloader version).** The app reads three hex-ASCII chars in the
> bootloader's flash at `0x08004FF9/FA/FB` (= `0x08004FE4 + 0x15/16/17`). Only
> if `[0x4FFA]` and `[0x4FFB]` are ASCII `'0'` does it parse them — as
> `reg = (nibble[0x4FFB]<<8) | (nibble[0x4FFA]<<4) | nibble[0x4FF9]` — otherwise
> it returns the hardcoded default **`0x0004`**. So the slot stores the version
> digit at `0x4FF9` with `"00"` following (only single-hex-digit versions fit).
> Verified against two real bootloaders:
>   - **BL V004** (2019-11-19, on shipped packs): slot is unprogrammed
>     (`00 00 00`) → condition false → fallback `0x0004`. Banner at `0x080049C1`.
>   - **BL V007** (2022-11-04, `bmsboot.bin` in the firmware archive): slot is
>     `'7' '0' '0'` → `0x0007`. Banner at `0x08004919`.

### Cell mapping (the headline)

The 10S pack's live per-cell voltages are read from the FEDL5236 AFE into
`0x200027D4` (`s_cell_voltage_table`, u16 mV) and copied by `FUN_080138ac`
to the telemetry mirror `0x200028A4`. So:

> **Register `27 + (n−1)` = cell *n* voltage in mV**, for n = 1…10
> (registers 0x1B…0x24). Pack total = register 4; max/min cell = registers
> 41/42; the active cell count is `cfg_blk[4]`.

(The AFE per-cell *die* temperatures are tracked separately at `0x20002880`
for balancing and are not in this read map; registers 3/37/38/39 are the
board NTC + MOSFET thermistors.)

### Register 2 — protection / shutdown bitfield

Non-zero means the pack has shut down. Bit → flag (LSB first):

```
0 DOTP   1 DUTP   2 COTP   3 CUTP    4 DOCP1  5 DOCP2  6 COCP1  7 COCP2
8 OVP1   9 OVP2  10 UVP1  11 UVP2   12 PDOCP 13 PDSCP 14 MOTP  15 SCP
```

The same 16 protections have their (best-guess, DynaPack-undocumented) trip
**thresholds** exposed as read registers 71–86 (`0x47`–`0x56`), in the same
order DOTP…SCP.

## Write commands (function 0x06 — write single register)

Writing a register issues function `0x06`; the firmware dispatches on the
register number as the command word `cw` in `modem_command_dispatch`. Verified
host commands and their firmware handlers:

| Reg (cw) | Write | Effect | Firmware handler |
| --- | --- | --- | --- |
| `0x01` | 0 | Ship mode | `cw==1` → default tail (bootloader hook) |
| `0x08` | 0/1 | Discharge MOSFET off/on | `cw==8` → config tables |
| `0x09` | 0/1 | Test/Debug mode off/on | `cw==9` → `arm_cfg_flag` (persists `cfg_blk+5`) |
| `0x0A` | 0 | Reset ESN | `cw==10` → `arm_tick_persist` |
| `0x1A` | 0/1 | Charge MOSFET off/on | `cw==0x1a` → `arm_state_bit` (toggles ctrl bit 12 / mode bit 1, `bms_configure`) |
| `0x80` | 0 | Reset MCU / firmware update | `cw==0x80` → `oad_firmware_update` |

Counter/calibration writes use command words `0xF020`–`0xF023` (counter
stores), `0xF45` (history hex-dump readout), and `0x95` (a separate
ship/idle path).

## Function 0x10 — calibration & OTA streaming

A variable-length write-multiple frame handled by `flash_stream_handler`:

```
AA 10 cwHi cwLo  qtyHi qtyLo  len  <payload …>  CRClo CRChi
```

accepted once ≥10 bytes are in and `qty == len/2`, CRC-16 over `len+7` bytes.
Two payload modes by command word `cw`:

- **`cw < 0x15`** — write the pack **identity** into EEPROM
  `0x0808000F`…`0x08080020`: the 14-byte **ESN** (regs `0x0C`–`0x12`) then the
  4-byte **manufacture date** `[0x00,year,month,day]` (regs `0x13`–`0x14`),
  one register/cell pair per command-word step, gated on an anti-replay tick
  triplet at EEPROM `0x08080021/25/29`. See `docs/eeprom.md` §3.
- **`cw == 0x82`** — stream an OTA image into the flash staging area at
  `0x0801A800` page-by-page (page address in `0x200047CC`, scratch
  `0x2000474C`), verified per 0x80-byte page. The cmd-0x80 reset then copies
  the staged image down to the application area `0x08000000` and CRC-checks it
  against `0x08004FFC`.

## ASCII `KEY=VALUE` console (secondary)

When the control word `0x20002C00` selects command mode, printable bytes are
*also* accumulated into a line buffer (`0x20004510`, max 0x2c) and dispatched
on CR to `command_parser` (FUN_08009ac4), a 23-entry name table: `Who?`, `Now?`,
`PF`, `Reset BMS`, `DF`, `Upgrade AP`, `Upgrade BL`, `Into BootLoader`,
`CHG CAL`/`CHG CAL?`, `DSG CAL`/`DSG CAL?`, `Reset ESN`, `Log Clear`,
`TS0/1/2`(+`?`), `TS Reset`, `FCC`, `SOC`. After matching, `command_parser`
tail-jumps (`mov pc`) into a 24-pointer dispatch table at runtime
`0x08017FD8` (file `0x17fd8` in `bmsv007.bin`; Ghidra `0x08012FD8` via the
−0x5000 runtime/Ghidra offset). The handlers run in `command_parser`'s own
frame. The `Reset BMS` handler (action 4, runtime `0x0800ef20`) arms a reset
flag at SRAM `0x20002C48`, persists it to data-EEPROM `0x08080001`, prints
`"\nOK\r"`, flushes the UART, then `NVIC_SystemReset()`
(`SCB->AIRCR = 0x05FA0004`). When the control word selects bootloader mode
instead, bytes go to the YMODEM receiver (`ymodem_receive`).
