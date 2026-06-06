# bmsboot — decomp progress

Target binary: `bmsboot_v007.bin` (20 480 B, ARM Cortex-M0+, STM32L072CZT6).
Loaded into Ghidra at **`0x08000000`**. The loader owns the first 20 KB of flash,
validates the application bank (AP `0x08005000`), recovers it from the Shadow bank
(`0x0801A800`) when needed, and jumps to it — or runs a serial-download server when
asked. Identity banner: `"\nI am VanMoof BL V007 2022-11-04 09:32:30\r"`.

See `docs/memory-map.md` for the flash/EEPROM/SRAM layout, `docs/hardware.md` for
the MCU/peripheral/GPIO map, `docs/protocol.md` for the image format + download
protocol, and `docs/changelog-v004-to-v007.md` for the differences from the older
V004 image.

## Decomp scope policy

**Decode only VanMoof-custom code.** The loader logic (boot decision, image
verify, flash copy, download server, ring-buffer comms, bring-up orchestration)
is translated to `src/`. The leaf drivers are recognised vendor code and left
`named` (renamed, body not translated) or `vendor-stock` (still `FUN_`):

- **STM32L0 HAL** — RCC / FLASH / GPIO / CRC / IWDG / UART (`hal_*`).
- **CMSIS / libgcc runtime** — `nvic_*`, `systick_set_reload`, `aeabi_*`, `clz*`.

> Decomp discipline: C is derived from **this** binary's own disassembly;
> `powerbankboot` (the STM32F091 sibling of this loader) is confirmation only for
> the shared "VanMoof BL" core.

## Summary

| Count | Status |
| --- | --- |
| ~38 | decomp-c (custom loader logic translated to `src/`) |
| ~29 | named (renamed; body vendor HAL/runtime — `hal_*`, `nvic_*`, `aeabi_*`, …) |
| 27 | vendor-stock (HAL / runtime internals — still `FUN_`) |
| 94 | total functions (`ghidra/exports/bmsboot_program.json`; 67 non-`FUN_` named) |

Build state: every `src/*.c` compiles clean under
`-Wall -Wextra -Wpedantic -Wshadow` (`make`). The image does **not** link yet —
the HAL/runtime leaves are `extern` and resolve once those libraries are vendored
in. The only unresolved symbols are `hal_*`, `crc32_accumulate`, `nvic_*`,
`systick_set_reload`, `g_hcrc` and `g_huart1`.

## Boot control-flow map

```
Reset_Handler (0x08001E60)                          src/startup_stm32l072.S
  └─ inline .data copy + .bss zero
  └─ SystemInit (0x08004860, empty) + __libc_init_array (0x0800486C)
  └─ main (0x08000980)                               src/main.c
       ├─ boot_hw_init (0x08000DBC)                   src/system.c
       │    ├─ mem_copy_bytes -> relocate vectors to 0x20000000, set VTOR
       │    ├─ hal_init (0x08001EB4)                  FLASH prefetch + 1 ms SysTick
       │    ├─ clock_periph_init (0x0800112C)         HSE/PLL + CRC unit
       │    ├─ comms_rx_state_init (0x080017C8)
       │    ├─ iwdg_hal_init (0x08001348)             HAL_IWDG_Init + first 0xAAAA
       │    ├─ hal_flash_unlock / hal_flash_unlock2
       │    ├─ download_pin_check (0x08000F14)         gpio_init; PA10 high -> download_uart_init
       │    └─ flash_program -> persist RCC_CSR to EEPROM 0x08080002
       ├─ led_init (0x08000E88) + uart_tx_string(banner) + uart_tx_flush
       ├─ read boot flag (EEPROM 0x08080000):
       │    0xCC -> if image_verify(Shadow)==OK: flash_copy_image (install + boot)
       │    0x33 -> rewrite 0x55 ;  0x5A -> rewrite 0x55 + erase AP & Shadow
       └─ super-loop (paced by SysTick g_boot_events):
            ├─ bit2: boot decision — image_verify(AP)==OK -> goto_application
            │         (0x0800163C); else recover from Shadow; else count down
            ├─ bit6: iwdg kick (IWDG_KR=0xAAAA) while idle
            ├─ uart_rx_drain (0x08001D38) -> ota_process_byte (0x0800049C)
            └─ uart_tx_pump  (0x08001DA0)

image_verify (0x08000C68)  magic+size+HW-CRC32 -> 0/1/2     src/image.c
flash_copy_image (0x08000D18)  Shadow->AP install, then boot src/image.c
flash_erase_page / flash_program / flash_program_verify / flash_read  src/flash.c
ota_process_byte / ota_send_response  "WHO?" download machine src/ota.c
USART1_IRQHandler (0x080017E8)  RX/TX rings                  src/uart.c
HardFault_Handler (0x08000C50) -> failsafe -> system_reset   src/handlers.c
SysTick_Handler (0x0800169C)  event-bit pacer                src/handlers.c
```

## Per-module decomp log

- **`startup_stm32l072.S`** — Cortex-M0+ vector table at flash base (48 slots; all
  device IRQs are `Default_Handler` except IRQ19/IRQ30 = 0 and IRQ27 = USART1),
  the inline-init `Reset_Handler` (`0x08001E60`), empty `SystemInit` (`0x08004860`),
  `__libc_init_array` (`0x0800486C`) + `_init` (`0x080048D8`), and `Default_Handler`
  (`0x08001EB0`). The live handlers (HardFault/SysTick/USART1) are weak-aliased and
  overridden by C. `decomp-c` (asm).

- **`main.c`** — `main` (`0x08000980`): bring-up, banner, the EEPROM-boot-flag
  decision, and the SysTick-driven resident super-loop (boot decision, watchdog
  kick, download service). Folds in what powerbankboot splits as `main` +
  `boot_main`. `decomp-c`.

- **`boot.c`** — `goto_application` (`0x0800163C`): tear down USART1, kick the IWDG,
  load the app SP/reset from `AP_BASE + 0x28`/`+0x2C`, set MSP, branch. `decomp-c`.

- **`image.c`** — `image_verify` (`0x08000C68`: magic `0xAA55AA55` + size + HW CRC-32
  over header(blanked crc/size)+body → 0/1/2) and `flash_copy_image` (`0x08000D18`:
  128-byte Shadow→AP mirror with erase/verify retry, full re-verify, then boot).
  `decomp-c`.

- **`flash.c`** — `flash_erase_page` (`0x080013D8`: HAL page erase, 50-retry,
  fail-safe), `flash_program` (`0x080015CE`: EEPROM byte program + read-back),
  `flash_program_verify` (`0x08001468`: 64-byte half-page program), `flash_read`
  (`0x080014E0`), `mem_copy` (`0x08000ED4`), `mem_copy_bytes` (`0x080048B4`) and
  `mem_set` (`0x080048C6`). `decomp-c`.

- **`ota.c`** — `ota_process_byte` (`0x0800049C`: the IDLE/ARG/DATA "WHO?" machine,
  incl. the deferred header-page commit), `ota_send_response` (`0x0800090C`),
  `ota_reset` and `ota_addr`. `decomp-c`.

- **`uart.c`** — `download_uart_init` (`0x08001A90`, USART1 @ 9600 8N1 PA9/PA10 AF4 +
  RXNE IRQ/NVIC), `comms_rx_state_init` (`0x080017C8`), `uart_tx_string`
  (`0x08001C7C`), `uart_tx_byte` (`0x08001CE0`), `uart_rx_drain` (`0x08001D38`),
  `uart_tx_pump` (`0x08001DA0`), `uart_tx_flush` (`0x08001E3C`), `comms_deinit`
  (`0x08001C1C`) and `USART1_IRQHandler` (`0x080017E8`). Interrupt-driven ring-buffer
  comms (TX 4096 B @ `0x200008C0`, RX 1024 B @ `0x200018C0`). `decomp-c`.

- **`system.c`** — `boot_hw_init` (`0x08000DBC`), `hal_init` (`0x08001EB4`),
  `systick_config` (`0x08001F00`), `clock_periph_init` (`0x0800112C`), `gpio_init`
  (`0x08000F58`), `led_init` (`0x08000E88`), `download_pin_check` (`0x08000F14`) and
  `iwdg_hal_init` (`0x08001348`). Vector relocation + VTOR, clock/CRC/IWDG bring-up,
  GPIO setup and the persisted reset-cause write. The clock/GPIO/CRC/IWDG leaves
  delegate to HAL externs. `decomp-c` (custom parts).

- **`handlers.c`** — `HardFault_Handler` (`0x08000C50`) → `failsafe` (`0x08000C5E`) →
  `system_reset` (`0x0800095C`, SCB AIRCR), and `SysTick_Handler` (`0x0800169C`, the
  super-loop event-bit pacer). NMI/SVCall/PendSV are `Default_Handler` in the OEM
  table. `decomp-c`.

- **`strings.c`** — the three banner strings, byte-for-byte from the OEM rodata
  block (`0x08004900`, `0x08004918`, `0x08004944`). `decomp-c` (data).

## Vendor-stock leaves (still `FUN_`)

27 functions remain `FUN_` in `ghidra/exports/bmsboot_program.json`: STM32L0 HAL
internals (RCC osc/clock sub-steps, GPIO/UART config helpers, flash status) and
libgcc runtime tails. They are recognised vendor code and deliberately left
untranslated per the decomp-scope policy. The five phantom "functions" Ghidra
auto-created inside the `.data` initializer image (`0x080049D8`..`0x08004ED4`) were
deleted.
