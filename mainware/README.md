# mainware — VanMoof S3 main-controller application

Clean-room C reconstruction of `mainware 1.07.06` (218 784 B), the bike's
central-controller firmware. MCU **STM32F413VGT6** (Cortex-M4F), image base
`0x08020000` (512-B VanMoof envelope, then the vector table at `0x08020200`).
Build with `make`; subsystem status and the per-function log live in
[`docs/progress.md`](docs/progress.md), the peripheral/SRAM map in
[`docs/hardware.md`](docs/hardware.md).

## Serial links

The controller runs **eight** interrupt-driven serial links, each a register
block + a software TX/RX ring-buffer pair, pumped by a per-port `*_irq_handler`
(byte move + RM0430 `SR`-then-`DR` error-flag clear). The serial transport layer
is fully reconstructed.

| Port | Baud | Peer | Module |
| --- | --- | --- | --- |
| USART1 | 115200 | ES3 console (2nd port) | `console.c` |
| USART2 | 115200 | GSM / u-blox SARA modem | `modem.c` |
| USART3 | 9600 | eShifter (Modbus, slave 0x20) | `shifter.c` |
| UART5 | 115200 | BLE coprocessor (data link) | `uart.c` |
| UART4 | 9600 | BMS / battery (Modbus, slave 0xAA) | `bus.c` |
| USART6 | 38400 | inter-module SSPM bus | `ssp.c` |
| UART8 | 115200 | BLE coprocessor (debug link) | `uart.c` |
| **UART7** | **115200** | **ES3 debug console (primary port)** | **`console.c`** |

Each link's ISR is reached through a thin NVIC vector trampoline (`src/vectors.c`,
the external-IRQ slots of the table at `0x08020200`). Because the vector slot index
equals the IRQ number, the table is the authoritative map of which physical UART
serves which role — that is how the four `UART4/5/7/8` assignments above were pinned
(e.g. the console is on UART7 because its vector is IRQ82 and its port selector is `7`).

## The UART7 console port

UART7 (STM32F413 APB1, `0x40007800`) carries the **"ES3" debug console** — the
login-gated service shell whose 49-command dispatch table is documented in
[`docs/console.md`](docs/console.md). It is one of two physical ports for that
console (USART1 is the twin — see *Dual-port console* below); UART7 is the
default/primary. `uart7_init` brings the peripheral up and stores its register
block behind a pointer-to-pointer handle.

### Memory model

| Symbol | Address | Meaning |
| --- | --- | --- |
| `g_uart7_dev_pp` | `0x200097E4` | handle: `*g_uart7_dev_pp` → the UART7 register block |
| `g_console_uart_ctx` | `0x20003C34` | shared console context; UART7 ring handles live here |
| — TX ring | `g_console_uart_ctx + 0x164` | `ringbuf_t *` (1024-byte FIFO) |
| — RX ring | `g_console_uart_ctx + 0x168` | `ringbuf_t *` (1024-byte FIFO) |
| `g_console_state` | `0x20004D2C` | line-discipline block (escape counters + flags) |
| `g_console_log_echo` | `0x20000083` | when set, `console_printf` also mirrors lines to the SRAM log |
| `g_log_func` | `0x20009D98` | 5-slot active-console I/O table (see below) |

Register layout on the handle (`USART_TypeDef`): `SR +0x00`, `DR +0x04`,
`BRR +0x08`, `CR1 +0x0C`. `CR1` bits: `TXEIE 0x80`, `RXNEIE 0x20`. `SR` error
mask `0x0F` (PE/FE/NE/ORE).

### Functions (all in `console.c`)

| Function | OEM | Role |
| --- | --- | --- |
| `uart7_tx_byte` | `0x08036754` | mask TXEIE, push one byte to the TX ring, re-enable |
| `uart7_puts` | `0x0803678C` | transmit a NUL-terminated string |
| `uart7_write` | `0x0803679E` | transmit `len` raw bytes |
| `uart_rx_ringbuf_get_byte` | `0x080367B8` | mask RXNEIE, pop one byte from the RX ring, re-enable |
| `console_printf` | `0x080367F0` | the firmware-wide `printf` (see below) |
| `uart7_irq_handler` | `0x080368D4` | RX/TX byte pump + error clear + command-key handler |

The TX/RX primitives mask the relevant `CR1` interrupt-enable bit behind a
`DSB`/`ISB` pair while they touch the ring, then re-enable it; each returns the
ring-buffer status (`1` = byte moved, `0` = ring full/empty) left in `r0`.

### `console_printf` is `g_log_func[0]`

`console_io_table_install` (`0x080430D8`) binds the five UART7 functions into the
`g_log_func` dispatch table at `0x20009D98`:

```
g_log_func[0] = console_printf            ← the printf every module calls
g_log_func[1] = uart7_tx_byte
g_log_func[2] = uart7_puts
g_log_func[3] = uart7_write
g_log_func[4] = uart_rx_ringbuf_get_byte
```

So the ubiquitous `g_log_func("…")` calls throughout the firmware resolve to
`console_printf` whenever UART7 is the active console. `console_printf` formats
with `vsnprintf` into a 256-byte stack buffer (optionally mirroring an
`"<epoch> "`-prefixed copy into the SRAM log when `g_console_log_echo` is set),
then writes the message out **synchronously**: it queues bytes into the TX ring
with TXEIE masked, and whenever the ring fills it enables TXEIE and busy-waits
(kicking the watchdog) until the ISR has fully drained the FIFO before resuming;
TXEIE is left enabled on exit so the ISR sends the tail.

### Command keys (in `uart7_irq_handler`)

Each received byte is watched for two escape sequences:

* **10× ESC (`0x1B`)** — counter `g_console_state + 0x83`. On the tenth: write the
  bootloader hand-off magic `0x55AA5507` to SRAM `0x20000000`, log `"NVICReset"`,
  `systick_delay(10)`, then `system_reset()` (AIRCR `SYSRESETREQ`) — reboots into
  the loader for a firmware update.
* **10× TAB (`0x09`)** — counter `g_console_state + 0x84`. On the tenth: enter
  command mode (`console_io_table_install()`, log `"To Commandmode"`, set the
  command-mode flag `g_console_state + 0x82`).

Any other byte resets both counters.

### Dual-port console

UART7 and **USART1** are two physical ports for the *same* ES3 console. They share
the command-mode flag (`g_console_state + 0x82`) and the log-echo flag, but each
tracks its own ESC/TAB counters (UART7 at `+0x83`/`+0x84`, USART1 at
`+0x80`/`+0x81`). The active port is whichever one's I/O functions are currently
bound into `g_log_func`; a one-byte selector `g_console_port_sel` (`0x20000114`)
records it — `console_io_table_install` sets it to **7** (UART7),
`usart1_io_table_install` to **1** (USART1). The per-port reset magic differs so
the loader can tell which port asked: UART7 = `0x55AA5507`, USART1 = `0x55AA5501`.
