#!/usr/bin/env zsh
set -e

SCRIPT_DIR="${0:A:h}"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

# Automatically use the Arm GNU Toolchain (with newlib/nosys.specs) installed in ~/.pico-sdk/toolchain
TOOLCHAIN_DIR="$HOME/.pico-sdk/toolchain/arm-gnu-toolchain-14.2.rel1-darwin-arm64-arm-none-eabi/bin"
if [[ -d "$TOOLCHAIN_DIR" ]]; then
    export PICO_TOOLCHAIN_PATH="$TOOLCHAIN_DIR"
    export PATH="$TOOLCHAIN_DIR:$PATH"
fi

export PICO_SDK_PATH="${PICO_SDK_PATH:-$SCRIPT_DIR/pico-sdk}"

case "${1:-build}" in
    clean)
        echo "==> Cleaning build directory..."
        rm -rf "$SCRIPT_DIR/build"
        ;;
    flash)
        echo "==> Building and flashing via Pico Debug Probe (OpenOCD)..."
        "$0" build
        openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
            -c "adapter speed 1000" \
            -c "program $SCRIPT_DIR/build/pico_uac_i2s_bridge.elf verify reset exit"
        ;;
    build|*)
        mkdir -p "$SCRIPT_DIR/build"
        cd "$SCRIPT_DIR/build"
        cmake -GNinja \
            -DPICO_BOARD=pico_w \
            -DPICO_NO_PICOTOOL=1 \
            ${PICO_TOOLCHAIN_PATH:+-DPICO_TOOLCHAIN_PATH="$PICO_TOOLCHAIN_PATH"} \
            ..
        ninja
        python3 -c '
import struct, sys
UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
RP2040_FAMILY_ID, FLASH_BASE = 0xE48BFF56, 0x10000000
data = open(sys.argv[1], "rb").read()
if len(data) % 256: data += b"\x00" * (256 - (len(data) % 256))
num_blocks = len(data) // 256
with open(sys.argv[2], "wb") as out:
    for b in range(num_blocks):
        hdr = struct.pack("<IIIIIIII", UF2_MAGIC_START0, UF2_MAGIC_START1, 0x2000, FLASH_BASE + b * 256, 256, b, num_blocks, RP2040_FAMILY_ID)
        out.write(hdr + data[b*256:(b+1)*256] + b"\x00"*220 + struct.pack("<I", UF2_MAGIC_END))
' "$SCRIPT_DIR/build/pico_uac_i2s_bridge.bin" "$SCRIPT_DIR/build/pico_uac_i2s_bridge.uf2"
        echo ""
        echo "========================================================"
        echo " Build Complete!"
        echo "  UF2 : $SCRIPT_DIR/build/pico_uac_i2s_bridge.uf2"
        echo "  ELF : $SCRIPT_DIR/build/pico_uac_i2s_bridge.elf"
        echo "========================================================"
        ;;
esac
