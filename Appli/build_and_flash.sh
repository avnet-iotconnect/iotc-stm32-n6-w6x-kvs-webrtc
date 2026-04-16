#!/bin/bash
# build_and_flash.sh — Build, sign, and flash the STM32N6570-DK firmware
#
# Usage:
#   ./build_and_flash.sh [build|sign|flash|all]
#
# Requires:
#   - STM32CubeIDE GCC 14.3 (for -fcyclomatic-complexity)
#   - STM32CubeProgrammer (signing tool + flash programmer)
#   - Board connected via ST-LINK SWD
#
# Flash layout (external NOR at 0x70000000):
#   0x70000000: FSBL-trusted.bin   (signed FSBL, ~35KB)
#   0x70100000: Appli-signed.bin   (signed Application, ~1.5MB)
#
# The FSBL copies the Appli from 0x70100000 to AXISRAM at 0x34000000,
# then jumps to the vector table at 0x34000400.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"

# --- Toolchain paths ---
GCC_DIR="/c/ST/STM32CubeIDE_2.1.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin"
# Use CubeProgrammer v2.20 — v2.22 has issues with STM32N6 application startup
CUBEPROG_DIR="/c/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin"
EXT_LOADER="$CUBEPROG_DIR/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr"

# --- Build variant (SW_Crypto or HW_Crypto) ---
VARIANT="${BUILD_VARIANT:-SW_Crypto}"
BUILD_DIR="$PROJECT_DIR/$VARIANT"

# --- FSBL paths ---
FSBL_DIR="$SCRIPT_DIR/../FSBL/Release"
FSBL_SIGNED="$FSBL_DIR/FSBL-trusted.bin"

# --- Output names ---
ELF_NAME="stm32n6570_dk_w6x_iot_reference_kvs_webrtc_Appli"
BIN_FILE="$BUILD_DIR/${ELF_NAME}.bin"
ELF_FILE="$BUILD_DIR/${ELF_NAME}.elf"
SIGNED_FILE="$BUILD_DIR/${ELF_NAME}_signed.bin"

# --- Flash addresses ---
FSBL_FLASH_ADDR="0x70000000"
APPLI_FLASH_ADDR="0x70100000"

# Prepend correct GCC to PATH
export PATH="$GCC_DIR:$PATH"

do_build() {
    echo "=== Building $VARIANT ==="
    cd "$BUILD_DIR"

    # Regenerate objects.list from make database to handle space-in-path dirs
    echo "Regenerating objects.list..."
    # Step 1: Extract OBJS, splitting on space but NOT breaking space-in-dir-name paths
    make -p CC=arm-none-eabi-gcc 2>/dev/null \
        | grep "^OBJS " | head -1 \
        | tr ' ' '\n' \
        | grep '\.o$' \
        | sort -u > /tmp/objs_raw.txt

    # Step 2: Fix entries broken by spaces in directory names
    # Known dirs with spaces: "mbed TLS", "coreMQTT Agent", "Event Groups", "Message Buffer"
    > /tmp/objs_fixed.txt
    # Keep good entries (starting with ./)
    grep '^\.\/' /tmp/objs_raw.txt | sed 's/^/"/;s/$/"/' >> /tmp/objs_fixed.txt

    # Extract correct entries from space-name subdir.mk files
    for mk in \
        "Middlewares/mbedTLS/Security/mbed TLS/subdir.mk" \
        "Middlewares/coreMQTT_Agent/FreeRTOS/coreMQTT Agent/subdir.mk" \
        "Middlewares/FreeRTOS/RTOS/Event Groups/subdir.mk" \
        "Middlewares/FreeRTOS/RTOS/Message Buffer/subdir.mk"; do
        if [ -f "$mk" ]; then
            grep '^\.\/' "$mk" | grep '\.o' | tr -d '\\' \
                | sed 's/^ *//;s/ *$//' \
                | while IFS= read -r line; do echo "\"$line\""; done \
                >> /tmp/objs_fixed.txt
        fi
    done

    # Remove duplicates and filter out conflicts (SW_Crypto uses mbedTLS):
    # - Middlewares coreJSON (conflicts with Libraries/kvs_webrtc/coreJSON)
    # - libsrtp OpenSSL variants (keep mbedTLS: hmac_mbedtls, aes_icm_mbedtls)
    # - base64 custom impl (keep mbedTLS: base64_mbedtls)
    sort -u /tmp/objs_fixed.txt \
        | grep -v 'Middlewares/coreJSON/' \
        | grep -v 'libsrtp/crypto/hash/hmac\.o' \
        | grep -v 'libsrtp/crypto/cipher/aes_icm\.o' \
        | grep -v 'base64/custom/base64_custom\.o' \
        > objects.list
    rm -f /tmp/objs_raw.txt /tmp/objs_fixed.txt

    echo "objects.list: $(wc -l < objects.list) entries"

    # Build
    if [ "${CLEAN:-0}" = "1" ]; then
        make clean CC=arm-none-eabi-gcc 2>&1 | tail -1
    fi
    make -j$(nproc) all CC=arm-none-eabi-gcc 2>&1 | tail -5

    echo "=== Build complete ==="
    arm-none-eabi-size "$ELF_FILE"
}

do_sign() {
    echo "=== Signing $VARIANT ==="
    cd "$BUILD_DIR"

    if [ ! -f "$BIN_FILE" ]; then
        echo "ERROR: $BIN_FILE not found. Run build first."
        exit 1
    fi

    # Extract entry point from ELF
    ENTRY=$(arm-none-eabi-readelf -h "$ELF_FILE" | grep "Entry point" | awk '{print $NF}')
    echo "Entry point: $ENTRY"

    # Sign with v2.20 (auto-detects entry point and auto-aligns):
    #   -nk          no keys (unsigned, for development)
    #   -of 0x80000000  option flags
    #   -t fsbl      binary type (required for STM32N6 boot)
    #   -hv 2.3      header version for STM32N6
    # NOTE: v2.20 auto-aligns payload to 0x400 and correctly auto-detects
    #       the entry point. v2.22 gets both wrong — do NOT upgrade without testing.
    rm -f "$SIGNED_FILE"
    "$CUBEPROG_DIR/STM32_SigningTool_CLI.exe" \
        -bin "$BIN_FILE" \
        -nk -of 0x80000000 -t fsbl \
        -o "$SIGNED_FILE" -hv 2.3

    # Verify
    echo ""
    echo "Verifying signed image..."
    VT_WORD1=$(xxd -s 0x400 -l 4 -e "$SIGNED_FILE" | awk '{print $2}')
    VT_WORD2=$(xxd -s 0x404 -l 4 -e "$SIGNED_FILE" | awk '{print $2}')
    echo "  Vector table @ 0x400: SP=$VT_WORD1 Reset=$VT_WORD2"
    echo "  Expected Reset: $ENTRY"

    SIZE=$(wc -c < "$SIGNED_FILE")
    MAX_SIZE=$((0x180000))
    echo "  Signed size: $SIZE bytes (max $MAX_SIZE)"
    if [ "$SIZE" -gt "$MAX_SIZE" ]; then
        echo "ERROR: Signed binary exceeds EXTMEM_LRUN_SOURCE_SIZE (0x180000)!"
        exit 1
    fi
    echo "=== Signing complete ==="
}

do_flash() {
    echo "=== Flashing to board ==="

    if [ ! -f "$SIGNED_FILE" ]; then
        echo "ERROR: $SIGNED_FILE not found. Run sign first."
        exit 1
    fi
    if [ ! -f "$FSBL_SIGNED" ]; then
        echo "ERROR: $FSBL_SIGNED not found."
        exit 1
    fi

    echo "--- FSBL -> $FSBL_FLASH_ADDR ---"
    "$CUBEPROG_DIR/STM32_Programmer_CLI.exe" \
        -c port=SWD mode=HOTPLUG ap=1 \
        -el "$EXT_LOADER" \
        -w "$FSBL_SIGNED" "$FSBL_FLASH_ADDR"

    echo ""
    echo "--- Appli -> $APPLI_FLASH_ADDR ---"
    "$CUBEPROG_DIR/STM32_Programmer_CLI.exe" \
        -c port=SWD mode=HOTPLUG ap=1 \
        -el "$EXT_LOADER" \
        -w "$SIGNED_FILE" "$APPLI_FLASH_ADDR"

    echo ""
    echo "--- Resetting board ---"
    "$CUBEPROG_DIR/STM32_Programmer_CLI.exe" \
        -c port=SWD mode=HOTPLUG ap=1 \
        -rst 2>&1 || true

    echo "=== Flash complete — check UART output ==="
}

# --- Main ---
CMD="${1:-all}"

case "$CMD" in
    build)   do_build ;;
    sign)    do_sign ;;
    flash)   do_flash ;;
    all)     do_build && do_sign && do_flash ;;
    clean)   cd "$BUILD_DIR" && make clean CC=arm-none-eabi-gcc ;;
    *)       echo "Usage: $0 [build|sign|flash|all|clean]"; exit 1 ;;
esac
