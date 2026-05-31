# batteryware — service/host protocol

The BMS talks to the host (main module / service tool) over USART1. Two
protocols share the link, selected by the control word at `0x20002C00`:

1. a **binary command/telemetry** protocol (`uart_protocol_handler`,
   FUN_0800afa4), and
2. an **ASCII `KEY=VALUE`** line protocol (`command_parser`, FUN_08009ac4),

plus a **YMODEM** firmware path (`ymodem_receive`) when the control word
selects bootloader/upgrade mode.

`uart_resp_handler` (FUN_0800adbc, polled from the main loop) drains the RX
ring buffer at `0x20004088` and routes each byte: to `uart_protocol_handler`
(unless control bit 17 is set), to the ASCII line buffer (`0x20004510`,
flushed to `command_parser` on CR), or to `ymodem_receive`.

## Binary frame format

```
0xAA  CMD  D0 D1 D2 D3  CRClo CRchi      (8 bytes, CMD ∈ {3, 6})
```

The RX state machine accepts a command byte only if `(1 << CMD)` is set in
mask `0x10048` — i.e. **CMD ∈ {3, 6, 0x10}**. For CMD 3/6 an 8-byte frame is
accumulated and validated with CRC-16 over the first 6 bytes (Modbus CRC-16,
poly 0xA001) against `D[6:7]` (little-endian). CMD 0x10 is a variable-length
streaming frame handled separately (see below).

`D0:D1` (big-endian) is the **command word** `cw`; `D2:D3` (big-endian) is
the **argument** `arg`.

## CMD 3 — telemetry report

`cw` is reinterpreted as a **report start index** and `arg` as a **report
count** (stored ×2 at `0x20002C44`-gate `0x200047D4`). The response is built
in `0x20004648` (`0xAA, 0x03, count, …fields…, CRClo, CRChi`) and transmitted.

Every field is emitted only while `report_start_index < N` (a lower start
index ⇒ more fields) and the report count is non-zero. Fields cover pack
cell voltages/temperatures, charge status, fuel-gauge state, the calibration
table read back from EEPROM `0x0808000F..0x08080020`, charger and context
blocks, and high-range diagnostic words (`0xF31..0xF45`). Index 0..2 use a
23-entry **report sub-table** that yields either an aggregate fault word
(composed from `0x20002C44`) or an individual status bitmask.

## CMD 6 — command-word dispatch (`modem_command_dispatch`, FUN_0800ce9e)

| `cw` | action |
| --- | --- |
| `0xF020/F021/F022/F023` | counter-store commands (write `arg`, bump a counter, ACK) |
| `0xF45` | history hex-dump: read a 0x38-byte record from the ext-flash ring buffers (`0x08080200` / `0x08080E00`) and print it |
| `0x95` | shipping mode: charge MOSFET off, enable line low, BMS idle |
| `1..0x1a` | per-command config table (table1) — bit toggles, tick/flag persistence, default echo+bootloader tail |
| `0x80` | firmware-update / OAD bring-up (see below) |
| other | default tail: echo the frame, enter the bootloader hook, run the config-bit tables (table2/table3) |

## CMD 0x10 — streaming (`flash_stream_handler`, FUN_0800d8f0)

A variable-length frame: `0xAA 0x10 cwHi cwLo argHi argLo len <payload> CRClo
CRChi`, accepted once ≥10 bytes and `arg == len/2`, CRC-16 over `len+7`
bytes. Two payload modes:

- **`cw < 0x15`** — bulk-write 16-bit calibration pairs into EEPROM
  `0x0808000F..0x0808001F` (one extra pair admitted per threshold step),
  gated on an anti-replay tick triplet stored at EEPROM `0x08080021/25/29`.
- **`cw == 0x82`** — stream an OTA image into the flash staging area at
  `0x0801A800` page-by-page (page address in `0x200047CC`, scratch buffer
  `0x2000474C`, verified per 0x80-byte page).

On completion it appends a CRC-16 to the buffer and echoes the 8-byte header.

## OAD firmware update (CMD 6, `cw == 0x80`)

Quiesces the pack and re-inits the modem UART. When a full `0x5000`-byte
image has been staged at `0x0801A800` (counter at `0x200047D8`), it
reconfigures the oscillator/clock tree (`rcc_osc_config`, `rcc_configure`,
`rcc_reconfigure`, each retried up to 0x32×) and copies the staged image down
to the application area at `0x08000000` in 0x80-byte DMA-verified pages, then
CRC-checks the result against the word at `0x08004FFC`.

## ASCII `KEY=VALUE` protocol (`command_parser`)

When command mode is active, printable bytes are also accumulated into the
line buffer (`0x20004510`, max 0x2c) and dispatched on CR to a 23-entry
name table (`who`, `now`, `pf`, `reset_bms`, `df`, `upgrade_ap`,
`upgrade_bl`, `into_bootloader`, `chg_cal_set/get`, `dsg_cal_set/get`,
`reset_esn`, `log_clear`, `ts0/1/2_set/get`, `ts_reset`, `fcc`, `soc`).
