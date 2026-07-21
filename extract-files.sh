#!/usr/bin/env bash
set -euo pipefail

# Extract every file from a stock vendor ext4 image without mounting it.  This
# is intended for a Linux LineageOS build host with e2fsprogs installed.
DEVICE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${1:-$DEVICE_DIR/../stock-images/vendor.img}"
VENDOR_DIR="${ANDROID_BUILD_TOP:-$(cd "$DEVICE_DIR/../../.." && pwd)}/vendor/onyx/leaf3"
PROPRIETARY_DIR="$VENDOR_DIR/proprietary"

command -v debugfs >/dev/null || { echo "debugfs (e2fsprogs) is required." >&2; exit 1; }
[[ -f "$IMAGE" ]] || { echo "Missing vendor image: $IMAGE" >&2; exit 1; }

rm -rf "$PROPRIETARY_DIR"
mkdir -p "$PROPRIETARY_DIR"
debugfs -R "rdump / $PROPRIETARY_DIR/vendor" "$IMAGE" >/dev/null 2>&1
rm -rf "$PROPRIETARY_DIR/vendor/lost+found"
"$DEVICE_DIR/setup-makefiles.sh" "$VENDOR_DIR"
echo "Extracted vendor blobs to $VENDOR_DIR"
