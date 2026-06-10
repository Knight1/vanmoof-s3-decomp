# I²C HAL driver (`i2c.c`)

The S3 main controller drives every on-board I²C peripheral through the stock
**ST CubeF4 `stm32f4xx_hal_i2c.c`** blocking master/memory driver. This module
is the faithful reconstruction of that driver's transfer layer plus the two
VanMoof handle-management wrappers.

Two physical buses (see `docs/hardware.md`):

* **I²C3** (`0x40005C00`, 100 kHz, 7-bit) — config EEPROM (`0xA0`), STC3115
  gas-gauge, LIS3DH accelerometer. Handle `g_i2c3_handle` @ SRAM `0x20009B04`.
* **I²C2** (`0x40005400`, 400 kHz) — the two IS31FL3236 display drivers
  (`0x60`/`0x66`), the panel sub-controller + ambient light sensor (`0x20`),
  the MAX9768 audio amp. Handle @ `0x20009BB8`.

The transfer functions are bus-agnostic — every caller passes its own
`I2C_HandleTypeDef *`. `HAL_I2C_Master_Transmit` (`0x08024760`) alone is reached
from `audio_amp_init`, `display_send_init_cmd`, `display_write_reg20_init`,
`eeprom_read_id_block`, `led_driver_brightness_write`, `led_driver_panel_config`,
`led_driver_standby_write`, `light_sensor_i2c_read`, and more.

Reconstructed from the OEM functions at `0x08023EEC..0x08025170`, adversarially
verified against the raw disassembly. Behaviour-equivalent (not byte-identical):
the clock-config magic-multiplies are written as the plain divisions they
implement.

## Handle layout (`I2C_HandleTypeDef`)

Standard CubeF4 layout; offsets verified against the binary.

| Offset | Field | Notes |
| --- | --- | --- |
| `+0x00` | `Instance` | `I2C_TypeDef *` (peripheral base) |
| `+0x04` | `Init` | `I2C_InitTypeDef` (8 words) |
| `+0x24` | `pBuffPtr` | live data pointer |
| `+0x28` | `XferSize` | loop counter |
| `+0x2A` | `XferCount` | remaining bytes |
| `+0x2C` | `XferOptions` | sequential-frame option (here always `I2C_NO_OPTION_FRAME`) |
| `+0x30` | `PreviousState` | gates the repeated-START decision |
| `+0x3C` | `Lock` | `0` unlocked / `1` locked |
| `+0x3D` | `State` | `0`=RESET, `0x20`=READY, `0x21`=BUSY_TX, `0x22`=BUSY_RX, `0x24`=BUSY |
| `+0x3E` | `Mode` | `0`=NONE, `0x10`=MASTER, `0x40`=MEM |
| `+0x40` | `ErrorCode` | bit `0x04`=AF, `0x20`=TIMEOUT, `0x200`=WRONG_START |

The `I2C_TypeDef` register block: `CR1 +0x00`, `CR2 +0x04`, `OAR1 +0x08`,
`OAR2 +0x0C`, `DR +0x10`, `SR1 +0x14`, `SR2 +0x18`, `CCR +0x1C`, `TRISE +0x20`.

## Flag encoding

The wait helpers take a CubeF4 flag token `(reg << 16) | bit`: the high half
selects the status register (`0x0001` → SR1, `0x0010` → SR2), the low half is
the bit mask. Values read verbatim from the literal pools:

| Token | Value | Reg / bit |
| --- | --- | --- |
| `I2C_FLAG_SB` | `0x00010001` | SR1 bit 0 (start) |
| `I2C_FLAG_ADDR` | `0x00010002` | SR1 bit 1 (address sent/matched) |
| `I2C_FLAG_BTF` | `0x00010004` | SR1 bit 2 (byte transfer finished) |
| `I2C_FLAG_ADD10` | `0x00010008` | SR1 bit 3 (10-bit header sent) |
| `I2C_FLAG_BUSY` | `0x00100002` | SR2 bit 1 (bus busy) |

## Function map

| OEM | Function | Role |
| --- | --- | --- |
| `0x08024570` | `HAL_I2C_Init` | clock/CCR/TRISE/CR2/OAR config (SWRST, MspInit gate) |
| `0x0802472C` | `HAL_I2C_DeInit` | disable PE, MspDeInit, back to RESET |
| `0x08024760` | `HAL_I2C_Master_Transmit` | blocking N-byte write (BTF double-byte fast path) |
| `0x080248D8` | `HAL_I2C_Master_Receive` | blocking N-byte read (N=1/2/3 ACK/STOP/POS corner cases) |
| `0x08024D2C` | `HAL_I2C_Mem_Write` | EEPROM-style write (Mode=MEM, 8/16-bit mem address) |
| `0x08024E90` | `HAL_I2C_Mem_Read` | EEPROM-style read (repeated START into read) |
| `0x0802405C` | `I2C_MasterRequestWrite` | START + slave write-address (7/10-bit) |
| `0x08024110` | `I2C_MasterRequestRead` | START + slave read-address (7/10-bit, repeated START) |
| `0x08024284` | `I2C_RequestMemoryWrite` | START, write-addr, 1/2-byte memory address |
| `0x0802435C` | `I2C_RequestMemoryRead` | …then repeated START into read-address |
| `0x08023F3C` | `I2C_WaitOnFlagUntilTimeout` | generic flag poll (status = wait-while value) |
| `0x08023FB0` | `I2C_WaitOnMasterAddressFlagUntilTimeout` | ADDR/ADD10 wait, abort+STOP on NACK |
| `0x08024230` | `I2C_WaitOnTXEFlagUntilTimeout` | TXE wait (aborts on NACK) |
| `0x080244B0` | `I2C_WaitOnBTFFlagUntilTimeout` | BTF wait (aborts on NACK) |
| `0x08024504` | `I2C_WaitOnRXNEFlagUntilTimeout` | RXNE wait; STOPF → frame ended; **unconditional** timeout |
| `0x08023EEC` | `I2C_IsAcknowledgeFailed` | AF check + clear + abort |

Return codes everywhere: `0`=HAL_OK, `1`=HAL_ERROR, `2`=HAL_BUSY, `3`=HAL_TIMEOUT.
The `tickstart` is `systick_now()` (HAL_GetTick equivalent); the start-of-transfer
bus-free wait uses the fixed `I2C_TIMEOUT_BUSY` = 25 ms.

## Subtleties preserved (and verified)

* **`Master_Receive` vs `Mem_Read` N≥3 setup** — `Master_Receive`'s pre-loop
  ACK/STOP/POS setup has an `else { CR1 |= ACK }` for N≥3; `Mem_Read`'s does
  **not** (ACK was already set in `I2C_RequestMemoryRead`). This asymmetry is
  real and reproduced.
* **RXNE wait** — no `HAL_MAX_DELAY` escape (timeout always checked); the STOPF
  path clears STOPF and unwinds to READY **without** touching `ErrorCode`.
* **WRONG_START** — on an SB-wait timeout with START still asserted, the request
  helpers write `ErrorCode = 0x200` (`HAL_I2C_WRONG_START`) and return HAL_TIMEOUT.
* **ADDR clear** — `I2C_RequestMemory{Write,Read}` clear ADDR with the standard
  SR1-then-SR2 read pair (both volatile reads emitted).
* **Clock config** — `HAL_I2C_Init` reproduces `I2C_FREQRANGE` (`pclk1/1000000`),
  `I2C_RISE_TIME` (std `freqrange+1`, fast `(freqrange*300)/1000 + 1`), and
  `I2C_SPEED` (standard vs fast, DUTYCYCLE_2 ÷3 vs DUTYCYCLE_16_9 ÷25, the
  `(field+1)&0xFFF==0 → 1` and `(ccr&0xFFC)==0 → 4` clamps), with PCLK1 ≥ 2 MHz
  (standard) / ≥ 4 MHz (fast) validation.

## Out of scope (still external)

* `HAL_I2C_MspInit` (`0x0803C69C`) / `HAL_I2C_MspDeInit` (`0x0803C820`) — GPIO
  alt-function + RCC clock-enable + NVIC setup for I2C1/I2C3; a separate MSP
  cluster (pulls in `HAL_GPIO_Init`, `nvic_*`).
* `HAL_RCC_GetPCLK1Freq` (`0x08027374`) — APB1 clock query (RCC module).

## SRAM handles

| Address | Symbol | Meaning |
| --- | --- | --- |
| `0x20009B04` | `g_i2c3_handle` | I²C3 handle (EEPROM/STC3115/LIS3DH bus) |
| `0x20009BB8` | — | I²C2 handle (display/light-sensor/audio bus) |
