# Decompilation progress — batteryware

Per-function tracker. Source of truth for "what's left to do."

> Companion docs: **`memory-map.md`** for the MCU memory layout,
> header format, and vector table.

Populate the **Functions** table by:

1. In Ghidra, run *Script Manager → VanMoof → DumpBatterywareProgram*.
   This writes `ghidra/exports/batteryware_program.json`.
2. Append a row per function from the JSON, sorted by address.

Status legend:
- **pending** — not started
- **in-progress** — claimed by a contributor (note who in Notes)
- **named** — renamed in Ghidra, prototype set, but no C yet
- **decomp-c** — translated to C, builds, but bytes diverge from OEM
- **byte-eq** — translated to C and `make compare` reports zero diff
- **deferred** — intentionally skipped (e.g. unreachable, encrypted blob)

_Last refresh from `ghidra/exports/batteryware_program.json`: 2026-05-25
(image base `0x08000000`, **281 functions**, 59 strings)._

## Summary

| Status | Count |
| --- | --- |
| pending      | 131 |
| in-progress  |   0 |
| named        |  87 |
| decomp-c     |  63 |
| byte-eq      |   0 |
| deferred     |   0 |

_Total functions: 281. 150 decomp-c/named, 131 pending (`FUN_*`)._

## Functions

| Addr | Size | Name | Status | Notes |
| --- | --- | --- | --- | --- |
| `0x08000130` | — | `__aeabi_uidivmod` | decomp-c | ARM runtime unsigned division + modulo (binary long division) |
| `0x0800023c` | — | `FUN_0800023c` | pending | Division wrapper with zero-check (→ `__aeabi_uidiv`) |
| `0x08000244` | — | `FUN_08000244` | pending | Null subroutine (trap redirect) |
| `0x08000248` | — | `FUN_08000248` | pending | 64-bit div-by-zero handler (`__aeabi_ldiv0`) |
| `0x08000288` | — | `FUN_08000288` | pending | |
| `0x080002d8` | — | `FUN_080002d8` | pending | |
| `0x08000470` | — | `FUN_08000470` | pending | |
| `0x08000488` | 42 | `__clzsi2` | named | Compiler built-in count-leading-zeros |
| `0x08000658` | — | `FUN_08000658` | pending | |
| `0x0800080c` | 54 | `modem_init` | named | USART1 DMA/modem setup + GPIOA reset |
| `0x08000850` | 34 | `dma_stop` | named | Disable DMA + call stop |
| `0x08000880` | — | `FUN_08000880` | pending | |
| `0x08001060` | 72 | `state_handler_0b` | decomp-c | State machine handler → bms_set_state(0x0B) |
| `0x080010b4` | — | `FUN_080010b4` | pending | |
| `0x080011d8` | 72 | `state_handler_0c` | decomp-c | State machine handler → bms_set_state(0x0C) |
| `0x0800122c` | — | `FUN_0800122c` | pending | |
| `0x080013e0` | 72 | `state_handler_12` | decomp-c | State machine handler → bms_set_state(0x12) |
| `0x08001434` | — | `FUN_08001434` | pending | |
| `0x080015e8` | 72 | `state_handler_13` | decomp-c | State machine handler → bms_set_state(0x13) |
| `0x0800163c` | — | `FUN_0800163c` | pending | |
| `0x08001898` | 72 | `state_handler_02` | decomp-c | State machine handler (MOSFET on variant) → bms_set_state(0x02) |
| `0x080018ec` | 46 | `capacity_decrement` | named | Saturating counter decrement |
| `0x08001920` | 132 | `rsoc_lookup` | named | SoC lookup table (100-entry descending search) |
| `0x080019b8` | — | `FUN_080019b8` | pending | |
| `0x08001b44` | 90 | `state_handler_0d` | decomp-c | State handler → bms_set_state(0x0D) [conditional variant] |
| `0x08001bb4` | — | `FUN_08001bb4` | pending | |
| `0x08001d40` | 90 | `state_handler_0e` | decomp-c | State handler → bms_set_state(0x0E) [conditional variant] |
| `0x08001db0` | — | `FUN_08001db0` | pending | |
| `0x08001f50` | 72 | `state_handler_14` | decomp-c | State machine handler → bms_set_state(0x14) |
| `0x08001fa4` | — | `FUN_08001fa4` | pending | |
| `0x08002140` | 72 | `state_handler_15` | decomp-c | State machine handler → bms_set_state(0x15) |
| `0x08002194` | — | `FUN_08002194` | pending | |
| `0x08002ba6` | — | `FUN_08002ba6` | pending | |
| `0x08002bac` | — | `FUN_08002bac` | pending | |
| `0x08002bc8` | — | `FUN_08002bc8` | pending | |
| `0x08002cb8` | 142 | `discharge_mosfet_set` | named | Discharge MOSFET control with state tracking |
| `0x08002d50` | 106 | `charge_mosfet_set` | decomp-c | Charge MOSFET on/off via GPIOB pin 9 with idempotent state tracking |
| `0x08002dc4` | — | `FUN_08002dc4` | pending | |
| `0x08003188` | 60 | `ymodem_send_byte` | decomp-c | Send YMODEM response byte + reset protocol state |
| `0x080031d8` | — | `FUN_080031d8` | pending | |
| `0x0800325c` | — | `FUN_0800325c` | pending | |
| `0x080039c2` | — | `FUN_080039c2` | pending | |
| `0x08004634` | — | `FUN_08004634` | pending | |
| `0x08004764` | — | `FUN_08004764` | pending | |
| `0x0800478c` | — | `FUN_0800478c` | pending | |
| `0x080048cc` | — | `FUN_080048cc` | pending | |
| `0x08004a18` | — | `FUN_08004a18` | pending | |
| `0x08004d04` | — | `FUN_08004d04` | pending | |
| `0x080050ac` | — | `FUN_080050ac` | pending | |
| `0x0800527c` | — | `led_flash` | decomp-c | LED GPIO toggle with configurable fast/slow timing |
| `0x080052d8` | — | `FUN_080052d8` | pending | |
| `0x0800537c` | — | `FUN_0800537c` | pending | |
| `0x08005388` | — | `FUN_08005388` | pending | |
| `0x080054cc` | — | `FUN_080054cc` | pending | |
| `0x080055a8` | — | `FUN_080055a8` | pending | |
| `0x08005738` | 72 | `state_handler_16` | decomp-c | State machine handler → bms_set_state(0x16) |
| `0x0800578c` | 26 | `nvic_system_reset` | decomp-c | CMSIS __NVIC_SystemReset — SCB AIRCR system reset |
| `0x080057b0` | — | `FUN_080057b0` | pending | |
| `0x08005b34` | — | `FUN_08005b34` | pending | |
| `0x08006328` | — | `FUN_08006328` | pending | |
| `0x08006336` | 10 | `system_reset` | decomp-c | Wrapper around nvic_system_reset |
| `0x08006340` | — | `FUN_08006340` | pending | |
| `0x080063e0` | — | `FUN_080063e0` | pending | |
| `0x08006748` | — | `FUN_08006748` | pending | |
| `0x08006810` | — | `FUN_08006810` | pending | |
| `0x08006948` | 74 | `state_handler_07` | decomp-c | State handler → bms_set_state(0x07) [MOSFET on variant] |
| `0x0800699c` | — | `FUN_0800699c` | pending | |
| `0x08006ad4` | 74 | `state_handler_08` | decomp-c | State handler → bms_set_state(0x08) [MOSFET on variant] |
| `0x08006b28` | — | `FUN_08006b28` | pending | |
| `0x08006c60` | 72 | `state_handler_0f` | decomp-c | State machine handler → bms_set_state(0x0F) |
| `0x08006cb4` | — | `FUN_08006cb4` | pending | |
| `0x08006dec` | 72 | `state_handler_10` | decomp-c | State handler → bms_set_state(0x10) |
| `0x08006e40` | — | `FUN_08006e40` | pending | |
| `0x08006f68` | 72 | `state_handler_11` | decomp-c | State handler → bms_set_state(0x11) |
| `0x08006fbc` | — | `FUN_08006fbc` | pending | |
| `0x080070f8` | 78 | `modem_reinit` | decomp-c | USART/modem reinit + GPIO reset |
| `0x08007158` | 20 | `system_reset_with_arg` | decomp-c | Wrapper — takes arg, calls system_reset |
| `0x0800716c` | — | `FUN_0800716c` | pending | |
| `0x08007178` | — | `FUN_08007178` | pending | |
| `0x0800721c` | — | `FUN_0800721c` | pending | |
| `0x08007228` | 26 | `nvic_system_reset_dup` | decomp-c | Duplicate of nvic_system_reset from a different translation unit |
| `0x0800724c` | 36 | `uart_check_parity_error` | decomp-c | USART1 parity error detection + flag set |
| `0x08007278` | 40 | `uart_check_overrun_error` | decomp-c | USART1 overrun error detection |
| `0x080072a8` | — | `FUN_080072a8` | pending | |
| `0x08007368` | — | `FUN_08007368` | pending | |
| `0x080078c8` | — | `FUN_080078c8` | pending | |
| `0x08007cf8` | 64 | `memcpy_byte` | decomp-c | Byte-by-byte memcpy (ldrb/strb) |
| `0x08007d38` | 62 | `system_init` | named | System startup: GPIO + modem + flash + NVIC |
| `0x08007d78` | — | `FUN_08007d78` | pending | |
| `0x08007f50` | 100 | `modem_config` | named | USART2/modem struct init + peripheral enable |
| `0x08007fc4` | 80 | `uart_puthex_byte` | decomp-c | Print byte as 2 hex chars via uart_putchar |
| `0x08008014` | — | `FUN_08008014` | pending | |
| `0x080080a8` | — | `FUN_080080a8` | pending | |
| `0x080081a8` | — | `FUN_080081a8` | pending | |
| `0x08008998` | — | `FUN_08008998` | pending | |
| `0x08008f28` | — | `FUN_08008f28` | pending | |
| `0x08008f6c` | 56 | `hex_to_nibble` | decomp-c | ASCII hex char → nibble (reverse of nibble_to_hex) |
| `0x08008fa4` | — | `FUN_08008fa4` | pending | |
| `0x08009084` | 64 | `dma_init` | named | DMA channel configuration + reset on fail |
| `0x080090dc` | 110 | `flash_dma_start` | named | Flash DMA transfer start with retry + reset on fail |
| `0x0800915c` | 106 | `dma_compare` | named | DMA transfer compare (0x40-byte chunks) |
| `0x080091d4` | — | `FUN_080091d4` | pending | |
| `0x080092b8` | — | `FUN_080092b8` | pending | |
| `0x080093a6` | 108 | `memcmp_verify` | named | Byte-verify with per-byte spin-wait (I2C/config check) |
| `0x08009412` | — | `FUN_08009412` | pending | |
| `0x0800946c` | — | `delay_ms` | decomp-c | Busy-wait SysTick-polling millisecond delay |
| `0x080094d4` | — | `FUN_080094d4` | pending | |
| `0x080094ec` | — | `FUN_080094ec` | pending | |
| `0x08009520` | — | `FUN_08009520` | pending | |
| `0x08009558` | 106 | `fg_uvp1_check` | decomp-c | Under-voltage protection 1 monitor |
| `0x080095d4` | 106 | `fg_uvp2_check` | decomp-c | Under-voltage protection 2 monitor |
| `0x08009650` | 106 | `fg_ovp1_check` | decomp-c | Over-voltage protection 1 monitor |
| `0x080096cc` | 106 | `fg_ovp2_check` | decomp-c | Over-voltage protection 2 monitor |
| `0x08009748` | 86 | `fg_threshold_check` | decomp-c | Temperature sensor threshold monitor → FAULT_TS (0x10) |
| `0x080097b0` | 76 | `fg_alert_monitor` | named | Fuel gauge alert pin monitor |
| `0x0800980c` | 118 | `fg_discharge_oc_check` | decomp-c | Discharge over-current monitor |
| `0x0800989c` | 118 | `fg_charge_oc_check` | decomp-c | Charge over-current monitor |
| `0x0800992c` | 52 | `config_resend_all` | named | Re-send config data via memcpy-verify |
| `0x0800997c` | 128 | `fg_charge_status` | named | Charge/discharge status flags from fuel gauge |
| `0x08009a10` | 46 | `fg_status_flag_get` | decomp-c | Fuel gauge status flag read |
| `0x08009a44` | 54 | `fg_status_flag2_get` | decomp-c | Fuel gauge status flag 2 (bit 1) |
| `0x08009a80` | — | `FUN_08009a80` | pending | |
| `0x08009aa0` | — | `FUN_08009aa0` | pending | |
| `0x08009ac4` | — | `FUN_08009ac4` | pending | |
| `0x0800a6aa` | — | `thunk_FUN_0800a6e0` | pending | Thunk |
| `0x0800a6ba` | — | `thunk_FUN_0800a6e0` | pending | Thunk |
| `0x0800a6be` | — | `thunk_FUN_0800a6e0` | pending | Thunk |
| `0x0800a6e0` | — | `FUN_0800a6e0` | pending | |
| `0x0800a70c` | 134 | `atoi_hex_offset1` | decomp-c | Parse hex string (offset 1) as decimal int |
| `0x0800a794` | — | `FUN_0800a794` | pending | |
| `0x0800a934` | 72 | `state_handler_09` | decomp-c | State handler → bms_set_state(0x09) [dual-MOSFET variant] |
| `0x0800a988` | — | `FUN_0800a988` | pending | |
| `0x0800ab28` | 72 | `state_handler_0a` | decomp-c | State handler → bms_set_state(0x0A) [dual-MOSFET variant] |
| `0x0800ab7c` | — | `FUN_0800ab7c` | pending | |
| `0x0800ad00` | 88 | `uart_puts` | decomp-c | Write null-terminated string to TX ring buffer |
| `0x0800ad64` | 74 | `uart_putchar` | decomp-c | UART TX ring buffer write with 0x1400-byte circular buffer |
| `0x0800adbc` | — | `FUN_0800adbc` | pending | |
| `0x0800aee4` | — | `FUN_0800aee4` | pending | |
| `0x0800aee4` | 130 | `uart_tx_isr` | decomp-c | TXE interrupt — drains TX ring buffer to USART data register |
| `0x0800af80` | 28 | `uart_tx_flush` | decomp-c | Blocks until TX buffer fully drained |
| `0x0800afa4` | — | `FUN_0800afa4` | pending | |
| `0x0800b328` | 3690 | `HardFault_Handler` | named | Misidentified — actually modem response builder (indirect dispatch table) |
| `0x0800c24c` | 44 | `EXTI0_1_IRQHandler` | named | Vector IRQ handler — EXTI lines 0 and 1 |
| `0x0800c278` | 2970 | `EXTI4_15_IRQHandler` | named | Vector IRQ handler — EXTI lines 4-15 |
| `0x0800ce9e` | — | `FUN_0800ce9e` | pending | |
| `0x0800d75e` | 34 | `cmd_counter_inc` | named | Increment command counter |
| `0x0800d780` | 34 | `cmd_counter_inc_v2` | named | Counter increment variant 2 |
| `0x0800d7fc` | 34 | `cmd_counter_inc_v3` | named | Counter increment variant 3 |
| `0x0800d81e` | 40 | `cmd_write_and_inc` | named | Write to struct field + counter increment |
| `0x0800d846` | — | `FUN_0800d846` | pending | |
| `0x0800d84a` | — | `FUN_0800d84a` | pending | |
| `0x0800d850` | 70 | `cmd_send_response` | named | Send 8-byte Modbus response via uart_putchar |
| `0x0800d896` | 48 | `cmd_send_8byte` | named | Send 8-byte response via uart_putchar |
| `0x0800d8c6` | — | `FUN_0800d8c6` | pending | |
| `0x0800d8f0` | — | `FUN_0800d8f0` | pending | |
| `0x0800e1b4` | — | `FUN_0800e1b4` | pending | |
| `0x0800e1bc` | — | `FUN_0800e1bc` | pending | |
| `0x0800e1c4` | — | `FUN_0800e1c4` | pending | |
| `0x0800e1c8` | — | `FUN_0800e1c8` | pending | |
| `0x0800e1ca` | — | `FUN_0800e1ca` | pending | |
| `0x0800e250` | 58 | `peripheral_reset` | named | Set reset bit, poll for completion |
| `0x0800e290` | — | `FUN_0800e290` | pending | |
| `0x0800e29c` | 92 | `flash_timeout_check` | named | Flash operation timeout + opt byte fallback |
| `0x0800e304` | — | `FUN_0800e304` | pending | |
| `0x0800e318` | — | `FUN_0800e318` | pending | |
| `0x0800e32c` | — | `FUN_0800e32c` | pending | |
| `0x0800e340` | — | `FUN_0800e340` | pending | |
| `0x0800e354` | — | `FUN_0800e354` | pending | |
| `0x0800e63c` | — | `FUN_0800e63c` | pending | |
| `0x0800e774` | — | `FUN_0800e774` | pending | |
| `0x0800e784` | — | `FUN_0800e784` | pending | |
| `0x0800e794` | — | `FUN_0800e794` | pending | |
| `0x0800e878` | — | `FUN_0800e878` | pending | |
| `0x0800e984` | — | `FUN_0800e984` | pending | |
| `0x0800ea44` | — | `FUN_0800ea44` | pending | |
| `0x0800eb04` | 140 | `flash_wait_ready` | named | Flash operation wait-for-complete with timeout |
| `0x0800eb90` | 54 | `delay_us` | decomp-c | Calibrated busy-wait microsecond delay |
| `0x0800ebd0` | 48 | `nvic_enable_irq` | decomp-c | NVIC ISER bit set |
| `0x0800ec04` | 60 | `nvic_enable_irq_dsb` | decomp-c | NVIC ISER bit set + DSB/ISB |
| `0x0800ec48` | 60 | `interrupt_set_priority` | decomp-c | NVIC/SCB interrupt priority setter |
| `0x0800ed24` | 66 | `flash_page_erase` | decomp-c | Systick-based erase timeout (not actual flash erase) |
| `0x0800ed6c` | 42 | `flash_opt_byte_op` | decomp-c | Priority setter wrapper (signed-char conversion) |
| `0x0800ed96` | 32 | `nvic_enable_irq_s` | decomp-c | NVIC IRQ enable (signed char wrapper) |
| `0x0800edb6` | 32 | `nvic_enable_irq_s_dsb` | decomp-c | NVIC IRQ enable + DSB/ISB (signed char) |
| `0x0800edd6` | — | `FUN_0800edd6` | pending | |
| `0x0800edf0` | — | `FUN_0800edf0` | pending | |
| `0x0800eebc` | — | `FUN_0800eebc` | pending | |
| `0x0800eecc` | — | `FUN_0800eecc` | pending | |
| `0x0800ef5a` | — | `FUN_0800ef5a` | pending | |
| `0x0800eff8` | — | `FUN_0800eff8` | pending | |
| `0x0800f11a` | 110 | `memcpy_halfword` | named | Halfword-aligned memcpy with odd-byte tail |
| `0x0800f188` | — | `FUN_0800f188` | pending | |
| `0x0800f264` | 110 | `flash_word_write` | named | Single 32-bit word flash write |
| `0x0800f2dc` | — | `FUN_0800f2dc` | pending | |
| `0x0800f384` | 36 | `flash_enable_prefetch` | decomp-c | FLASH_ACR prefetch + latency enable |
| `0x0800f3ac` | — | `FUN_0800f3ac` | pending | |
| `0x0800f490` | — | `FUN_0800f490` | pending | |
| `0x0800f5c8` | — | `FUN_0800f5c8` | pending | |
| `0x0800f694` | 78 | `flash_unlock` | named | Flash unlock sequence with PRIMASK save/restore |
| `0x0800f6f0` | — | `FUN_0800f6f0` | pending | |
| `0x0800f7a0` | 58 | `dma_channel_reset` | named | DMA channel reset + register clear |
| `0x0800f7e4` | — | `FUN_0800f7e4` | pending | |
| `0x0800fae0` | — | `FUN_0800fae0` | pending | |
| `0x0800fca4` | — | `FUN_0800fca4` | pending | |
| `0x0800fcde` | 58 | `gpio_bit_write` | decomp-c | Atomic GPIO bit set/clear via BSRR/BRR |
| `0x0800fca4` | 36 | `gpio_bit_read` | decomp-c | GPIO input read via IDR register |
| `0x0800fd18` | 136 | `dma_flash_start` | named | DMA-based flash write with 0x2A timeout |
| `0x0800fdac` | — | `FUN_0800fdac` | pending | |
| `0x0801053e` | — | `FUN_0801053e` | pending | |
| `0x08010554` | — | `FUN_08010554` | pending | |
| `0x080107e4` | — | `FUN_080107e4` | pending | |
| `0x08010930` | — | `FUN_08010930` | pending | |
| `0x08010944` | 34 | `fg_read_field_8` | named | Fuel gauge register field read (shift 8) |
| `0x08010970` | 34 | `fg_read_field_11` | named | Fuel gauge register field read (shift 11) |
| `0x0801099c` | — | `FUN_0801099c` | pending | |
| `0x08010c48` | — | `FUN_08010c48` | pending | |
| `0x08010d84` | 82 | `modem_deinit` | named | USART/modem deinit + register cleanup |
| `0x08010dd6` | — | `FUN_08010dd6` | pending | |
| `0x08010de6` | — | `FUN_08010de6` | pending | |
| `0x08010df8` | — | `FUN_08010df8` | pending | |
| `0x08010f78` | — | `FUN_08010f78` | pending | |
| `0x08010f88` | — | `FUN_08010f88` | pending | |
| `0x08010fa0` | 120 | `dma_byte_handler` | named | DMA byte-by-byte transfer callback |
| `0x0801101c` | — | `FUN_0801101c` | pending | |
| `0x08011056` | — | `FUN_08011056` | pending | |
| `0x080110e8` | — | `FUN_080110e8` | pending | |
| `0x08011160` | 46 | `flash_op_cleanup` | named | Flash operation cleanup (clear CR bit) |
| `0x0801118e` | — | `FUN_0801118e` | pending | |
| `0x0801121c` | — | `FUN_0801121c` | pending | |
| `0x08011338` | — | `FUN_08011338` | pending | |
| `0x080113c4` | — | `FUN_080113c4` | pending | |
| `0x080114ec` | — | `FUN_080114ec` | pending | |
| `0x08011594` | — | `FUN_08011594` | pending | |
| `0x080115a4` | — | `FUN_080115a4` | pending | |
| `0x08011b20` | — | `FUN_08011b20` | pending | |
| `0x08011c88` | — | `FUN_08011c88` | pending | |
| `0x08011d18` | — | `FUN_08011d18` | pending | |
| `0x08011e14` | — | `FUN_08011e14` | pending | |
| `0x08011e68` | — | `FUN_08011e68` | pending | |
| `0x08011e7a` | — | `FUN_08011e7a` | pending | |
| `0x08011e98` | — | `FUN_08011e98` | pending | |
| `0x08011ea8` | — | `FUN_08011ea8` | pending | |
| `0x08011eb8` | — | `FUN_08011eb8` | pending | |
| `0x08011ec8` | — | `FUN_08011ec8` | pending | |
| `0x08011ed8` | — | `FUN_08011ed8` | pending | |
| `0x08011ee8` | — | `FUN_08011ee8` | pending | |
| `0x08011ef8` | — | `FUN_08011ef8` | pending | |
| `0x08011f08` | — | `FUN_08011f08` | pending | |
| `0x08011f18` | — | `FUN_08011f18` | pending | |
| `0x08011f28` | — | `FUN_08011f28` | pending | |
| `0x08011f38` | — | `FUN_08011f38` | pending | |
| `0x08011f48` | — | `FUN_08011f48` | pending | |
| `0x08011f58` | — | `FUN_08011f58` | pending | |
| `0x08011f68` | — | `FUN_08011f68` | pending | |
| `0x08011f88` | — | `FUN_08011f88` | pending | |
| `0x080131f8` | 128 | `Reset_Handler` | named | Vector reset entry — copies .data, zeroes .bss, calls main |
| `0x0801324c` | 502 | `NMI_Handler` | named | Shared default handler (also NMI, PendSV, SVC, and most IRQ traps) |
| `0x08013800` | 90 | `rsoc_set` | named | Relative SoC percentage setter |
| `0x08013860` | — | `FUN_08013860` | pending | |
| `0x080138ac` | — | `FUN_080138ac` | pending | |
| `0x08013d88` | — | `FUN_08013d88` | pending | |
| `0x08014130` | — | `FUN_08014130` | pending | |
| `0x080141e4` | — | `FUN_080141e4` | pending | |
| `0x080149b8` | — | `FUN_080149b8` | pending | |
| `0x08014a90` | 88 | `shipping_mode_check` | decomp-c | Shipping mode entry: state 0x0F/0x10/0x11 timeout check |
| `0x08014af8` | — | `FUN_08014af8` | pending | |
| `0x08014ea4` | — | `FUN_08014ea4` | pending | |
| `0x08014f40` | — | `FUN_08014f40` | pending | |
| `0x08014fd0` | — | `FUN_08014fd0` | pending | |
| `0x0801507c` | 36 | `flash_unlock_opt` | decomp-c | Flash option byte unlock (OPTKEYR write) |
| `0x080150ac` | 36 | `flash_lock_opt` | decomp-c | Flash option byte lock |
| `0x0801518c` | — | `FUN_0801518c` | pending | |
| `0x08015294` | — | `FUN_08015294` | pending | |
| `0x08015340` | 26 | `get_tick_ms` | decomp-c | Read system tick counter (ms since boot) from SRAM |
| `0x08015360` | — | `FUN_08015360` | pending | |
| `0x08015434` | — | `FUN_08015434` | pending | |
| `0x0801556c` | — | `FUN_0801556c` | pending | |
| `0x0801557c` | — | `FUN_0801557c` | pending | |
| `0x0801558c` | — | `FUN_0801558c` | pending | |
| `0x0801559c` | — | `FUN_0801559c` | pending | |
| `0x080155cc` | — | `FUN_080155cc` | pending | |
| `0x080155ec` | — | `FUN_080155ec` | pending | |
