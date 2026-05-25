#!/bin/bash
# fetch_sdk.sh — extract the TI SimpleLink SDK into vendor/ti-sdk-3.40/.
#
# Usage:
#   ./vendor/fetch_sdk.sh <path-to-sdk-installer>
#
# Supports:
#   .run  — Linux self-extracting installer (recommended)
#   .zip  — Windows/Mac zip archive
#
# Required: SimpleLink CC13x2/CC26x2 SDK 3.40.00.02
# Download: https://www.ti.com/tool/download/SIMPLELINK-CC13X2-26X2-SDK/3.40.00.02
# (free TI.com account required)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="$SCRIPT_DIR"
SDK_DIR="$VENDOR_DIR/ti-sdk-3.40"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path-to-sdk-installer>"
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

echo "=== Extracting TI SimpleLink SDK 3.40 ==="
echo "Source: $SDK_ARCHIVE"
echo "Target: $SDK_DIR"
echo ""

rm -rf "$SDK_DIR"
mkdir -p "$SDK_DIR"

case "$SDK_ARCHIVE" in
    *.run)
        echo "Detected .run installer."
        chmod +x "$SDK_ARCHIVE"
        TEMP_INSTALL="$VENDOR_DIR/.tmp_sdk_install"
        rm -rf "$TEMP_INSTALL"
        mkdir -p "$TEMP_INSTALL"

        "$SDK_ARCHIVE" --mode unattended --prefix "$TEMP_INSTALL" 2>&1 | tail -5

        # The installer creates a subdirectory with the SDK version name
        SDK_CONTENT=$(find "$TEMP_INSTALL" -maxdepth 2 -name "source" -type d | head -1)
        if [ -z "$SDK_CONTENT" ]; then
            echo "ERROR: SDK extraction failed — no 'source/' directory found."
            echo "Try the .zip download instead."
            rm -rf "$TEMP_INSTALL"
            exit 1
        fi
        SDK_ROOT="$(dirname "$SDK_CONTENT")"
        cp -r "$SDK_ROOT/source" "$SDK_DIR/"
        cp -r "$SDK_ROOT/kernel" "$SDK_DIR/"
        rm -rf "$TEMP_INSTALL"
        ;;

    *.zip)
        echo "Detected ZIP archive."
        unzip -o "$SDK_ARCHIVE" "source/**" "kernel/**" -d "$SDK_DIR" 2>&1 | tail -5
        ;;

    *.exe)
        echo "ERROR: Windows .exe installer not supported."
        echo "Download the .zip or .run variant instead."
        exit 1
        ;;

    *)
        echo "ERROR: Unknown archive format. Expected .run or .zip"
        exit 1
        ;;
esac

echo ""
echo "=== Verifying extraction ==="

ROM_JT="$SDK_DIR/source/ti/ble5stack/rom/rom_jt.h"
if [ -f "$ROM_JT" ]; then
    echo "  OK  rom_jt.h (ROM symbol table)"
else
    echo "  MISSING  rom_jt.h — SDK version may be wrong"
    echo "  Expected: $ROM_JT"
fi

if [ -d "$SDK_DIR/kernel/tirtos/packages" ]; then
    echo "  OK  TI-RTOS kernel headers"
else
    echo "  MISSING  kernel/ directory"
fi

echo ""
echo "=== Done ==="
echo "SDK extracted to: $SDK_DIR"
echo ""
echo "  make TI_SDK=$SDK_DIR          — build with SDK headers"
echo "  make                          — build with weak stubs (no SDK needed)"
echo "  make compare OEM_IMAGE=...     — byte-level diff against OEM binary"
