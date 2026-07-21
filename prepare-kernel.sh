#!/usr/bin/env bash
set -euo pipefail

DEVICE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_ROOT="${ANDROID_BUILD_TOP:-$(cd "$DEVICE_DIR/../../.." && pwd)}"
BOOT_IMAGE="${1:-$DEVICE_DIR/stock-images/boot.img}"
DTBO_IMAGE="${2:-$(dirname "$BOOT_IMAGE")/dtbo.img}"
RECOVERY_IMAGE="${3:-$(dirname "$BOOT_IMAGE")/recovery.img}"
UNPACK_BOOTIMG="$ANDROID_ROOT/system/tools/mkbootimg/unpack_bootimg.py"
OUT_DIR="$DEVICE_DIR/prebuilt"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

[[ -f "$BOOT_IMAGE" ]] || { echo "Missing boot image: $BOOT_IMAGE" >&2; exit 1; }
[[ -f "$DTBO_IMAGE" ]] || { echo "Missing DTBO image: $DTBO_IMAGE" >&2; exit 1; }
[[ -f "$RECOVERY_IMAGE" ]] || { echo "Missing recovery image: $RECOVERY_IMAGE" >&2; exit 1; }
[[ -f "$UNPACK_BOOTIMG" ]] || { echo "Run from a LineageOS 18.1 source tree." >&2; exit 1; }

python3 "$UNPACK_BOOTIMG" --boot_img "$BOOT_IMAGE" --out "$WORK_DIR"
install -Dm0644 "$WORK_DIR/kernel" "$OUT_DIR/kernel"
install -Dm0644 "$WORK_DIR/dtb" "$OUT_DIR/dtb/leaf3.dtb"
install -Dm0644 "$DTBO_IMAGE" "$OUT_DIR/dtbo.img"

RECOVERY_OUT="$WORK_DIR/recovery"
RECOVERY_RAMDISK="$WORK_DIR/recovery-ramdisk"
mkdir -p "$RECOVERY_OUT" "$RECOVERY_RAMDISK"
python3 "$UNPACK_BOOTIMG" --boot_img "$RECOVERY_IMAGE" --out "$RECOVERY_OUT" >/dev/null
(
  cd "$RECOVERY_RAMDISK"
  gzip -dc "$RECOVERY_OUT/ramdisk" | cpio -idm waveform/eink_waveform.wbf 2>/dev/null
)
[[ -s "$RECOVERY_RAMDISK/waveform/eink_waveform.wbf" ]] || {
  echo "Recovery ramdisk does not contain the e-ink waveform" >&2
  exit 1
}
install -Dm0644 "$RECOVERY_RAMDISK/waveform/eink_waveform.wbf" "$OUT_DIR/eink_waveform.wbf"
echo "Prepared stock kernel, DTB, DTBO and e-ink waveform in $OUT_DIR"
