# Vendor SDK integration

bleware is built against the **TI SimpleLink CC13x2/CC26x2 SDK 3.40.00.02**.
The SDK is **not** distributed in this repository — you must obtain it from
TI and extract the needed files.

## Quick start

```bash
# 1. Download the SDK from TI.com (free account required):
#    https://www.ti.com/tool/download/SIMPLELINK-CC13X2-26X2-SDK/3.40.00.02
#    Choose the Linux .run installer or Windows .zip.

# 2. Extract the SDK into vendor/:
./vendor/fetch_sdk.sh ~/Downloads/simplelink_cc13x2_26x2_sdk_3_40_00_02.run

# 3. Build with SDK headers:
make TI_SDK=vendor/ti-sdk-3.40
```

Or set `TI_SDK` permanently:

```bash
echo 'export TI_SDK='"$(pwd)"'/vendor/ti-sdk-3.40' >> ~/.bashrc
make
```

## What the SDK provides

This project compiles only the **VanMoof-custom application code** (47 `.c`
files in `src/`). The TI SimpleLink SDK provides the **headers** that our
code compiles against:

| SDK path | Provided by |
|----------|-------------|
| `source/ti/ble5stack/rom/rom_jt.h` | ROM function address → name mapping |
| `source/ti/ble5stack/inc/` | BLE stack type definitions |
| `source/ti/ble5stack/icall/inc/` | ICall inter-task messaging API |
| `source/ti/devices/cc13x2_cc26x2/inc/` | Peripheral register addresses |
| `source/ti/devices/cc13x2_cc26x2/driverlib/` | DriverLib function prototypes |
| `source/ti/drivers/` | SPI, GPIO, PIN, Crypto driver headers |
| `kernel/tirtos/packages/` | TI-RTOS kernel type definitions |

The SDK also contains TI's source code (BLE host stack, TI-RTOS kernel,
drivers) but these are **not compiled** by this Makefile — they require
TI's XDCtools configuration system and Code Composer Studio project format.
All runtime functions called by our code reside in the CC2642R1F **mask ROM**
and are accessed through the ROM thunks in `src/hal_stubs.S`.

## Build modes

| Command | Description |
|---------|-------------|
| `make` | Build with weak stubs — compiles VanMoof code, all TI functions are no-ops |
| `make TI_SDK=vendor/ti-sdk-3.40` | Build with SDK headers — VanMoof code compiles against real TI types |
| `make compare OEM_IMAGE=path/to/oem.bin` | Byte-level diff against OEM firmware |

The weak-stub build produces a 1 KB binary (OAD header + VanMoof code only).
A functional 180 KB firmware image requires building the full TI BLE stack
in Code Composer Studio with the SimpleLink SDK 3.40 project. The OAD header
(144 bytes at flash 0x00-0x8F) is byte-equivalent to the OEM binary.

## SDK version pinning

bleware 1.4.01 was compiled against **SDK 3.40.00.02** (April 2020).
Later SDK versions (4.x, 5.x, 6.x) changed ROM jump table layouts and
driver APIs. The build will refuse to proceed if the wrong SDK is detected.

## Legal basis

Reverse engineering for interoperability is permitted under EU Software
Directive 2009/24/EC Art. 6 and US DMCA §1201(f). The ROM addresses,
function prototypes, and struct layouts in our source code are **facts
about the system** discovered through reverse engineering, not copyrighted
expression. No TI source code is distributed in this repository.
