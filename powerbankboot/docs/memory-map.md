# powerbankboot — memory map

Target: **STM32F091xC** (ARM Cortex-M0, 256 KB flash, 32 KB SRAM).
OEM image: `powerbank_bootloader_1.00.bin` (32 768 B, exactly 32 KB).
SHA-256 `900f8149a015ef893de94dffd94108621eaa19408d120d65b8dc0dbe316d332e`.

> All facts below are derived from the OEM binary's own vector table and reset
> disassembly. Ghidra image base is set to **`0x08000000`** (Ghidra addr ==
> runtime addr). The loader has **no** VanMoof image header — the vector table
> is at the very base of flash, because the CPU resets straight into it.

## Flash

| Region | Start | End | Size | Notes |
| --- | --- | --- | --- | --- |
| **powerbankboot** | `0x08000000` | `0x08007FFF` | 32 KB | this image |
| **AP** (application) | `0x08008000` | `0x08023FFF` | 112 KB | booted bank — `powerbankware` runs here |
| **Shadow** (backup) | `0x08024000` | `0x0803FFFF` | 112 KB | OTA staging / golden backup |

Each app bank is prefixed by the 40-byte (`0x28`) VanMoof image header; the real
Cortex-M0 vector table starts at `bank + 0x28` (SP at `+0x28`, reset at `+0x2C`).
`goto_application()` boots `AP_BASE + 0x28`.

The serial-download protocol only accepts target addresses in
`0x08008000 .. 0x08023FFF` (the AP bank) — bounds `0x08007FFF < addr <= 0x08023FFF`.
`image_verify()` rejects any image whose `size` field is `>= 0x1C001` (112 KB+1).

## SRAM

| Symbol | Address | |
| --- | --- | --- |
| SRAM base | `0x20000000` | 32 KB |
| `_estack` (initial SP) | `0x20008000` | top of SRAM (vector slot 0) |

## Reset path (`Reset_Handler` @ `0x08002878`)

A minimal stub — it does **not** call `SystemInit`; the clock tree is brought up
later, inside `boot_hw_init()` (called from `boot_main`):

```
Reset_Handler:   SP = 0x20008000
                 bl  init_data_bss      ; 0x0800283C — copy .data, zero .bss
                 ldr r0, =main          ; 0x0800070D
                 bx  r0                 ; -> main (never returns)
```

`init_data_bss` (`0x0800283C`) is a standalone routine: the STL `main` re-invokes
it between self-test phases to refresh the RAM image, so it must be idempotent.

## Vector table (`0x08000000`, 48 entries)

| Slot | Value | Handler |
| --- | --- | --- |
| 0 SP | `0x20008000` | top of SRAM |
| 1 Reset | `0x08002878` | `Reset_Handler` |
| 2 NMI | `0x080016AC` | `nmi_css_handler` (clock-security) |
| 3 HardFault | `0x080016FC` | `HardFault_Handler` |
| 11 SVCall | `0x08001718` | `svc_trap_handler` |
| 14 PendSV | `0x08001734` | `PendSV_Handler` |
| 15 SysTick | `0x0800214C` | `SysTick_Handler` |
| IRQ17 TIM6_DAC | `0x08001158` | `stl_clock_meas_capture_irq` |
| IRQ28 USART2 | `0x08002248` | `USART2_IRQHandler` (comms) |
| all other IRQs | `0x0800289C` | `Default_Handler` (`b .`) |
