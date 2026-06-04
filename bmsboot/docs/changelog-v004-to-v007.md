# bmsboot — changes from V004 to V007

Diff between the two OEM bootloader images shipped for the battery module:

| | V004 | V007 |
| --- | --- | --- |
| File | `bmsboot_v004.bin` | `bmsboot_v007.bin` |
| Size | 20 480 B | 20 480 B |
| SHA-256 | `51048dd1c5c938cbd5dd6c470c67fc7515dcc9b4be23539181446f9c46d084d4` | `aa8d4ce13502dbe155483c5b9a498df018df3bfda4147226679dba359c939a6a` |
| Banner | `"I am G5 VanMoof BL V004 2019-11-19"` | `"I am VanMoof BL V007 2022-11-04 09:32:30"` |
| Build | 2019-11-19 | 2022-11-04 09:32:30 |

> Scope: V007 is the fully decompiled target (`../docs`, `../src`). V004 was
> **not** separately decompiled — this diff is derived from the two binaries'
> own bytes (objdump/raw reads) plus the V007 function map. The image base for
> both is `0x08000000` (loader at flash base, no VanMoof header).

## Headline

V004 → V007 is a **full recompile with a completely different function layout**,
not an incremental patch: **18 983 / 20 480 bytes differ (92.7 %)**. Almost every
function moved to a new address (see the vector table below). The *design* is
unchanged — same headerless A/B-bank loader, same inline-init reset, same "WHO?"
serial download — but two functional changes stand out:

1. **V004 wires RTC + EXTI interrupts that V007 dropped** (the most meaningful
   behavioural change).
2. The **identity banner** was reworded and the **`.data` image grew** from 4 B
   to 1 276 B.

## 1. Interrupt vector table

Only the *live* entries are shown; every other IRQ slot is `Default_Handler` in
both images (the V004 default is `0x080048CC`, the V007 default `0x08001EB0`).

| Slot | Vector | V004 | V007 | Change |
| --- | --- | --- | --- | --- |
| 1 | Reset | `0x0800487C` | `0x08001E60` | relocated (same logic) |
| 3 | HardFault | `0x080034FC` | `0x08000C50` | relocated |
| 15 | SysTick | `0x08004144` | `0x0800169C` | relocated |
| 18 | **RTC** (IRQ2) | `0x08003690` | `Default` | **removed** |
| 21 | **EXTI0_1** (IRQ5) | `0x080036B4` | `Default` | **removed** |
| 22 | **EXTI2_3** (IRQ6) | `0x080036BE` | `Default` | **removed** |
| 23 | **EXTI4_15** (IRQ7) | `0x080036C8` | `Default` | **removed** |
| 43 | USART1 (IRQ27) | `0x08004290` | `0x080017E8` | relocated |

NMI / SVCall / PendSV point at the default handler in both. IRQ19 and IRQ30 are
`0` in both.

### Example — V004 RTC wakeup ISR (gone in V007)

V004 services the **RTC wake-up** interrupt: it sets a RAM event flag and clears
the EXTI line-20 (RTC wake-up) pending bit. V007 has no RTC handler at all.

```asm
; V004 RTC_IRQHandler @ 0x08003690
push  {r7, lr}
ldr   r2, =0x20000350      ; RAM event flag
ldrb  r3, [r2]
orrs  r3, #4               ; *0x20000350 |= 0x04   (post "RTC woke us" event)
strb  r3, [r2]
ldr   r3, =0x40010400      ; EXTI base
movs  r2, #0x80
lsls  r2, r2, #13          ; r2 = 0x00100000  (bit 20 = RTC wakeup line)
str   r2, [r3, #0x14]      ; EXTI->PR = 0x100000  (clear the pending line)
pop   {r7, pc}
```

### Example — V004 EXTI ISR stubs (gone in V007)

The three EXTI handlers in V004 are present but **empty** (they push/return
without touching anything) — placeholders that V007 simply removed:

```asm
; V004 EXTI0_1_IRQHandler @ 0x080036B4 (EXTI2_3 @ 0x080036BE and
;       EXTI4_15 @ 0x080036C8 are byte-identical stubs)
push  {r7, lr}
nop
pop   {r7, pc}
```

**Takeaway:** V004 ran a periodic RTC wake-up (and reserved the EXTI lines);
V007 removed the RTC wake-up path and the EXTI vectors. The V007 loader is purely
SysTick-paced (`SysTick_Handler` event bits) with USART1 the only device IRQ.

## 2. Identity banner

```
V004:  "I am G5 VanMoof BL V004 2019-11-19"        (@ 0x080049C1)
V007:  "I am VanMoof BL V007 2022-11-04 09:32:30"  (@ 0x08004919)
```

- V004 carries a **`G5 `** hardware-generation tag that V007 drops.
- V004's date is day-only; V007 adds a build **time**.
- V004 has a **single** banner string (V004). V007 has **three**: the V007
  startup banner plus two `"I am VanMoof BL V006 "` copies (the WHO?-handshake
  reply and the super-loop re-announce — see `protocol.md`). So the
  download-protocol reply version (`V006`) is decoupled from the build tag
  (`V007`).

## 3. Memory layout (from the reset literal pools)

Both reset handlers are byte-for-byte the same *shape* (set SP → inline `.data`
copy → inline `.bss` zero → `SystemInit` → `__libc_init_array` → `main` → hang),
only relocated. The linker symbols differ:

| Symbol | V004 | V007 |
| --- | --- | --- |
| `_estack` | `0x20005000` | `0x20005000` |
| `.data` (`_sdata`..`_edata`) | `0x200000C0`..`0x200000C4` (**4 B**) | `0x200000C0`..`0x200005BC` (**0x4FC = 1276 B**) |
| `.data` LMA (`_sidata`) | `0x08004A14` | `0x080049D8` |
| `.bss` end (`_ebss`) | `0x20001850` | `0x20001D6C` |

V007 ships far more **initialised data** (1 276 B vs 4 B) and uses more SRAM
(`.bss` ends ~0x51C higher). The 4 KB TX / 1 KB RX comms rings in V007 live at
`0x200008C0` / `0x200018C0`; V004's RAM footprint ends much lower (`0x20001850`),
so V004 either used smaller buffers or fewer resident structures.

## 4. Code layout

The bodies were re-laid-out wholesale by the (3-years-newer) toolchain/build:
the single differing region `0x08000252..0x08004ED3` (19 586 B) covers essentially
all code + rodata + the `.data` initialiser. Because every function moved, a
function-by-function name diff isn't meaningful here — the *structural* changes
above (dropped RTC/EXTI ISRs, banner, `.data` growth) are the substantive ones.
Spot-checks confirm the core routines are the same loader (inline-init reset,
HardFault→reset, SysTick pacer, USART1 "WHO?" download), just relocated.
