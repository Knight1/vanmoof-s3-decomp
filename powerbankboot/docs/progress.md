# powerbankboot — decomp progress

Target binary: `powerbank_bootloader_1.00.bin` (32 768 B, ARM Cortex-M0,
STM32F091xC). Loaded into Ghidra at **`0x08000000`**. The loader owns the first
32 KB of flash, validates the two application banks (AP `0x08008000`, Shadow
`0x08024000`), installs the freshest valid one, and jumps to it — or runs a
serial-download server when neither bank is valid.

Identity: the banner is `"\nI am VM-BATT BL\r"` — this is the **VanMoof
battery-module bootloader** (shared with `bmsboot`; the PowerBank is a BMS
sibling). See `docs/hardware.md` for the binary identity and `docs/protocol.md`
for the image format + download protocol.

## Decomp scope policy

**Decode only VanMoof-custom code.** The bulk of the 32 KB image is recognised
vendor code, left `vendor-stock` (recognised, no C translation):

- **ST X-CUBE-STL** — IEC-60730 Class-B self-test library (CPU/RAM/Flash-CRC/
  clock tests, control-flow signature counters, `FailSafe`, reset-cause logging,
  clock cross-measurement). The OEM spliced the bootloader call into the STL
  `main()` right after log-init, so on a normal boot the loader runs first and
  the self-test body is dormant.
- **STM32F0 HAL** — RCC / FLASH / RTC / UART / GPIO / IWDG.
- **tinyprintf** formatter behind `dbg_printf`.

> Decomp discipline: C is derived from **this** binary's own disassembly;
> `bmsboot` is confirmation only for the shared loader core.

## Summary

| Count | Status |
| --- | --- |
| ~35 | decomp-c (custom loader logic translated to `src/`) |
| ~26 | named (renamed; body vendor HAL/STL — the X-CUBE-STL self-test suite `stl_*`, plus `hal_*`, `nvic_*`, `dbg_printf`, `main`) |
| 103 | vendor-stock (X-CUBE-STL / HAL / tinyprintf internals — still `FUN_`) |
| 164 | total functions (`ghidra/exports/powerbankboot_program.json`; 61 non-`FUN_` named) |

Build state: every `src/*.c` compiles clean under
`-Wall -Wextra -Wpedantic -Wshadow`. The image does **not** link yet — the HAL
and STL leaves are `extern` and resolve once those libraries are vendored in (the
only unresolved symbols are `hal_*`, `flash_hal_*`, `crc32_*`, `stl_*` and the
STL trace strings).

## Boot control-flow map

```
Reset_Handler (0x08002878)                          src/startup_stm32f091.S
  └─ init_data_bss (0x0800283C)
  └─ main (0x0800070C)                               src/main.c  [STL template]
       ├─ stl_log_init / stl_log_init2               vendor-stock
       └─ boot_main (0x080014A0)  ◀── spliced in     src/boot.c
            ├─ boot_hw_init (0x080018CC)              src/system.c
            │    ├─ hal_init (0x08002B88)            HAL_Init (prefetch+tick+msp)
            │    ├─ clock_periph_init (0x08001C08)    HSE/PLL, RTC(LSE), USART1
            │    ├─ comms_rx_state_init (0x08002214)
            │    ├─ iwdg_init (0x08001E4C)            IWDG + first 0xAAAA kick
            │    ├─ flash_lock (0x080033F4)           HAL_FLASH_Lock
            │    └─ gpio_init (0x08001A68)
            ├─ boot_read_persistent_flags (0x08001948)  RTC BKP0R/BKP1R
            │    └─ comms_uart_init (0x080024E4)      USART2 @115200, PA2/PA3, IRQ28
            ├─ image_verify (0x08001750)  ×N          src/image.c
            ├─ flash_copy_image (0x08001824)          AP↔Shadow, 2 KB pages
            │    ├─ flash_read / flash_erase_page / flash_program   src/flash.c
            │    └─ image_verify (re-check)
            ├─ goto_application (0x080019C0)  ── jump to AP+0x28
            └─ [no valid bank] download server loop:
                 ├─ uart_rx_drain (0x080026F0) → ota_process_byte (0x08000220)
                 │       └─ ota_send_response / flash_erase_page / flash_program
                 ├─ uart_tx_pump (0x08002768)
                 └─ events from SysTick_Handler (0x0800214C)

USART2_IRQHandler (0x08002248)  RX/TX rings           src/uart.c
NMI/HardFault/SVC/PendSV handlers                     src/handlers.c
TIM6_DAC_IRQHandler (0x08001158)  STL clock capture   src/handlers.c
```

## Per-module decomp log

- **`startup_stm32f091.S`** — Cortex-M0 vector table at flash base (48 slots),
  the minimal `Reset_Handler` (`0x08002878`: set SP → `init_data_bss` → `main`,
  no `SystemInit`), the idempotent `init_data_bss` (`0x0800283C`) and
  `Default_Handler` (`0x0800289C`). Seven live handlers are weak-aliased and
  overridden by C. `decomp-c` (asm).

- **`main.c`** — `main` (`0x0800070C`). The X-CUBE-STL Class-B `main` template
  with `boot_main()` spliced in after the trace console. `boot_main()` never
  returns on a normal boot, so the self-test body is dormant scaffold —
  reproduced with the real test sequence for documentation (CPU → IWDG →
  CRC-unit → flash-CRC → CP1 → RAM-march → clock → CP2, with the `cfc`
  control-flow signature inc/dec around each step). Every leaf is recognised
  X-CUBE-STL — see the suite table below. `named`.

- **`boot.c`** — `boot_main` (`0x080014A0`, the A/B orchestrator + download
  server loop) and `goto_application` (`0x080019C0`). The boot decision, the
  SysTick-driven server loop (keepalive, IWDG kick, finalise-mirror), and the
  app hand-off (load SP/PC from `AP+0x28`/`+0x2C`, set MSP, branch). `decomp-c`.

- **`image.c`** — `image_verify` (`0x08001750`: magic `0xAA55AA55` + size + HW
  CRC-32 over header(blanked crc/size) + body → 0/1/2) and `flash_copy_image`
  (`0x08001824`: 2 KB-page mirror with erase/program retry and full re-verify).
  `decomp-c`.

- **`flash.c`** — `flash_erase_page` (`0x08001E94`: HAL page erase, 50-retry,
  `FailSafe` on persistent failure → `"Write Flash NG"`), `flash_program`
  (`0x08001F1C`: word program + read-back verify), `flash_read` (`0x08002008`)
  and `mem_copy` (`0x08001A28`). `decomp-c`.

- **`ota.c`** — `ota_process_byte` (`0x08000220`: the 3-state "Who?" download
  machine), `ota_send_response` (`0x0800069C`: ACK/NAK + frame reset) and
  `ota_reset` (the state clear `boot_main` performs). `decomp-c`.

- **`uart.c`** — `comms_uart_init` (`0x080024E4`, USART2 @115200 8N1 PA2/PA3 AF1
  + RXNE IRQ/NVIC), `uart_tx_string` (`0x0800264C`), `uart_tx_byte` (`0x080026A4`),
  `uart_rx_drain` (`0x080026F0`), `uart_tx_pump` (`0x08002768`), `uart_tx_flush`
  (`0x08002810`) and `USART2_IRQHandler` (`0x08002248`). Interrupt-driven
  ring-buffer comms over USART2 (RX 1024 B @ `0x20000BC4`, TX 4096 B @
  `0x20000FCC`). `decomp-c`.

- **`system.c`** — `boot_hw_init` (`0x080018CC`), `clock_periph_init`
  (`0x08001C08`), `iwdg_init` (`0x08001E4C`) + `iwdg_refresh`, `gpio_init`
  (`0x08001A68`), `gpio_write_pins` (`0x08003978`, BSRR/BRR), `comms_rx_state_init`
  (`0x08002214`), `boot_read_persistent_flags` (`0x08001948`), `store_boot_flag`
  (`0x080020FC`), `rtc_bkp_read` (`0x080048AA`) and `rtc_bkp_write` (`0x0800487A`).
  Bring-up + persisted upgrade flag. The clock/RTC/UART/GPIO setup delegates to HAL
  externs (`hal_init` `0x08002B88`, `flash_lock` `0x080033F4`, …). `decomp-c`
  (custom parts).

- **`handlers.c`** — `NMI_Handler` (`0x080016AC`, clock-security), `HardFault_Handler`
  (`0x080016FC`), `SVC_Handler` (`0x08001718`), `PendSV_Handler` (`0x08001734`),
  `SysTick_Handler` (`0x0800214C`) and `TIM6_DAC_IRQHandler` (`0x08001158`, STL
  clock capture). The four fault traps log an "NG" line and `FailSafe`. `decomp-c`.

- **`strings.c`** — flash banner / trace strings, byte-for-byte from the OEM
  rodata block (`0x08006B8C..0x08006CC4`). `decomp-c` (data).

- **`state.c`** — shared SRAM globals (comms validity pair, OTA end address, STL
  clock-measurement state, HAL handle objects). Named, not yet byte-placed.

## X-CUBE-STL self-test suite (vendor-stock, named)

The IEC-60730 Class-B startup tests `main` runs (dormant on a normal boot, since
`boot_main` takes over first). Named from their disassembly + the trace strings
they emit; the bodies stay vendor-stock (they are stock X-CUBE-STL, not decoded).

| Addr | Name | Test | Trace |
| --- | --- | --- | --- |
| `0x08000A80` | `stl_clock_uart_startup` | SystemClock_Config + trace-UART bring-up | — |
| `0x080028B4` | `stl_cpu_test` | CPU core registers + PSP/MSP stack pointers | `CPU Test OK/NG` |
| `0x08000D10` | `stl_iwdg_test` | reset-cause check + IWDG watchdog test | `POR/SW/Pin/IWDG/LP reset` |
| `0x08000C98` | `stl_iwdg_config` | `HAL_IWDG_Init` for the watchdog test | — |
| `0x08000988` | `stl_crc_init` | `HAL_CRC_Init` (CRC unit for the flash test) | — |
| `0x08000E38` | `stl_flash_crc_test` | CRC the whole flash (`0x08000000`+N words) vs golden | `Flash OK/NG` |
| `0x08000940` | `stl_checkpoint_verify` | control-flow counter == `0x26`/`0x7C` (+ complement) | `CP 1/2 OK/NG` |
| `0x08002AB0` | `stl_ram_march_test` | RAM March C− over `0x20000000..0x20007FFF` (IRQs off) | `RAM OK/NG` |
| `0x08000BCC` | `stl_uart_handle_reset` | restore the trace-UART handle after the RAM test | — |
| `0x08000E84` | `stl_clock_test` | oscillator/PLL self-test (LSI/LSE/HSE/PLL/switch) | `Clock/LSI/LSE/HSE/PLL NG` |
| `0x080009D0` | `stl_clock_config` | RCC Osc+Clock reconfig (clock-test step) | `OscInit/ClkInit NG` |
| `0x080011F8` | `stl_clock_test_helper` | clock-test sub-step | — |
| `0x080006EC` | `stl_iwdg_test_helper` | IWDG-test sub-step | — |
| `0x0800112C` | `stl_failsafe` | IEC-60730 fail-safe (no return) | `--> FailSafePOR()` |
| `0x08005870` / `0x080059FC` | `stl_log_init` / `stl_log_init2` | trace/printf bring-up | — |
| `0x080058F8` | `stl_startup` | STL startup hook | — |

`stl_clock_meas_capture_irq` (`0x08001158`, the TIM6_DAC ISR) feeds
`stl_clock_test`'s independent cross-measurement (`Xmeas` / `LSE = %ld` /
`HSE = %ld`). The bodies of these `stl_*` leaves are recognised X-CUBE-STL and
deliberately left untranslated (decomp scope policy).