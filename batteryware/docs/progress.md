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
 | pending      |  0  |
 | in-progress  |  0 |
 | named        | 19 |
 | decomp-c     | 229 |
 | byte-eq      |  0 |
 | deferred     | 33 |

_Total functions: 281. 229 decomp-c, 19 named (declared + signature known, no C
body yet — dead-stripped state handlers and large unimplemented routines), 33
deferred (toolchain-provided runtime + intentionally-skipped thunks/veneers),
0 pending._
## Functions

| Addr | Size | Name | Status | Notes |
| --- | --- | --- | --- | --- |
| `0x08000130` | 266 | `__aeabi_uidivmod` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x0800023c` | 6 | `__aeabi_uidiv` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000244` | 2 | `trap_div0` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000248` | 56 | `__aeabi_ldiv0` | decomp-c | 64-bit div-by-zero handler (returns 0xFFFFFFFFFFFFFFFF) |
| `0x08000288` | 80 | `__aeabi_lmul` | named | ARM runtime 64-bit multiplication |
| `0x080002d8` | 406 | `__aeabi_ldivmod` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000470` | 22 | `__clz64` | deferred | Provided by `-lgcc`; OEM has its own copy at this address (byte-divergent).
| `0x08000488` | 42 | `__clzsi2` | named | ARM runtime count-leading-zeros (32-bit) |
| `0x08000658` | 392 | `main_clock_setup` | decomp-c | System clock tree initialisation |
| `0x0800080c` | 54 | `modem_init` | decomp-c | Modem peripheral initialisation |
| `0x08000850` | 34 | `dma_stop` | decomp-c | Stop all DMA transfers |
| `0x08000880` | 1614 | `cell_balance_update` | decomp-c | Cell balancing: voltage measurement and FET control |
| `0x08001060` | 72 | `state_handler_0b` | decomp-c | BMS state handler (0b) |
| `0x080010b4` | 272 | `state_timer_0b` | decomp-c | BMS state timer (0b) |
| `0x080011d8` | 72 | `state_handler_0c` | decomp-c | BMS state handler (0c) |
| `0x0800122c` | 396 | `state_timer_0c` | named | BMS state timer (0c) |
| `0x080013e0` | 72 | `state_handler_12` | decomp-c | BMS state handler (12) |
| `0x08001434` | 396 | `state_timer_12` | named | BMS state timer (12) |
| `0x080015e8` | 72 | `state_handler_13` | decomp-c | BMS state handler (13) |
| `0x0800163c` | 562 | `state_timer_13` | named | BMS state timer (13) |
| `0x08001898` | 72 | `state_handler_02` | decomp-c | BMS state handler (02) |
| `0x080018ec` | 46 | `capacity_decrement` | decomp-c | Decrement capacity counter by amount |
| `0x08001920` | 132 | `rsoc_lookup` | decomp-c | Lookup RSOC from state-of-charge table |
| `0x080019b8` | 358 | `state_timer_15` | named | BMS state timer (15) |
| `0x08001b44` | 90 | `state_handler_0d` | decomp-c | BMS state handler (0d) |
| `0x08001bb4` | 364 | `state_timer_0d` | decomp-c | BMS state timer (0d) |
| `0x08001d40` | 90 | `state_handler_0e` | decomp-c | BMS state handler (0e) |
| `0x08001db0` | 384 | `state_timer_0e` | decomp-c | BMS state timer (0e) |
| `0x08001f50` | 72 | `state_handler_14` | decomp-c | BMS state handler (14) |
| `0x08001fa4` | 378 | `state_timer_14` | decomp-c | BMS state timer (14) |
| `0x08002140` | 72 | `state_handler_15` | decomp-c | BMS state handler (15) |
| `0x08002194` | 2406 | `bms_state_machine` | decomp-c | Main BMS state machine — full pass (dual-pack MOSFET timing, OVP recovery, precharge SM) |
| `0x08002ba6` | 6 | `nop_2ba6` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08002bac` | 6 | `nop_2bac` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08002bc8` | 202 | `state_handler_03_init` | decomp-c | BMS state handler (03_init) |
| `0x08002cb8` | 142 | `discharge_mosfet_set` | decomp-c | Control discharge MOSFET (on/off) |
| `0x08002d50` | 106 | `charge_mosfet_set` | decomp-c | Control charge MOSFET (on/off) |
| `0x08002dc4` | 894 | `ymodem_receive` | decomp-c | YMODEM firmware receive protocol |
| `0x08003188` | 60 | `ymodem_send_byte` | decomp-c | YMODEM byte transmit |
| `0x080031d8` | 120 | `fg_watchdog_kick` | decomp-c | Fuel gauge watchdog reset |
| `0x0800325c` | 1314 | `fg_scan` | decomp-c | Fuel gauge register scan/poll loop |
| `0x080039c2` | 2860 | `fg_coulomb_update` | decomp-c | Fuel gauge coulomb counter update |
| `0x08004634` | 164 | `fg_read_loop` | decomp-c | Fuel gauge readout loop |
| `0x08004764` | 6 | `nop_4764` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800478c` | 296 | `smbus_read_nack` | decomp-c | SMBus read with NACK termination |
| `0x080048cc` | 312 | `smbus_read` | decomp-c | SMBus read transaction |
| `0x08004a18` | 724 | `smbus_write_reg` | decomp-c | SMBus register write transaction |
| `0x08004d04` | 864 | `bms_init` | decomp-c | BMS chip initialisation sequence |
| `0x080050ac` | 424 | `button_entry_check` | named | Check button for bootloader entry |
| `0x0800527c` | 86 | `led_flash` | decomp-c | Flash status LED |
| `0x080052d8` | 148 | `bms_configure` | decomp-c | Send configuration byte to BMS IC |
| `0x0800537c` | 10 | `nop_537c` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08005388` | 278 | `state_flags_handler_timer` | decomp-c | Timer-driven state flag handler |
| `0x080054cc` | 198 | `state_handler_17_19` | decomp-c | BMS state handler (17_19) |
| `0x080055a8` | 366 | `can_transmit` | named | CAN bus transmit |
| `0x08005738` | 72 | `state_handler_16` | decomp-c | BMS state handler (16) |
| `0x0800578c` | 26 | `nvic_system_reset` | decomp-c | NVIC system reset |
| `0x080057b0` | 666 | `main` | decomp-c | OEM `main` — early init, then **boot mode-report** (re-decomped 2026-05-29: latches power-on mode from ext-flash `0x08080001` → `0x20002C48`; 8-way "Power On" + 4-way "Power On Detect" UVP/OVP dispatch printing the `s_*_mode` strings via `uart_printf`; DP/VanMoof via GPIOB PB11), then state-machine super-loop. **Loop tail still suspect**: it gates on `0x20002C44` but the OEM gates on `s_bms_cfg` (`0x20002C00`) bits 0/1/2 and dispatches via a jump table at runtime `0x0801757C` — pending a separate pass. |
| `0x08005b34` | 818 | `bms_set_state` | decomp-c | Transition BMS state — full pass (two-stage switch, per-state counters, 0x38-B telemetry ring buffers) |
| `0x08006328` | 14 | `system_reset_simple` | named | system_reset wrapper |
| `0x08006336` | 10 | `system_reset` | decomp-c | System reset via NVIC |
| `0x08006340` | 142 | `flash_verify_header` | decomp-c | Verify flash image header CRC |
| `0x080063e0` | 788 | `state_timer_05` | decomp-c | Complex fault-dispatch engine with cell balancing + protection checks |
| `0x08006748` | 186 | `state_handler_01` | decomp-c | BMS state handler (01) |
| `0x08006810` | 292 | `state_timer_03` | decomp-c | Discharge monitoring state timer |
| `0x08006948` | 74 | `state_handler_07` | decomp-c | BMS state handler (07) |
| `0x0800699c` | 292 | `state_timer_06` | decomp-c | Discharge + alert monitoring |
| `0x08006ad4` | 74 | `state_handler_08` | decomp-c | BMS state handler (08) |
| `0x08006b28` | 292 | `state_timer_07` | decomp-c | Charge + alert monitoring |
| `0x08006c60` | 72 | `state_handler_0f` | decomp-c | BMS state handler (0f) |
| `0x08006cb4` | 292 | `state_timer_08` | decomp-c | Charge + alert (duplicate) |
| `0x08006dec` | 72 | `state_handler_10` | decomp-c | BMS state handler (10) |
| `0x08006e40` | 280 | `state_timer_09` | decomp-c | Discharge + charge monitoring |
| `0x08006f68` | 72 | `state_handler_11` | decomp-c | BMS state handler (11) |
| `0x08006fbc` | 288 | `state_timer_10` | decomp-c | DMA/USART init with threshold config |
| `0x080070f8` | 78 | `modem_reinit` | decomp-c | Modem reinitialisation |
| `0x08007158` | 20 | `system_reset_with_arg` | decomp-c | System reset with stored argument |
| `0x0800716c` | 10 | `nop_716c` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08007178` | 132 | `bootloader_entry` | decomp-c | Jump to bootloader |
| `0x0800721c` | 10 | `nop_721c` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x08007228` | 26 | `nvic_system_reset_dup` | named | NVIC system reset (duplicate) |
| `0x0800724c` | 36 | `uart_check_parity_error` | decomp-c | Check UART parity error flag |
| `0x08007278` | 40 | `uart_check_overrun_error` | decomp-c | Check UART overrun error flag |
| `0x080072a8` | 156 | `batteryware_main` | decomp-c | Main application entry (after init) |
| `0x08007368` | 1220 | `config_init` | decomp-c | BMS config struct init from EEPROM (0xB8 bytes) |
| `0x080078c8` | 924 | `bms_setup` | decomp-c | Full BMS init: clears SRAM, configures GPIO, loads EEPROM, calls FEDL5236 init, validates calibration, recalculates RSOC |
| `0x08007cf8` | 64 | `memcpy_byte` | decomp-c | Byte-by-byte memory copy |
| `0x08007d38` | 62 | `system_init` | decomp-c |  |
| `0x08007d78` | 430 | `gpio_init_buttons` | decomp-c | GPIO port A-D pin init + button/LED/charge MOSFET setup |
| `0x08007f50` | 100 | `modem_config` | decomp-c | Modem configuration |
| `0x08007fc4` | 80 | `uart_puthex_byte` | decomp-c | UART transmit byte as hex |
| `0x08008014` | 148 | `uart_puthex_16` | decomp-c | UART transmit 16-bit as hex |
| `0x080080a8` | 256 | `uart_puthex_32` | decomp-c | Print uint32 as 8 hex chars via uart_putchar |
| `0x080081a8` | 1872 | `uart_putdec_64` | decomp-c | Print uint64 as decimal via uart_putchar with division chain |
| `0x08008998` | 148 | `uart_printf` | decomp-c | Printf-like UART formatter (%x, %i, %d, %l, %s, %w) |
| `0x08008f28` | 68 | `nibble_to_hex` | decomp-c | Nibble to ASCII hex character |
| `0x08008f6c` | 56 | `hex_to_nibble` | decomp-c | ASCII hex character to nibble |
| `0x08008fa4` | 210 | `peripheral_init` | decomp-c | Peripheral clock and GPIO init |
| `0x08009084` | 64 | `dma_init` | decomp-c | DMA controller initialisation |
| `0x080090dc` | 110 | `flash_dma_start` | decomp-c | Start flash DMA operation |
| `0x0800915c` | 106 | `dma_compare` | decomp-c | DMA memory compare |
| `0x080091d4` | 214 | `flash_write_verify` | decomp-c | Flash write with verify |
| `0x080092b8` | 238 | `word_to_bytes` | decomp-c | Split word into byte array |
| `0x080093a6` | 108 | `memcmp_verify` | decomp-c | Memory compare with verification |
| `0x08009412` | 88 | `memcpy` | decomp-c | Memory copy (byte count) |
| `0x0800946c` | 92 | `delay_ms` | decomp-c | Millisecond delay loop |
| `0x080094d4` | 24 | `fault_led_trigger` | decomp-c | Trigger fault LED pattern |
| `0x080094ec` | 40 | `charge_mosfet_on` | decomp-c | Turn charge MOSFET on |
| `0x08009520` | 38 | `charge_mosfet_off` | decomp-c | Turn charge MOSFET off |
| `0x08009558` | 106 | `fg_uvp1_check` | decomp-c | Under-voltage protection level 1 check |
| `0x080095d4` | 106 | `fg_uvp2_check` | decomp-c | Under-voltage protection level 2 check |
| `0x08009650` | 106 | `fg_ovp1_check` | decomp-c | Over-voltage protection level 1 check |
| `0x080096cc` | 106 | `fg_ovp2_check` | decomp-c | Over-voltage protection level 2 check |
| `0x08009748` | 86 | `fg_threshold_check` | decomp-c | Fuel gauge voltage/current threshold check |
| `0x080097b0` | 76 | `fg_alert_monitor` | decomp-c | Fuel gauge alert register monitor |
| `0x0800980c` | 118 | `fg_discharge_oc_check` | decomp-c | Discharge over-current check |
| `0x0800989c` | 118 | `fg_charge_oc_check` | decomp-c | Charge over-current check |
| `0x0800992c` | 52 | `config_resend_all` | decomp-c | Resend all configuration registers |
| `0x0800997c` | 128 | `fg_charge_status` | decomp-c | Fuel gauge charging status check |
| `0x08009a10` | 46 | `fg_status_flag_get` | decomp-c | Read fuel gauge status flag |
| `0x08009a44` | 54 | `fg_status_flag2_get` | decomp-c | Read fuel gauge status flag 2 |
| `0x08009a80` | 22 | `fg_clear_status` | decomp-c | Clear fuel gauge status register |
| `0x08009aa0` | 26 | `nvic_system_reset_v3` | named | NVIC system reset (variant 3) |
| `0x08009ac4` | 820 | `command_parser` | decomp-c | KEY=VALUE command parser with 24-entry dispatch table |
| `0x0800a6aa` | 4 | `veneer_a6aa` | deferred | Thunk/veneer to another function |
| `0x0800a6ba` | 4 | `veneer_a6ba` | deferred | Thunk/veneer to another function |
| `0x0800a6be` | 4 | `veneer_a6be` | deferred | Thunk/veneer to another function |
| `0x0800a6e0` | 14 | `nop_a6e0` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800a70c` | 134 | `atoi_hex_offset1` | decomp-c | Hex string to int (offset=1) |
| `0x0800a794` | 374 | `state_timer_charge_a` | decomp-c | Charge monitoring with bootloader entry threshold |
| `0x0800a934` | 72 | `state_handler_09` | decomp-c | BMS state handler (09) |
| `0x0800a988` | 374 | `state_timer_charge_b` | decomp-c | Charge monitoring variant (duplicate) |
| `0x0800ab28` | 72 | `state_handler_0a` | decomp-c | BMS state handler (0a) |
| `0x0800ab7c` | 348 | `phase2_init` | decomp-c | GPIO check + flash/DMA page init for phase 2 boot |
| `0x0800ad00` | 88 | `uart_puts` | decomp-c | UART string transmit |
| `0x0800ad64` | 76 | `uart_putchar` | decomp-c | UART single character transmit |
| `0x0800adbc` | 272 | `uart_resp_handler` | decomp-c | UART response handler |
| `0x0800aee4` | 138 | `uart_tx_isr` | decomp-c | UART TX interrupt handler |
| `0x0800af80` | 32 | `uart_tx_flush` | decomp-c | Flush UART TX buffer |
| `0x0800afa4` | 526 | `uart_protocol_handler` | decomp-c | Modbus-like byte-at-a-time frame receiver |
| `0x0800b328` | 3690 | `HardFault_Handler` | named | Hard fault handler with register dump |
| `0x0800c24c` | 44 | `EXTI0_1_IRQHandler` | named | External interrupt 0/1 handler |
| `0x0800c278` | 2970 | `EXTI4_15_IRQHandler` | named | External interrupt 4-15 handler |
| `0x0800ce9e` | 1690 | `modem_command_handler` | decomp-c | Extended modem command handler + flash programming |
| `0x0800d75e` | 34 | `cmd_counter_inc` | decomp-c | Increment command counter |
| `0x0800d780` | 34 | `cmd_counter_inc_v2` | decomp-c | Increment command counter (v2) |
| `0x0800d7fc` | 34 | `cmd_counter_inc_v3` | decomp-c | Increment command counter (v3) |
| `0x0800d81e` | 40 | `cmd_write_and_inc` | decomp-c | Write command and increment counter |
| `0x0800d846` | 4 | `cmd_send_response_stub` | decomp-c | Empty cmd_send_response stub |
| `0x0800d84a` | 4 | `cmd_send_response_stub2` | decomp-c | Empty cmd_send_response stub (duplicate) |
| `0x0800d850` | 70 | `cmd_send_response` | decomp-c | Send command response |
| `0x0800d896` | 48 | `cmd_send_8byte` | decomp-c | Send 8-byte command response |
| `0x0800d8c6` | 16 | `protocol_reset` | decomp-c | Clears protocol state counter at 0x20002D8C |
| `0x0800d8f0` | 2152 | `flash_program_handler` | decomp-c | Multi-packet flash programming via DMA |
| `0x0800e1b4` | 8 | `thunk_e1b4` | deferred | Thunk to another function |
| `0x0800e1bc` | 8 | `thunk_e1bc` | deferred | Thunk to another function |
| `0x0800e1c4` | 4 | `thunk_e1c4` | deferred | Thunk to another function |
| `0x0800e1c8` | 2 | `nop_e1c8` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800e1ca` | 14 | `epilogue_e1ca` | decomp-c | Function epilogue / return stub |
| `0x0800e250` | 58 | `peripheral_reset` | decomp-c | Peripheral reset with timeout check |
| `0x0800e290` | 10 | `epilogue` | decomp-c | Empty return thunk |
| `0x0800e29c` | 92 | `flash_timeout_check` | decomp-c | Flash operation timeout check |
| `0x0800e304` | 14 | `tick_get` | decomp-c | Get tick counter value |
| `0x0800e318` | 14 | `tick_val_get` | decomp-c | Get tick value |
| `0x0800e32c` | 14 | `tick_ms_get` | decomp-c | Get millisecond tick |
| `0x0800e340` | 14 | `tick_timeout_get` | decomp-c | Get timeout tick value |
| `0x0800e354` | 718 | `flash_op_start` | decomp-c | Start flash operation with DMA |
| `0x0800e63c` | 286 | `usart1_dma_setup` | decomp-c | USART1 DMA configuration |
| `0x0800e774` | 16 | `nop_e774` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800e784` | 16 | `nop_e784` | decomp-c | Empty no-op stub (push/pop frame) |
| `0x0800e794` | 224 | `dma_deinit` | decomp-c | DMA deinitialisation |
| `0x0800e878` | 250 | `nvic_reconfigure` | decomp-c | Reconfigure NVIC interrupt priorities |
| `0x0800e984` | 188 | `flash_program_start` | decomp-c | Start flash program operation |
| `0x0800ea44` | 192 | `flash_erase_start` | decomp-c | Start flash erase operation |
| `0x0800eb04` | 140 | `flash_wait_ready` | decomp-c | Wait for flash ready |
| `0x0800eb90` | 54 | `delay_us` | decomp-c | Microsecond delay loop |
| `0x0800ebd0` | 48 | `nvic_enable_irq` | decomp-c | Enable NVIC interrupt |
| `0x0800ec04` | 62 | `nvic_enable_irq_dsb` | decomp-c | Enable NVIC interrupt with DSB |
| `0x0800ec48` | 212 | `interrupt_set_priority` | decomp-c | Set interrupt priority |
| `0x0800ed24` | 66 | `flash_page_erase` | decomp-c | Flash page erase with timeout |
| `0x0800ed6c` | 42 | `flash_opt_byte_op` | decomp-c | Flash option byte operation |
| `0x0800ed96` | 32 | `nvic_enable_irq_s` | decomp-c | Enable NVIC interrupt (short) |
| `0x0800edb6` | 32 | `nvic_enable_irq_s_dsb` | decomp-c | Enable NVIC interrupt (short) with DSB |
| `0x0800edd6` | 26 | `flash_erase_page_wrapper` | decomp-c | flash_page_erase boolean wrapper |
| `0x0800edf0` | 200 | `crc_init` | decomp-c | HAL_CRC_Init — re-id'd from `dma_channel_init`; 0x04C11DB7 = CRC-32 poly literal pool entry was misread as a CPAR address. Moved to crc.c |
| `0x0800eebc` | 16 | `crc_msp_init` | decomp-c | HAL_CRC_MspInit — weak empty stub. Re-id'd from `nop_eebc`; moved to crc.c |
| `0x0800eecc` | 142 | `dma_transfer` | decomp-c | DMA transfer setup |
| `0x0800ef5a` | 158 | `dma_transfer_irq` | decomp-c | DMA transfer with IRQ |
| `0x0800eff8` | 290 | `bytes_to_words` | decomp-c | Byte-stream to word-array packer (reverse of word_to_bytes) |
| `0x0800f11a` | 110 | `memcpy_halfword` | decomp-c | Halfword memory copy |
| `0x0800f188` | 218 | `crcex_polynomial_set` | decomp-c | HAL_CRCEx_Polynomial_Set — re-id'd from `dma_mem_config` (the "mask" was the polynomial, "size" was the POLYSIZE encoding). Moved to crc.c |
| `0x0800f264` | 110 | `flash_word_write` | decomp-c |  |
| `0x0800f2dc` | 148 | `flash_unlock_both` | decomp-c | Unlock flash (main + option) |
| `0x0800f384` | 36 | `flash_enable_prefetch` | decomp-c | Enable flash prefetch buffer |
| `0x0800f3ac` | 224 | `dma_wait_for_ready` | decomp-c | DMA status poll with timeout |
| `0x0800f490` | 302 | `dma_error_clear` | decomp-c | DMA error flag clear |
| `0x0800f5c8` | 188 | `dma_channel_reset_all` | named | Reset all DMA channels |
| `0x0800f694` | 78 | `flash_unlock` | decomp-c | Unlock flash controller |
| `0x0800f6f0` | 166 | `spi_register_write` | decomp-c | SPI register write |
| `0x0800f7a0` | 58 | `dma_channel_reset` | decomp-c | Reset a DMA channel |
| `0x0800f7e4` | 732 | `gpio_pin_config` | decomp-c | Multi-pin GPIO mode/type/speed config |
| `0x0800fae0` | 424 | `gpio_pin_reset` | decomp-c | Reset GPIO pin configuration |
| `0x0800fca4` | 58 | `gpio_bit_read` | decomp-c | Read GPIO pin state |
| `0x0800fcde` | 58 | `gpio_bit_write` | decomp-c | Write GPIO pin state |
| `0x0800fd18` | 136 | `dma_flash_start` | decomp-c | Start DMA flash transfer |
| `0x0800fdac` | 1864 | `rcc_osc_config` | decomp-c | HAL_RCC_OscConfig — 7-phase multi-oscillator bring-up (HSE/HSI/MSI/LSI/LSE/HSI48/PLL) with per-phase RDY polling. Re-id'd from `bus_fault_reset` stub / `peripheral_op` placeholder. Lives in rcc.c |
| `0x0801053e` | 8 | `dma_get_status` | decomp-c | Returns passed register value (empty placeholder) |
| `0x08010554` | 622 | `rcc_configure` | decomp-c | HAL_RCC_ClockConfig — FLASH latency staging (raise/lower with 5 s timeout), HPRE/PPRE1/PPRE2 prescaler programming, SYSCLK source switch with SWS ack poll, SystemCoreClock recalc + HAL_InitTick. Re-id'd from `usart_bus_config` stub. Lives in rcc.c |
| `0x080107e4` | 312 | `clock_prescaler_val` | decomp-c | Calculate clock prescaler value |
| `0x08010930` | 14 | `tick_counter_read` | decomp-c | Read tick counter |
| `0x08010944` | 34 | `fg_read_field_8` | decomp-c | Read fuel gauge field (8-bit) |
| `0x08010970` | 34 | `fg_read_field_11` | decomp-c | Read fuel gauge field (11-bit) |
| `0x0801099c` | 640 | `rcc_reconfigure` | decomp-c | `HAL_RCCEx_PeriphCLKConfig` — RTC clock-domain (APB1ENR/PWR/CSR) + CCIPR peripheral-clock-source selection. Verified byte-by-byte; fixed every RCC register offset (were all off by a stray `/4`) |
| `0x08010c48` | 310 | `dma_usart_init` | decomp-c | Initialise DMA for USART |
| `0x08010d84` | 82 | `modem_deinit` | decomp-c | Modem deinitialisation |
| `0x08010dd6` | 16 | `modem_isr_ack_dup` | decomp-c | Modem ISR acknowledge (duplicate) |
| `0x08010de6` | 16 | `modem_isr_ack` | decomp-c | Modem ISR acknowledge |
| `0x08010df8` | 364 | `smbus_transmit` | decomp-c | SMBus transmit transaction |
| `0x08010f78` | 16 | `modem_restart_thunk` | decomp-c | Modem restart thunk |
| `0x08010f88` | 24 | `bus_ready_check` | decomp-c | Check bus ready status |
| `0x08010fa0` | 120 | `dma_byte_handler` | decomp-c | DMA byte transfer handler |
| `0x0801101c` | 58 | `dma_byte_done` | decomp-c | DMA byte transfer done |
| `0x08011056` | 144 | `dma_byte_handler_v2` | decomp-c | DMA byte handler (v2) |
| `0x080110e8` | 116 | `dma_halfword_handler_v2` | decomp-c | DMA halfword handler (v2) |
| `0x08011160` | 46 | `flash_op_cleanup` | decomp-c | Clean up after flash operation |
| `0x0801118e` | 142 | `dma_halfword_handler` | decomp-c | DMA halfword handler |
| `0x0801121c` | 274 | `timeout_poll` | decomp-c | Poll with timeout |
| `0x08011338` | 132 | `dma_timeout_copy` | decomp-c | DMA copy with timeout |
| `0x080113c4` | 282 | `dma_transfer_done` | decomp-c | DMA transfer completion handler |
| `0x080114ec` | 164 | `flash_page_program` | decomp-c | Flash page program |
| `0x08011594` | 16 | `flash_program_init` | decomp-c | Initialise flash programming |
| `0x080115a4` | 1218 | `flash_prescaler_setup` | decomp-c | Flash timing prescaler setup |
| `0x08011b20` | 324 | `dma_channel_config` | decomp-c | Configure DMA channel registers |
| `0x08011c88` | 140 | `dma_completion_handler` | decomp-c | DMA completion interrupt handler |
| `0x08011d18` | 248 | `timeout_poll_v2` | decomp-c | Poll with timeout (v2) |
| `0x08011e14` | 10 | `SystemInit` | decomp-c | OEM SystemInit — empty -O0 frame; byte-faithful asm in `startup_stm32l072.S` (opcodes match OEM exactly) |
| `0x08011e20` | 72 | `__libc_init_array_lite` | decomp-c | Standard newlib `__libc_init_array` (empty pre-init, `_init`, empty init array); byte-faithful asm in `startup_stm32l072.S` (only `bl _init` offset differs pre-convergence) |
| `0x08011e68` | 18 | `memset_byte_copy` | decomp-c | Memset byte copy |
| `0x08011e7a` | 16 | `memset_byte_fill` | decomp-c | Memset byte fill |
| `0x08011e8c` | 12 | `_init` | decomp-c | crti/crtn-style `_init` (no C ctors); byte-faithful asm in `startup_stm32l072.S` (opcodes match OEM exactly) |
| `0x08011e98` | 12 | `veneer_11e98` | deferred | Thunk/veneer to another function |
| `0x08011ea8` | 10 | `veneer_11ea8` | deferred | Thunk/veneer to another function |
| `0x08011eb8` | 10 | `veneer_11eb8` | deferred | Thunk/veneer to another function |
| `0x08011ec8` | 10 | `veneer_11ec8` | deferred | Thunk/veneer to another function |
| `0x08011ed8` | 10 | `veneer_11ed8` | deferred | Thunk/veneer to another function |
| `0x08011ee8` | 10 | `veneer_11ee8` | deferred | Thunk/veneer to another function |
| `0x08011ef8` | 10 | `veneer_11ef8` | deferred | Thunk/veneer to another function |
| `0x08011f08` | 10 | `veneer_11f08` | deferred | Thunk/veneer to another function |
| `0x08011f18` | 10 | `veneer_11f18` | deferred | Thunk/veneer to another function |
| `0x08011f28` | 10 | `veneer_11f28` | deferred | Thunk/veneer to another function |
| `0x08011f38` | 10 | `veneer_11f38` | deferred | Thunk/veneer to another function |
| `0x08011f48` | 10 | `veneer_11f48` | deferred | Thunk/veneer to another function |
| `0x08011f58` | 10 | `veneer_11f58` | deferred | Thunk/veneer to another function |
| `0x08011f68` | 10 | `veneer_11f68` | deferred | Thunk/veneer to another function |
| `0x08011f88` | 10 | `veneer_11f88` | deferred | Thunk/veneer to another function |
| `0x080131f8` | 128 | `Reset_Handler` | named | System reset entry, calls system_init then main |
| `0x08013228` | 1496 | `coulomb_counter` | decomp-c | Signed coulomb integrator (charge/discharge split, RSOC % update tail). Size corrected 2026-05-28. |
| `0x0801324c` | 502 | `NMI_Handler` | named | Non-maskable interrupt handler (fault collection) |
| `0x08013800` | 90 | `rsoc_set` | decomp-c | Set relative state of charge |
| `0x08013860` | 64 | `calculate_rsoc` | decomp-c | RSOC percentage calculation helper |
| `0x080138ac` | 1196 | `cell_voltage_scan` | decomp-c | Two-pass scan: balance triggering + pair-fault flags + sum/avg/min/max + outlier patch + secondary/tertiary tables. Size corrected 2026-05-28. |
| `0x08013d88` | 198 | `fg_cell_balance` | decomp-c | Cell balancing control |
| `0x08014130` | 180 | `crc8_calc` | decomp-c | CRC-8 calculation |
| `0x080141e4` | 1834 | `crc8_for_smbus` | named | CRC-8 for SMBus packet |
| `0x080149b8` | 186 | `state_flags_handler` | decomp-c | Handle BMS state flags |
| `0x08014a90` | 88 | `shipping_mode_check` | decomp-c | Check shipping mode condition |
| `0x08014af8` | 246 | `timer_scheduler` | decomp-c | Periodic timer with 1000/500/100/10ms divisors |
| `0x08014ea4` | 150 | `crc16_calc` | decomp-c | CRC-16 calculation |
| `0x08014f40` | 128 | `modem_send_2bytes` | decomp-c | Modem 2-byte send |
| `0x08014fd0` | 168 | `temp_offset_send` | decomp-c | Send temperature offset |
| `0x0801507c` | 36 | `flash_unlock_opt` | decomp-c | Unlock flash option bytes |
| `0x080150ac` | 36 | `flash_lock_opt` | decomp-c | Lock flash option bytes |
| `0x0801518c` | 248 | `dma_irq_copy` | decomp-c | DMA IRQ copy routine |
| `0x08015294` | 160 | `atomic_copy_16words` | decomp-c | Atomic 16-word copy |
| `0x08015340` | 26 | `get_tick_ms` | decomp-c | Get current tick in milliseconds |
| `0x08015360` | 206 | `dma_wait_done` | decomp-c | Wait for DMA completion |
| `0x08015434` | 304 | `dma_error_clear_v2` | decomp-c | DMA error flag clear (duplicate of dma_error_clear) |
| `0x0801556c` | 10 | `veneer_1556c` | deferred | Thunk/veneer to another function |
| `0x0801557c` | 10 | `veneer_1557c` | deferred | Thunk/veneer to another function |
| `0x0801558c` | 10 | `veneer_1558c` | deferred | Thunk/veneer to another function |
| `0x0801559c` | 10 | `veneer_1559c` | deferred | Thunk/veneer to another function |
| `0x080155cc` | 10 | `veneer_155cc` | deferred | Thunk/veneer to another function |
| `0x080155ec` | 10 | `veneer_155ec` | deferred | Thunk/veneer to another function |
