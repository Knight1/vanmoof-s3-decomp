# bmsboot — memory map

Target: **STM32L072CZT6** (ARM Cortex-M0+, 192 KB flash, 20 KB SRAM, 6 KB EEPROM).
OEM image: `bmsboot_v007.bin` (20 480 B, exactly 20 KB).
SHA-256 `aa8d4ce13502dbe155483c5b9a498df018df3bfda4147226679dba359c939a6a`.
Banner: `"\nI am VanMoof BL V007 2022-11-04 09:32:30\r"`.

> All facts below are derived from the OEM binary's own vector table and reset
> disassembly. Ghidra image base is **`0x08000000`** (Ghidra addr == runtime
> addr). The loader has **no** VanMoof image header — the vector table is at the
> very base of flash, because the CPU resets straight into it. (The application
> banks *do* carry the 40-byte header.)

## Flash

| Region | Start | End | Size | Notes |
| --- | --- | --- | --- | --- |
| **bmsboot** | `0x08000000` | `0x08004FFF` | 20 KB | this image |
| **AP** (application) | `0x08005000` | `0x0801A7FF` | 86 KB | booted bank — `batteryware` runs here |
| **Shadow** (backup) | `0x0801A800` | `0x0802FFFF` | 86 KB | OTA staging / golden backup |

`SHADOW_BASE == AP_BASE + 0x15800`. `flash_copy_image()` mirrors **Shadow → AP**
by reading each destination page's source from `dst + 0x15800`.

Each app bank is prefixed by the 40-byte (`0x28`) VanMoof image header; the real
Cortex-M0+ vector table starts at `bank + 0x28` (SP at `+0x28`, reset at `+0x2C`).
`goto_application()` boots `AP_BASE + 0x28`.

`image_verify()` rejects any image whose `size` field is `>= 0x15801` (86 KB+1).
The serial-download protocol only accepts target addresses `> 0x08004FFF`
(i.e. `>= AP_BASE`).

## EEPROM (data EEPROM, byte-writable via the flash controller)

| Address | Size | Use |
| --- | --- | --- |
| `0x08080000` | 1 B | **boot flag** (`0x55` normal · `0xCC` recover · `0x33` ack · `0x5A` wipe) |
| `0x08080002` | 4 B | saved `RCC_CSR` reset-cause word (written each boot by `boot_hw_init`) |

## SRAM

| Symbol | Address | |
| --- | --- | --- |
| SRAM base | `0x20000000` | 20 KB |
| relocated vector table | `0x20000000` | 0xC0 bytes (copied by `boot_hw_init`, `VTOR` points here) |
| `.data` | `0x200000C0 .. 0x200005BC` | LMA `0x080049D8`, 0x4FC bytes |
| `.bss` | `0x200005BC .. 0x20001D6C` | |
| TX ring | `0x200008C0` | 4096 B |
| RX ring | `0x200018C0` | 1024 B |
| USART1 handle | `0x20001CC0` | HAL `UART_HandleTypeDef` |
| `_estack` (initial SP) | `0x20005000` | top of SRAM (vector slot 0) |

## Reset path (`Reset_Handler` @ `0x08001E60`)

A minimal stub — it does **not** configure the clock tree there (that happens in
`boot_hw_init()`, called from `main`). `.data`/`.bss` init is inline (no separate
`init_data_bss` routine):

```
Reset_Handler:   SP = 0x20005000
                 copy .data  (_sidata 0x080049D8 -> 0x200000C0..0x200005BC)
                 zero .bss   (0x200005BC..0x20001D6C)
                 bl  SystemInit            ; 0x08004860 — empty
                 bl  __libc_init_array     ; 0x0800486C -> _init 0x080048D8
                 bl  main                  ; 0x08000980 (never returns)
.Lhang:          b   .Lhang
```

## Vector table (`0x08000000`, 48 entries)

The OEM fills every device-IRQ slot with `Default_Handler` (`0x08001EB0`, a tight
`b .`) **except** IRQ19 and IRQ30, which are `0`, and IRQ27 (USART1), which is the
live comms ISR.

| Slot | Value | Handler |
| --- | --- | --- |
| 0 SP | `0x20005000` | top of SRAM |
| 1 Reset | `0x08001E60` | `Reset_Handler` |
| 2 NMI | `0x08001EB0` | `Default_Handler` |
| 3 HardFault | `0x08000C50` | `HardFault_Handler` → `failsafe` → `system_reset` |
| 11 SVCall | `0x08001EB0` | `Default_Handler` |
| 14 PendSV | `0x08001EB0` | `Default_Handler` |
| 15 SysTick | `0x0800169C` | `SysTick_Handler` (super-loop event pacer) |
| IRQ27 USART1 | `0x080017E8` | `USART1_IRQHandler` (comms RX/TX) |
| IRQ19, IRQ30 | `0x00000000` | unused |
| all other IRQs | `0x08001EB0` | `Default_Handler` |
