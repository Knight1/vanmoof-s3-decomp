# UART HAL init path (`uart.c`)

The S3 main controller brings every serial link up through the stock
**ST CubeF4 `stm32f4xx_hal_uart.c`** init/config/deinit path. This module is the
faithful reconstruction of that path plus the VanMoof IRQ-driven TX primitive.

Two layers live in `uart.c`:

* **Bring-up (CubeF4 HAL).** `HAL_UART_Init` → `UART_SetConfig` configure the
  framing registers and program BRR; `HAL_UART_DeInit` tears the peripheral
  back down. The eight `uartX_init`/`usartX_init` wrappers each populate a
  `UART_HandleTypeDef` and call `HAL_UART_Init`.
* **Runtime TX (VanMoof).** `uart_send_byte` pushes a byte into the TX ring
  buffer with the peripheral TX interrupt masked for atomicity. The blocking
  `HAL_UART_Transmit`/`HAL_UART_Receive` entry points are **not** used by this
  firmware — runtime traffic is entirely IRQ + ring-buffer.

Reconstructed from the OEM functions at `0x08026AF0` (`UART_SetConfig`),
`0x08026CFC` (`HAL_UART_Init`), and `0x08026D5A` (`HAL_UART_DeInit`),
adversarially verified against the raw disassembly. Behaviour-equivalent (not
byte-identical): the BRR magic-multiplies are written as the plain divisions
they implement.

## Handle layout (`UART_HandleTypeDef`)

Standard CubeF4 layout; offsets verified against the binary.

| Offset | Field | Notes |
| --- | --- | --- |
| `+0x00` | `Instance` | `USART_TypeDef *` (peripheral base) |
| `+0x04` | `Init` | `UART_InitTypeDef` (7 words) |
| `+0x20` | `pTxBuffPtr` | (unused by the HAL init path) |
| `+0x24` | `TxXferSize` | |
| `+0x26` | `TxXferCount` | |
| `+0x28` | `pRxBuffPtr` | |
| `+0x2C` | `RxXferSize` | |
| `+0x2E` | `RxXferCount` | |
| `+0x30` | `hdmatx` | |
| `+0x34` | `hdmarx` | |
| `+0x38` | `Lock` | `0` unlocked / `1` locked (byte) |
| `+0x39` | `gState` | `0`=RESET, `0x20`=READY, `0x24`=BUSY (byte) |
| `+0x3A` | `RxState` | `0`=RESET, `0x20`=READY (byte) |
| `+0x3C` | `ErrorCode` | `0`=NONE (word) |

`UART_InitTypeDef`: `BaudRate +0x00`, `WordLength +0x04`, `StopBits +0x08`,
`Parity +0x0C`, `Mode +0x10`, `HwFlowCtl +0x14`, `OverSampling +0x18`.

`USART_TypeDef` register block: `SR +0x00`, `DR +0x04`, `BRR +0x08`,
`CR1 +0x0C`, `CR2 +0x10`, `CR3 +0x14`, `GTPR +0x18`.

## Function map

| OEM | Function | Role |
| --- | --- | --- |
| `0x08026CFC` | `HAL_UART_Init` | MspInit gate, disable UE, `UART_SetConfig`, clear CR2/CR3 mode bits, enable UE |
| `0x08026AF0` | `UART_SetConfig` | program CR1/CR2/CR3 framing + BRR (static) |
| `0x08026D5A` | `HAL_UART_DeInit` | disable UE, MspDeInit, back to RESET |

Return codes: `0`=HAL_OK, `1`=HAL_ERROR.

## Register masks (verified)

* **`HAL_UART_Init`** — `CR1 &= ~UE` (`0x2000`) before config, `CR1 |= UE` after;
  `CR2 &= ~(LINEN|CLKEN)` (`0x4800`); `CR3 &= ~(SCEN|HDSEL|IREN)` (`0x2A`).
* **`UART_SetConfig`** — `CR2 = (CR2 & ~STOP[0x3000]) | StopBits`;
  `CR1 = (CR1 & ~0x960C) | OverSampling | WordLength | Parity | Mode`
  (`0x960C` = `M|PCE|PS|TE|RE|OVER8`); `CR3 = (CR3 & ~(RTSE|CTSE)[0x300]) | HwFlowCtl`.

## Baud-rate generation (verified)

Clock source: **PCLK2** for the APB2 instances `USART1` (`0x40011000`),
`USART6` (`0x40011400`), `UART9` (`0x40011800`), `UART10` (`0x40011C00`);
**PCLK1** for every other USART/UART. The OEM evaluates the numerator
`pclk * 25` as a 64-bit dividend (`__aeabi_uldivmod`, `0x08020AE4`); the C uses a
`uint64_t` divide. The `0x51EB851F` constant with `>>37` is the reciprocal for
÷100, written as `/100`.

| OverSampling | `usartdiv` | `fraction` | `BRR` |
| --- | --- | --- | --- |
| OVER8 (`0x8000`) | `(pclk*25)/(2*baud)` | `((rem)*8 + 50)/100` | `mant<<4 \| (frac&0xF8)<<1 \| frac&0x07` |
| OVER16 (else) | `(pclk*25)/(4*baud)` | `((rem)*16 + 50)/100` | `mant<<4 \| (frac&0xF0) \| frac&0x0F` |

where `mant = usartdiv/100` and `rem = usartdiv - mant*100`. The OVER8 and
OVER16 branches were specifically confirmed **not swapped** against the
disassembly.

## Out of scope (still external)

* `HAL_UART_MspInit` (`0x080333C0`) — per-USART GPIO alt-function + RCC
  clock-enable + NVIC priority/enable (one branch per instance); pulls in
  `HAL_GPIO_Init`, `nvic_set_priority`, `nvic_enable_irq`.
* `HAL_UART_MspDeInit` (`0x08033740`).
* `HAL_RCC_GetPCLK1Freq` (`0x08027374`) / `HAL_RCC_GetPCLK2Freq` (`0x08027394`,
  = `HAL_RCC_GetHCLKFreq` `0x08027368` shifted by the APB2 prescaler).
