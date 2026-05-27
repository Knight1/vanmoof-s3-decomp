# batteryware

Clean-room reconstruction of the **batteryware 1.17.1** firmware that runs
the VanMoof S3 battery management system (BMS) on an
**ST STM32L072CZT6** (Cortex-M0+).

| | |
| --- | --- |
| MCU | ST STM32L072CZT6 (Cortex-M0+, 32 MHz, ultra-low-power) |
| Image | `batteryware_1.17.1.bin` (87,568 B) |
| Flash | `0x08000000..0x0801560F` (self-contained, no separate bootloader) |
| Initial SP | `0x20005000` (top of 20 KB SRAM) |
| Status | **195 decomp-c / 30 deferred (veneers) / 6 named (libgcc + handler stubs)** — every application function is decoded |

## What it does

The batteryware is the Software behind the VanMoof S3's
battery pack. It communicates with a **FEDL5236** fuel-gauge IC over a
bit-banged GPIO SMBus/I²C link, measures individual cell voltages,
controls charge/discharge MOSFETs, and manages a 26-state safety
state machine.

On every reset:

1. `batteryware_main` initialises clocks, USART1, DMA, and flash.
2. `peripheral_init` runs a 3-phase bus-fault / USART / RCC setup.
3. `fg_scan` polls the FEDL5236 for cell voltages and coulomb counter
   readings, then dispatches into per-cell monitoring callbacks via
   a jump table at `0x08017470`.
4. `bms_set_state` transitions the BMS through one of 26 states
   (0x00–0x19), each with an associated state handler and a periodic
   timer callback (`state_timer_XX`).
5. The state timers continuously monitor:
   - **UVP1/UVP2** — under-voltage protection (per cell)
   - **OVP1/OVP2** — over-voltage protection (per cell)
   - **Discharge OC / Charge OC** — over-current protection
   - **TS** — temperature sensor threshold
   - **ALERT** — FEDL5236 fault alert pin
   - Cell voltage balancing (pairwise averaging within ±49 LSB)
6. When faults trigger, the state machine dispatches to protective
   states (turn off charge/discharge MOSFETs, signal to the main
   module over USART1).
7. Shipping mode is entered after a timeout with no USB/charge
   activity — the battery goes into deep sleep.

## Building

```bash
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi   # Debian/Ubuntu
brew install --cask gcc-arm-embedded                         # macOS

cd batteryware/
make            # build/batteryware.bin
make compare    # byte-diff vs batteryware_1.17.1.bin
make clean
make size       # flash / SRAM usage
make disasm     # interleaved C+asm listing → build/batteryware.lst
```

The build links against `-lgcc` for the ARM runtime helpers
(`__aeabi_uidivmod`, `__aeabi_lmul`, `__clzsi2`, etc.). No CMSIS or
vendor HAL — all MMIO is done through volatile pointers matching the
OEM's register access patterns.

`make compare` reports differing bytes. Zero-byte equivalence is not yet
achieved (startup code and vector table are not emitted by the build
system), but the application function count is complete.

## Project layout

```
batteryware/
├── Makefile                 ← arm-none-eabi-gcc build (Cortex-M0+)
├── linker_stm32l072.ld      ← 192 KB flash + 20 KB SRAM memory map
├── include/
│   └── batteryware.h        ← all function declarations (186 entries)
├── src/
│   ├── gpio.c               ← atomic GPIO bit write/read + multi-pin reset
│   ├── delay.c              ← delay_ms (SysTick poll) + delay_us (calibrated busy-wait)
│   ├── led.c                ← LED flash (PA4 toggle) + fault LED trigger
│   ├── reset.c              ← nvic_system_reset + system_init
│   ├── charge.c             ← charge/discharge MOSFET control (GPIOB pin 2/9)
│   ├── uart.c               ← TX ring buffer, TX ISR, hex printer, parity/overrun check
│   ├── ymodem.c             ← YMODEM send / receive state machine
│   ├── modem.c              ← USART1/USART2 init, deinit, reinit, SMBus transmit engine
│   ├── flash.c              ← 15 flash controller functions (unlock, erase, program, DMA-backed write)
│   ├── fuel_gauge.c         ← 26 fuel gauge functions (UVP/OVP/OC/TS/ALERT monitors, cell balance, SoC lookup)
│   ├── crc.c                ← CRC-16 Modbus + CRC-8 for flash verification
│   ├── hexconv.c            ← nibble_to_hex, hex_to_nibble, atoi_hex_offset1
│   ├── cmd.c                ← Modbus command response helpers
│   ├── spi.c                ← SPI register write + SMBus read/write primitives (FEDL5236 comms)
│   ├── dma.c                ← 17 DMA functions (transfer, byte/halfword handlers, IRQ copy, channel config)
│   ├── tick.c               ← 7 tick counter getters + clock_prescaler_val + rcc_reconfigure
│   ├── nvic.c               ← 4 NVIC enable wrappers + IRQ mask reconfigure
│   ├── main.c               ← batteryware_main + peripheral_init + main_loop
│   ├── state_handlers.c     ← 17 state machine transition handlers + 8 state timer handlers + bms_state_machine
│   └── nops.c               ← compiler-generated empty thunks / ROP trampolines
├── docs/
│   ├── progress.md          ← per-function decomp status (281 functions)
│   ├── memory-map.md        ← Flash/SRAM layout, vector table, VanMoof header format
│   └── hardware.md          ← GPIO pins, SRAM globals, FEDL5236, peripheral map
├── ghidra/
│   ├── exports/             ← batteryware_program.json (refreshed from DumpBatterywareProgram.java)
│   └── scripts/             ← (Dump script + vector table init live in ~/ghidra_scripts/)
└── build/                   ← build output (gitignored)
```

## Architecture

### BMS state machine (26 states)

The firmware manages a complex state machine across 26 BMS states
(`0x00`–`0x19`). Each state has two associated callbacks dispatched
from a jump table in `bms_set_state`:

| Function group | Pattern |
| --- | --- |
| `state_handler_XX` | One-shot transition: GPIO setup → `bms_configure(n)` → `bms_set_state(N)` |
| `state_timer_XX`   | Periodic ISR: `fg_scan()` → fault check → state dispatch → response chain |

The handlers follow 5 macro patterns (standard, MOSFET-on, dual-MOSFET,
inverted, conditional), all instantiated in `state_handlers.c` as C macros.
`bms_state_machine` is the master periodic entry point that dispatches to
the correct timer based on the current state register.

### FEDL5236 communication

The FEDL5236 is accessed via GPIO bit-banging on GPIOA pin 15 (chip select)
and a custom SMBus protocol:

| Primitive | Role |
| --- | --- |
| `smbus_write_reg(reg, val, mask)` | Write with CRC8 verification, up to 10 retries |
| `smbus_read(addr, count)` | Read with CRC8 + NACK retry |
| `smbus_read_nack(addr, val)` | Fire-and-forget write (no response check) |
| `smbus_transmit(ctx, tx, rx, len)` | DMA-backed SMBus frame transmission |

The bus state is tracked via a status byte at `ctx + 0x51`:
`0` = idle, `1` = ready, `2` = deiniting, `4`/`5` = active.

### Flash programming (YMODEM over USART)

Firmware updates arrive via YMODEM over USART1:

1. `ymodem_receive` handles the 3-state YMODEM receiver (header sync →
   read metadata → stream data blocks).
2. `flash_dma_start` configures a DMA channel for the flash page address.
3. `flash_page_program` calls `flash_prescaler_setup` (baud rate
   calibration based on RCC clock config), then delegates to
   `dma_completion_handler` for polling USART status with timeout.
4. Each 128-byte block is verified via `dma_compare` → retried on
   mismatch.

### Protection monitors

All protection checks follow an identical debounce pattern:

```c
if (sensor_value < threshold) {
    *counter = 0;                    // reset debounce
} else {
    uint16_t count = ++*counter;
    if (debounce / 100 <= count) {
        *g_fault_flags |= FAULT_BIT; // latch fault
    }
}
```

The central fault flags register `g_fault_flags` at `0x20002C44` uses
bit assignments:

| Bit | Flag |
| --- | --- |
| 0 | UVP1 (cell 1 under-voltage) |
| 1 | UVP2 (cell 2 under-voltage) |
| 2 | OVP1 (cell 1 over-voltage) |
| 3 | OVP2 (cell 2 over-voltage) |
| 4 | TS (temperature sensor) |
| 5 | ALERT (FEDL5236 alert pin) |
| 6 | DISCHARGE_OC |
| 7 | CHARGE_OC |

### Tick / timeout infrastructure

Three independent tick counters drive all timeouts:

| Function | Address | Use |
| --- | --- | --- |
| `tick_get` | `0x200047DC` | General-purpose ms tick (flash ops, DMA polls) |
| `tick_counter_read` | `0x200000C8` | Raw hardware tick (prescaler scaling) |
| `get_tick_ms` | `0x200047E0 + 0x14` | Millisecond counter (shipping mode timer) |

### DMA transfer engine

The DMA subsystem has 4 byte/halfword transfer handlers running in
pairs (periph→mem and mem→periph), completing via `dma_transfer_done`
and `dma_completion_handler`. Channel configuration is applied by
`dma_channel_config` walking a bitmask in `ctx[9]`. The `timeout_poll`
and `timeout_poll_v2` functions handle status-condition polling with
deadline-based timeouts.

## Decompilation status

**All 281 functions identified.** Full breakdown:

| Status | Count | Description |
| --- | --- | --- |
| `decomp-c` | 195 | Translated to C, builds with `-Wall -Wextra -Wpedantic` |
| `named` | 6 | libgcc runtime helpers (`__aeabi_lmul`, `__clzsi2`) + handler stubs (`Reset_Handler`, `NMI_Handler`, `HardFault_Handler`, `bms_set_state`) |
| `deferred` | 30 | Linker PLT stubs / long-call veneers — no C equivalent, linker-generated |
| `pending` | 0 | — |

See `docs/progress.md` for the per-function tracker with addresses,
sizes, and descriptions.

## Key findings

1. **FEDL5236 via GPIO SMBus — not hardware I²C.** The FEDL5236 fuel
   gauge chip is accessed through GPIO bit-banging on GPIOA pin 15
   (chip select), using `gpio_bit_write` for clock/data and
   `smbus_transmit` for DMA-backed frame transmission. The `I2C1`
   peripheral at `0x40005400` is not used for this link.

2. **Cell balancing algorithm.** `cell_balance_update` at `0x08000880`
   implements a multi-pass voltage averaging filter across 5 cells × 3
   measurements, with outlier rejection (±5 counts), arithmetic-mean
   smoothing, and `memcmp_verify` persistence to flash-backed SRAM.

3. **Shipping mode timer.** Idle timeout is tracked by `fg_scan` →
   `state_flags_handler_timer` → button entry check. After the
   timeout, the firmware writes a flash configuration block and enters
   an infinite loop (deepest sleep).

4. **Modem/USART2 for debug.** USART2 at `0x40004400` is used as a
   debug/modem port, not the main BMS link. The main communication
   with the cartridge MCU goes over USART1 at `0x40013800`.

5. **Runtime-configurable vectors.** SysTick (`0x2000199C`), IRQ 25
   (`0x20000E70`), and IRQ 27 (`0x20001AA0`) are patched in SRAM at
   runtime, characteristic of a firmware that accepts ISR callbacks
   across different execution phases (bootloader hand-off).

## Legal

Same as the parent repo: clean-room interoperability work under EU
Software Directive 2009/24/EC Art. 6 and DMCA §1201(f). The OEM image
is not redistributed; extract `batteryware_1.17.1.bin` from a `.pak`
file or JTAG dump of an STM32L072CZT6 you own. No warranty — flashing
a wrong image will brick the battery pack. Use a spare BMS PCB or
bench-top STM32L072 dev board for testing.
