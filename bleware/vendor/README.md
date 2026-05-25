# Vendor SDK integration

bleware is built against the **TI SimpleLink CC13x2/CC26x2 SDK 3.40.00.02**.
The SDK is **not** distributed in this repository — you must obtain it from
TI and extract the needed files.

## Quick start

```bash
# 1. Download the SDK from TI.com (free account required):
#    https://www.ti.com/tool/download/SIMPLELINK-CC13X2-26X2-SDK/3.40.00.02
#    Choose the "Windows Zip" or "Linux Installer" variant.

# 2. Extract only the files bleware needs:
./vendor/fetch_sdk.sh ~/Downloads/simplelink_cc13x2_26x2_sdk_3_40_00_02.zip

# 3. Build:
make TI_SDK=vendor/ti-sdk-3.40
```

Or set `TI_SDK` permanently:

```bash
echo 'export TI_SDK='"$(pwd)"'/vendor/ti-sdk-3.40' >> ~/.bashrc
make
```

## What the SDK provides

| Component | Path in SDK | What bleware uses |
|-----------|-------------|-------------------|
| ROM symbol table | `source/ti/blestack/rom/r2/agama_r1/ble_rom_jt.h` | Maps ROM addresses → BLE stack function names |
| ICall | `source/ti/blestack/icall/` | Inter-task message passing (BLE stack ↔ app) |
| DriverLib | `source/ti/devices/cc13x2_cc26x2/driverlib/` | Peripheral register definitions (GPIO, SPI, UART, …) |
| TI-RTOS kernel | `kernel/tirtos/packages/ti/sysbios/` | Semaphore, Queue, Event, Task, Hwi, Clock, HeapMem |
| BLE host | `source/ti/blestack/` | GAP, GATT, L2CAP, HCI host stack |
| RF driver | `source/ti/drivers/rf/` | Radio configuration |
| PIN driver | `source/ti/drivers/pin/` | GPIO pin muxing |
| SPI driver | `source/ti/drivers/spi/` | SPI bus for external flash |
| Crypto driver | `source/ti/drivers/crypto/` | AES-128 ECB/CBC hardware accelerator |
| Power driver | `source/ti/drivers/power/` | Power/clock management |
| Startup | `source/ti/devices/cc13x2_cc26x2/startup_files/` | GCC startup (vector table, cinit) |

## What we DON'T ship (and why)

- **No TI `.c` or `.h` files** — these are TI's copyrighted code. You get them
  from TI directly.
- **No precompiled ROM symbols** — the ROM jump table addresses are factual
  (the ROM is fixed silicon, unchanging), but the *names* of those functions
  come from the SDK header. Our `hal_stubs.S` only has the addresses.
- **No linker command files (.cmd)** — we use our own `linker_cc2642r1.ld`.

## Legal basis

Reverse engineering for interoperability is permitted under:

- **EU Software Directive 2009/24/EC Art. 6** — "authorized to observe, study
  or test the functioning of the program in order to determine the ideas and
  principles which underlie any element of the program"
- **US DMCA §1201(f)** — reverse engineering for interoperability

The ROM addresses, function prototypes, and struct layouts in our source code
are **facts about the system** discovered through reverse engineering, not
copyrighted expression. The actual TI implementation code remains in the SDK
you download separately.

## Without the SDK

If you build without the SDK (`make` with no `TI_SDK` set), the build uses the
**weak stub fallbacks** in `hal_stubs.S` and `hal_stubs.c`. These are minimal
no-op implementations that let the VanMoof-custom code compile and link, but
the resulting binary will not be functional — TI-RTOS calls return safe defaults,
SPI/crypto operations are no-ops, and the BLE stack is absent.

This is useful for:
- Checking that VanMoof-custom code compiles without warnings
- Comparing decompiled functions against the OEM binary (the VanMoof parts
  should be byte-equivalent regardless of the SDK layer underneath)
- CI / static analysis

## SDK version pinning

bleware was compiled against **SDK 3.40.00.02** (shipped April 2020, matching
the bleware 1.4.01 build date). Later SDK versions (4.x, 5.x, 6.x) changed
ROM jump table layouts and driver APIs. If you use a different version, expect
linker errors.
