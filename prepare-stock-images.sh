#!/usr/bin/env bash
set -euo pipefail

# Download the checksum-pinned Page 3.5 OTA and make the partitions needed by
# the Android 11 bring-up.  This avoids committing proprietary images.
DEVICE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_DIR="${BOOX_PAGE_35_STOCK_CACHE:-$DEVICE_DIR/.cache/page-3.5-stock}"
OUT_DIR="${1:-$DEVICE_DIR/stock-images}"
TOOLS_DIR="$CACHE_DIR/tools"
VENV_DIR="$CACHE_DIR/venv"
FIRMWARE_URL="http://firmware-us-volc.boox.com/73efa5396d8ff9f53fd34a7e282b8053/update.upx"
UPX_SHA256="0be5912e1bc73a8177abe03623f0d1140c01184c49a2f7d7012b43573f6e148c"
DECRYPT_REVISION="ddcabf6ce27f1acff51a2506b597d506e5f1a928"
PAYLOAD_REVISION="6952cd8095573b14cae24198fe923347a13790df"

sha256_file() { sha256sum "$1" | awk '{print $1}'; }
clone_pinned() {
  local url="$1" revision="$2" destination="$3"
  if [[ ! -d "$destination/.git" ]]; then
    git clone --filter=blob:none --no-checkout "$url" "$destination"
  fi
  git -C "$destination" cat-file -e "$revision^{commit}" 2>/dev/null || \
    git -C "$destination" fetch --depth=1 origin "$revision"
  git -C "$destination" checkout --detach --force "$revision"
}

for command in curl git python3 unzip sha256sum; do
  command -v "$command" >/dev/null || { echo "Missing command: $command" >&2; exit 1; }
done

mkdir -p "$CACHE_DIR" "$TOOLS_DIR" "$OUT_DIR"
if [[ ! -x "$VENV_DIR/bin/python" ]]; then python3 -m venv "$VENV_DIR"; fi
"$VENV_DIR/bin/python" -m pip install --disable-pip-version-check protobuf==3.20.3 pycryptodome==3.23.0
clone_pinned https://github.com/Hagb/decryptBooxUpdateUpx.git "$DECRYPT_REVISION" "$TOOLS_DIR/decryptBooxUpdateUpx"
clone_pinned https://github.com/cyxx/extract_android_ota_payload.git "$PAYLOAD_REVISION" "$TOOLS_DIR/extract_android_ota_payload"

UPX_FILE="$CACHE_DIR/update.upx"
if [[ ! -f "$UPX_FILE" ]]; then
  curl --fail --location --retry 3 --continue-at - --output "$UPX_FILE.part" "$FIRMWARE_URL"
  mv "$UPX_FILE.part" "$UPX_FILE"
fi
[[ "$(sha256_file "$UPX_FILE")" == "$UPX_SHA256" ]] || { echo "OTA checksum mismatch" >&2; exit 1; }

WORK_DIR="$(mktemp -d "$CACHE_DIR/work.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT
ZIP_FILE="$WORK_DIR/update.zip"
PAYLOAD_FILE="$WORK_DIR/payload.bin"
"$VENV_DIR/bin/python" "$TOOLS_DIR/decryptBooxUpdateUpx/DeBooxUpx.py" Page "$UPX_FILE" "$ZIP_FILE"
unzip -p "$ZIP_FILE" payload.bin > "$PAYLOAD_FILE"
"$VENV_DIR/bin/python" "$DEVICE_DIR/tools/extract-stock-images.py" \
  "$TOOLS_DIR/extract_android_ota_payload" "$PAYLOAD_FILE" "$OUT_DIR"
echo "Stock images extracted to $OUT_DIR"
