#!/bin/bash
# fetch_sdk.sh — extract needed TI SimpleLink SDK files from a user-provided
# SDK installer or zip. Run this once after downloading the SDK from TI.com.
#
# Usage:
#   ./vendor/fetch_sdk.sh <path-to-sdk-installer-or-zip>
#
# The SDK installer is a Linux .run file or a Windows .exe; the zip variant
# is simpler. Either way, this extracts only the files bleware actually
# needs, keeping the vendor footprint small.
#
# Required SDK version: SimpleLink CC13x2/CC26x2 SDK 3.40.00.02
# Download from: https://www.ti.com/tool/download/SIMPLELINK-CC13X2-26X2-SDK/3.40.00.02
# (requires TI.com account, free registration)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="$SCRIPT_DIR"
SDK_DIR="$VENDOR_DIR/ti-sdk-3.40"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path-to-sdk-installer-or-zip>"
    echo ""
    echo "Download the SDK from:"
    echo "  https://www.ti.com/tool/download/SIMPLELINK-CC13X2-26X2-SDK/3.40.00.02"
    exit 1
fi

SDK_ARCHIVE="$1"

if [ ! -f "$SDK_ARCHIVE" ]; then
    echo "ERROR: $SDK_ARCHIVE not found"
    exit 1
fi

echo "=== Extracting TI SimpleLink SDK 3.40 files ==="
echo "Source: $SDK_ARCHIVE"
echo "Target: $SDK_DIR"
echo ""

# Clean any previous extraction
rm -rf "$SDK_DIR"
mkdir -p "$SDK_DIR"

# Determine archive type and extract
case "$SDK_ARCHIVE" in
    *.zip)
        echo "Detected ZIP archive, extracting needed paths..."
        # List of paths bleware actually needs (minimal set)
        NEEDED_PATHS=(
            "source/ti/blestack/rom/r2/agama_r1/ble_rom_jt.h"
            "source/ti/blestack/rom/r2/agama_r1/ble_rom_jt.c"
            "source/ti/blestack/inc/*.h"
            "source/ti/blestack/icall/inc/*.h"
            "source/ti/blestack/icall/src/icall.c"
            "source/ti/blestack/icall/src/icall_lite.c"
            "source/ti/blestack/hal/src/common/hal_assert.c"
            "source/ti/blestack/npi/src/npi_tl_uart.c"
            "source/ti/drivers/rf/RF.c"
            "source/ti/drivers/pin/PINCC26XX.c"
            "source/ti/drivers/power/PowerCC26XX.c"
            "source/ti/drivers/spi/SPICC26XXDMA.c"
            "source/ti/drivers/crypto/CryptoCC26XX.c"
            "source/ti/drivers/*.h"
            "source/ti/drivers/pin/*.h"
            "source/ti/drivers/power/*.h"
            "source/ti/drivers/spi/*.h"
            "source/ti/drivers/crypto/*.h"
            "source/ti/devices/cc13x2_cc26x2/startup_files/startup_cc13x2_cc26x2_gcc.c"
            "source/ti/devices/cc13x2_cc26x2/inc/*.h"
            "source/ti/devices/cc13x2_cc26x2/driverlib/*.c"
            "source/ti/devices/cc13x2_cc26x2/driverlib/*.h"
            "kernel/tirtos/packages/ti/sysbios/rom/cortexm/cc13xx/package/CC13X2_CC26X2.h"
            "kernel/tirtos/packages/ti/sysbios/family/arm/m3/Hwi.c"
            "kernel/tirtos/packages/ti/sysbios/knl/Semaphore.c"
            "kernel/tirtos/packages/ti/sysbios/knl/Queue.c"
            "kernel/tirtos/packages/ti/sysbios/knl/Event.c"
            "kernel/tirtos/packages/ti/sysbios/knl/Task.c"
            "kernel/tirtos/packages/ti/sysbios/knl/Clock.c"
            "kernel/tirtos/packages/ti/sysbios/knl/Swi.c"
            "kernel/tirtos/packages/ti/sysbios/heaps/HeapMem.c"
            "kernel/tirtos/packages/ti/sysbios/hal/Hwi.c"
        )

        # Build the unzip include list
        INCLUDE_ARGS=()
        for path in "${NEEDED_PATHS[@]}"; do
            INCLUDE_ARGS+=("$path")
        done

        unzip -o "$SDK_ARCHIVE" "${INCLUDE_ARGS[@]}" -d "$SDK_DIR" 2>&1 | tail -5
        ;;

    *.run)
        echo "Detected .run installer. This requires executing the installer"
        echo "in a temporary directory. If you prefer not to run it, download"
        echo "the .zip variant instead from the same TI.com page."
        echo ""
        echo "Attempting extraction with --noexec --target..."
        chmod +x "$SDK_ARCHIVE"
        TEMP_INSTALL="$VENDOR_DIR/.tmp_sdk_install"
        rm -rf "$TEMP_INSTALL"
        mkdir -p "$TEMP_INSTALL"
        "$SDK_ARCHIVE" --noexec --target "$TEMP_INSTALL" 2>&1 || {
            echo "NOTE: .run extraction failed. Try the .zip download instead."
            rm -rf "$TEMP_INSTALL"
            exit 1
        }
        # The .run extracts into a subdirectory; move the source/ tree
        SDK_CONTENT=$(find "$TEMP_INSTALL" -name "source" -type d | head -1)
        if [ -d "$SDK_CONTENT" ]; then
            cp -r "$SDK_CONTENT/.."/* "$SDK_DIR/"
        fi
        rm -rf "$TEMP_INSTALL"
        ;;

    *.exe)
        echo "ERROR: Windows .exe installer not supported on this platform."
        echo "Download the .zip variant from the same TI.com page instead."
        exit 1
        ;;

    *)
        echo "ERROR: Unknown archive format. Expected .zip or .run"
        exit 1
        ;;
esac

echo ""
echo "=== Verifying extraction ==="

# Check for the critical ROM jump table header (the one file everything depends on)
ROM_JT="$SDK_DIR/source/ti/blestack/rom/r2/agama_r1/ble_rom_jt.h"
if [ -f "$ROM_JT" ]; then
    echo "  OK  ble_rom_jt.h (ROM symbol table)"
else
    echo "  MISSING  ble_rom_jt.h — SDK extraction may have failed"
    echo "  Expected at: $ROM_JT"
fi

DRIVERLIB=$(find "$SDK_DIR" -name "driverlib" -type d 2>/dev/null | head -1)
if [ -n "$DRIVERLIB" ]; then
    echo "  OK  driverlib/ (peripheral register defs)"
else
    echo "  MISSING  driverlib/ — SDK extraction may be incomplete"
fi

echo ""
echo "=== Done ==="
echo "SDK files extracted to: $SDK_DIR"
echo ""
echo "Now run 'make TI_SDK=$SDK_DIR' to build with the SDK."
echo "Or set TI_SDK in your environment: export TI_SDK=$SDK_DIR"
