# MindMotion MM32F031 reference documentation

Official vendor documents for the MM32F031F6U6 (the MCU on the eShifter,
shifterboot). Stored locally so the
decomp doesn't depend on the upstream URLs staying live.

| File | Source | Size |
| --- | --- | --- |
| `DS_MM32F031xx_q_EN.pdf` | https://www.mindmotion.com.cn/download/products/DS_MM32F031xx_q_EN.pdf | 2.3 MB |
| `UM_MM32F031xx_q_EN.pdf` | https://www.mindmotion.com.cn/download/products/UM_MM32F031xx_q_EN.pdf | 14 MB |

Chinese-language equivalents (`*_SC.pdf`) also exist on
`mindmotion.com.cn/download/products/` and on
[SoCXin/MM32F031](https://github.com/SoCXin/MM32F031/tree/master/docs/Q)
if the EN versions ever go missing. The `q` variant matches the
`MM32F031xx_q` silicon stepping that the OEM ships; an older `n`
variant exists but isn't what's on the eShifter.

## Findings that resolved long-standing TBDs

### SYSCFG_CFGR1.MEM_MODE — what value `3` means

The OEM's `syscfg_set_mem_mode(3)` call in shifterware's `main` had been
flagged as "non-standard / vendor-specific" because `MEM_MODE = 3` is
listed as "reserved" in the STM32F0 reference. The MM32F031 UM
(§ "SYSCFG configuration register (SYSCFG_CFGR)") defines all four values
explicitly:

| `MEM_MODE[1:0]` | Effect |
| --- | --- |
| `x0` | Main flash memory mapped to `0x00000000` |
| `01` | System flash mapped to `0x00000000` |
| `11` | **Embedded RAM mapped to `0x00000000`** |

So `MEM_MODE = 3` activates the **SRAM-at-zero alias** — the Cortex-M0
vector-table relocation mechanism (CM0 has no VTOR register; remapping
SRAM to `0x00000000` makes the chip fetch vectors from SRAM `0x20000000`
instead of flash `0x08000000`).

Whether the OEM's shifterware actually relies on this for live IRQ
dispatch is another question — the 192-byte `memcpy(0x20000000,
0x08004828, 0xC0)` that precedes the `MEM_MODE = 3` write copies bytes
that don't decode as a valid vector table (slot 0 would be SP, but reads
as `0x2C014604` which is not a valid SRAM address). The most plausible
read is that the entire memcpy + `MEM_MODE=3` pair is vestigial vendor
startup-template code from the MindMotion BSP, with no live consumers
— consistent with our literal-pool scan finding zero references to
SRAM `0..0xBF` after the copy.

### RCC_CR bit 20 — the "48 vs 72 MHz" selector

UM § 5.3.1 ("Clock control register"):

> Bit 20  `HSI_72M_EN`  rw  Reset 0  Internal high-speed clock output selection
>   - 0: Internal high-speed clock output 48 MHz clock
>   - 1: Internal high-speed clock output 72 MHz clock

Shifterware's `set_sysclock_to_48m` clears this bit, locking the HSI to
48 MHz. shifterware-side `rcc_get_clocks_freq` already documented this
empirically; the UM confirms the bit name and direction.

### RCC_CR bit 2 — reserved

UM § 5.3.1:

> Bit 2  Reserved  Always read as 0.

The OEM's `set_sysclock_to_48m` clears this bit defensively but the
write has no effect.

### HSI clock tree

UM § 5.2 ("Clock control") block diagram:

- **HSI native frequency: 48 MHz** (or 72 MHz when `HSI_72M_EN` is set)
- **`/6` divider feeds the SW=HSI position** — so `SWS = 00` gives
  `HSI/6 = 8 MHz` (or `72/6 = 12 MHz` in 72 MHz mode), matching the
  shifterware-side `rcc_get_clocks_freq` table.
- **Direct HSI feeds the SW=PLL position** — so `SWS = 10` gives the
  full HSI output (48 MHz or 72 MHz). The "PLL" name is a misnomer:
  there is no actual multiplier on MM32F031's HSI path; the `SW=PLL`
  position is just HSI-without-the-/6-divider.

This is why shifterware's `set_sysclock_to_48m` works without ever
touching `PLLMUL` / `PLLSRC` / `PLLON` / `PLLRDY` — those bits exist
in the register layout but have no effect on the HSI-only path the
OEM uses. To get 48 MHz from HSI:

1. `RCC->CR.HSION = 1`, wait for `HSIRDY`
2. `RCC->CR.HSI_72M_EN = 0` (48 MHz mode)
3. `RCC->CFGR.SW = 10` (select the "PLL" path = direct HSI)
4. Wait for `RCC->CFGR.SWS == 10`

No external crystal required.

### Boot mode selection

UM § 1.5 ("Boot configuration"):

| `nBOOT1` (option byte) | `BOOT0` (pin) | Boot space mapped at `0x00000000` |
| --- | --- | --- |
| `x` | `0` | Main flash memory |
| `0` | `1` | System memory (vendor UART bootloader) |
| `1` | `1` | Embedded SRAM |

The BOOT pins are latched on the 4th rising SYSCLK edge after reset and
also re-sampled when waking from Standby. So the eShifter's PCB
hard-wires `BOOT0 = 0` (the default) and the chip executes from main
flash on power-on — confirming our model that the chip enters
shifterboot at `0x08000000` on cold reset.

### Memory map

UM § 1.4 ("Memory map") — the regions that matter for this project:

| Range | Size | Region |
| --- | --- | --- |
| `0x00000000..0x00007FFF` | 32 KB | Alias of one of {main flash, system memory, SRAM} per `MEM_MODE` / boot mode |
| `0x08000000..0x08007FFF` | 32 KB | Main flash (shifterboot at `0x08000000`, shifterware at `0x08003000`) |
| `0x1FFFF400..0x1FFFF7FF` | 1 KB | System memory (vendor UART bootloader — programmed by MindMotion at production) |
| `0x1FFFF800..0x1FFFF80F` | 16 B | Option bytes (`nBOOT1`, read/write protect, etc.) |
| `0x20000000..0x20000FFF` | 4 KB | SRAM |
| `0x40000000..0x40023FFF` | — | APB1 / APB2 / AHB peripherals (FLASH `0x40022000`, RCC `0x40021000`, etc.) |

## Open questions the UM does **not** resolve

- **How does the chip transition from shifterboot to shifterware?**
  The hardware-level boot path always runs shifterboot first (main
  flash, BOOT0=0). Nothing in shifterboot's main branches to
  shifterware's flash region. Possibilities — none confirmed:
  - A persistent flag in flash that shifterboot reads and uses to
    decide whether to set up an SRAM trampoline that then jumps to
    shifterware
  - `SYSCFG.MEM_MODE = 3` (set by shifterware itself, but that's after
    it has already started running)
  - shifterware is OTA-distributable but never actually executed on
    production hardware (only shifterboot runs)
- **Who writes `G_HCLK_HZ` at SRAM `0x20000148`?** A Ghidra xref scan
  found zero writes in either firmware. The OEM expects this value to
  be set before `boot_init_systick` runs. Mechanism unknown.

Both of these can only be resolved by hardware instrumentation
(JTAG, logic analyser, observed SRAM state at boot) rather than
binary archaeology.
