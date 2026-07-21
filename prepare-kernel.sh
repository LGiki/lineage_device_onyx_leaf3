#!/usr/bin/env bash
set -euo pipefail

DEVICE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_ROOT="${ANDROID_BUILD_TOP:-$(cd "$DEVICE_DIR/../../.." && pwd)}"
BOOT_IMAGE="${1:-$DEVICE_DIR/../stock-images/boot.img}"
UNPACK_BOOTIMG="$ANDROID_ROOT/system/tools/mkbootimg/unpack_bootimg.py"
OUT_DIR="$DEVICE_DIR/prebuilt"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

[[ -f "$BOOT_IMAGE" ]] || { echo "Missing boot image: $BOOT_IMAGE" >&2; exit 1; }
[[ -f "$UNPACK_BOOTIMG" ]] || { echo "Run from a LineageOS 18.1 source tree." >&2; exit 1; }

python3 "$UNPACK_BOOTIMG" --boot_img "$BOOT_IMAGE" --out "$WORK_DIR"
install -Dm0644 "$WORK_DIR/kernel" "$OUT_DIR/kernel"
install -Dm0644 "$WORK_DIR/dtb" "$OUT_DIR/dtb/leaf3.dtb"
echo "Prepared stock kernel and DTB in $OUT_DIR"
