#!/usr/bin/env bash
set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly LINEAGE_MANIFEST="https://github.com/LineageOS/android.git"
readonly LINEAGE_BRANCH="lineage-18.1"
readonly PRODUCT_OUT_REL="out/target/product/leaf3"
readonly REQUIRED_FREE_GIB=200
readonly INCREMENTAL_FREE_GIB=80

SOURCE_DIR=""
BUILD_JOBS="$(nproc)"
SYNC_JOBS="4"
DOWNLOAD_CONNECTIONS="8"
CCACHE_SIZE="30G"
PROXY_URL="${BUILD_PROXY:-}"
HTTP_PROXY_URL=""
HTTPS_PROXY_URL=""
NO_PROXY_VALUE=""
INSTALL_DEPS=0
SKIP_SYNC=0
SYNC_RETRIES=3

usage() {
  cat <<'EOF'
Build the BOOX Leaf3 LineageOS 18.1 first-boot images on Arch Linux.

Usage:
  ./build-lineage-arch.sh --source-dir PATH [options]

Required:
  --source-dir PATH       LineageOS checkout/build directory

Options:
  -j, --jobs N            Parallel build jobs (default: all logical CPUs)
  --sync-jobs N           Parallel repo sync jobs (default: 4)
  --sync-retries N        repo sync attempts (default: 3)
  --download-connections N
                           Stock ROM download connections, 1-16 (default: 8)
  --ccache-size SIZE      ccache limit (default: 30G)
  --proxy URL             Use one proxy for HTTP and HTTPS
  --http-proxy URL        HTTP proxy (overrides --proxy for HTTP)
  --https-proxy URL       HTTPS proxy (overrides --proxy for HTTPS)
  --no-proxy LIST         Comma-separated proxy exclusions
  --install-deps          Install official Arch packages with pacman
  --skip-sync             Reuse the existing source checkout without repo sync
  -h, --help              Show this help

Proxy values may also be supplied through BUILD_PROXY, http_proxy,
https_proxy, and no_proxy. CLI options take precedence.

Examples:
  ./build-lineage-arch.sh --source-dir /srv/android/lineage-18.1
  ./build-lineage-arch.sh --source-dir /srv/android/lineage-18.1 \
    --proxy http://127.0.0.1:7890 --sync-jobs 2 -j 12
EOF
}

die() {
  echo "error: $*" >&2
  exit 1
}

log() {
  printf '\n==> %s\n' "$*"
}

is_positive_integer() {
  [[ "$1" =~ ^[1-9][0-9]*$ ]]
}

validate_webview_apk() {
  local webview_dir="$SOURCE_DIR/external/chromium-webview/prebuilt/arm64"
  local webview_apk="$webview_dir/webview.apk"

  if unzip -tqq "$webview_apk" >/dev/null 2>&1; then
    return
  fi

  log "Repairing the invalid Chromium WebView Git LFS object"
  git -C "$webview_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
    die "WebView source repository is missing or invalid: $webview_dir"

  git -C "$webview_dir" lfs install --local
  git -C "$webview_dir" lfs pull --include='webview.apk'

  # Restore only this generated-source prebuilt. This replaces a corrupt file
  # or LFS pointer while leaving every other source-tree modification intact.
  git -C "$webview_dir" checkout -- webview.apk
  git -C "$webview_dir" lfs checkout webview.apk

  if ! unzip -tqq "$webview_apk"; then
    die "WebView APK is still invalid after Git LFS repair: $webview_apk"
  fi
}

while (($#)); do
  case "$1" in
    --source-dir)
      (($# >= 2)) || die "--source-dir requires a value"
      SOURCE_DIR="$2"
      shift 2
      ;;
    -j|--jobs)
      (($# >= 2)) || die "$1 requires a value"
      BUILD_JOBS="$2"
      shift 2
      ;;
    --sync-jobs)
      (($# >= 2)) || die "--sync-jobs requires a value"
      SYNC_JOBS="$2"
      shift 2
      ;;
    --sync-retries)
      (($# >= 2)) || die "--sync-retries requires a value"
      SYNC_RETRIES="$2"
      shift 2
      ;;
    --download-connections)
      (($# >= 2)) || die "--download-connections requires a value"
      DOWNLOAD_CONNECTIONS="$2"
      shift 2
      ;;
    --ccache-size)
      (($# >= 2)) || die "--ccache-size requires a value"
      CCACHE_SIZE="$2"
      shift 2
      ;;
    --proxy)
      (($# >= 2)) || die "--proxy requires a value"
      PROXY_URL="$2"
      shift 2
      ;;
    --http-proxy)
      (($# >= 2)) || die "--http-proxy requires a value"
      HTTP_PROXY_URL="$2"
      shift 2
      ;;
    --https-proxy)
      (($# >= 2)) || die "--https-proxy requires a value"
      HTTPS_PROXY_URL="$2"
      shift 2
      ;;
    --no-proxy)
      (($# >= 2)) || die "--no-proxy requires a value"
      NO_PROXY_VALUE="$2"
      shift 2
      ;;
    --install-deps)
      INSTALL_DEPS=1
      shift
      ;;
    --skip-sync)
      SKIP_SYNC=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1 (use --help)"
      ;;
  esac
done

[[ -n "$SOURCE_DIR" ]] || die "--source-dir is required"
is_positive_integer "$BUILD_JOBS" || die "--jobs must be a positive integer"
is_positive_integer "$SYNC_JOBS" || die "--sync-jobs must be a positive integer"
is_positive_integer "$SYNC_RETRIES" || die "--sync-retries must be a positive integer"
is_positive_integer "$DOWNLOAD_CONNECTIONS" || \
  die "--download-connections must be an integer from 1 to 16"
((DOWNLOAD_CONNECTIONS <= 16)) || \
  die "--download-connections must be an integer from 1 to 16"
[[ "$CCACHE_SIZE" =~ ^[1-9][0-9]*([KMGTP]([iI]?[bB])?)?$ ]] || \
  die "--ccache-size must look like 30G"

[[ "$(uname -s)" == "Linux" ]] || die "this script requires Linux"
[[ "$(uname -m)" == "x86_64" ]] || die "this script requires an x86_64 host"
[[ -r /etc/arch-release ]] || die "this script supports Arch Linux"
((EUID != 0)) || die "run this script as a normal build user, not as root"

if ((INSTALL_DEPS)); then
  log "Installing official Arch Linux build dependencies"
  sudo pacman -S --needed \
    aria2 base-devel bc bison ccache cpio curl e2fsprogs flex git git-lfs \
    gnupg gperf imagemagick inetutils jdk11-openjdk lib32-gcc-libs \
    lib32-glibc lib32-ncurses lib32-readline lib32-zlib libelf libxml2 \
    libxslt lz4 lzop ncurses openssl python python-pip repo rsync schedtool \
    squashfs-tools unzip which zip
fi

missing_commands=()
for command_name in \
  awk bash ccache cpio curl git gzip java make openssl python3 repo rsync \
  sha256sum unzip xmllint zip; do
  command -v "$command_name" >/dev/null 2>&1 || missing_commands+=("$command_name")
done
if ((${#missing_commands[@]})); then
  die "missing commands: ${missing_commands[*]}; run again with --install-deps"
fi

libtinfo_error=""
if ! libtinfo_error="$(
  python3 -c 'import ctypes; ctypes.CDLL("libtinfo.so.5")' 2>&1
)"; then
  echo "error: the dynamic loader cannot load libtinfo.so.5." >&2
  echo "Install the 64-bit AUR package ncurses5-compat-libs (not only lib32-ncurses5-compat-libs)." >&2
  echo "Loader error: $libtinfo_error" >&2
  echo "Installed package files can be checked with:" >&2
  echo "  pacman -Ql ncurses5-compat-libs | grep libtinfo" >&2
  exit 1
fi

if [[ -n "$PROXY_URL" ]]; then
  : "${HTTP_PROXY_URL:=$PROXY_URL}"
  : "${HTTPS_PROXY_URL:=$PROXY_URL}"
fi
: "${HTTP_PROXY_URL:=${http_proxy:-${HTTP_PROXY:-}}}"
: "${HTTPS_PROXY_URL:=${https_proxy:-${HTTPS_PROXY:-}}}"
: "${NO_PROXY_VALUE:=${no_proxy:-${NO_PROXY:-}}}"

if [[ -n "$HTTP_PROXY_URL" ]]; then
  export http_proxy="$HTTP_PROXY_URL"
  export HTTP_PROXY="$HTTP_PROXY_URL"
fi
if [[ -n "$HTTPS_PROXY_URL" ]]; then
  export https_proxy="$HTTPS_PROXY_URL"
  export HTTPS_PROXY="$HTTPS_PROXY_URL"
fi
if [[ -n "$NO_PROXY_VALUE" ]]; then
  export no_proxy="$NO_PROXY_VALUE"
  export NO_PROXY="$NO_PROXY_VALUE"
fi

mkdir -p "$SOURCE_DIR"
SOURCE_DIR="$(cd -- "$SOURCE_DIR" && pwd)"
readonly SOURCE_DIR
readonly TARGET_DEVICE_DIR="$SOURCE_DIR/device/onyx/leaf3"
readonly STOCK_CACHE_DIR="$SOURCE_DIR/.leaf3-cache/stock"
readonly STOCK_IMAGES_DIR="$SOURCE_DIR/.leaf3-cache/stock-images"
readonly CCACHE_DIR_PATH="${CCACHE_DIR:-$SOURCE_DIR/.ccache}"

available_kib="$(df -Pk "$SOURCE_DIR" | awk 'NR == 2 {print $4}')"
if [[ -d "$SOURCE_DIR/.repo" ]]; then
  minimum_free_gib="$INCREMENTAL_FREE_GIB"
else
  minimum_free_gib="$REQUIRED_FREE_GIB"
fi
required_kib=$((minimum_free_gib * 1024 * 1024))
if ((available_kib < required_kib)); then
  available_gib=$((available_kib / 1024 / 1024))
  die "only ${available_gib} GiB is free at $SOURCE_DIR; at least ${minimum_free_gib} GiB is required"
fi

if ((SKIP_SYNC == 0)); then
  log "Initializing LineageOS ${LINEAGE_BRANCH}"
  cd "$SOURCE_DIR"
  if [[ ! -d .repo ]]; then
    repo init \
      --depth=1 \
      --git-lfs \
      --no-clone-bundle \
      -u "$LINEAGE_MANIFEST" \
      -b "$LINEAGE_BRANCH"
  fi

  log "Synchronizing LineageOS source"
  sync_succeeded=0
  for ((attempt = 1; attempt <= SYNC_RETRIES; attempt++)); do
    echo "repo sync attempt ${attempt}/${SYNC_RETRIES}"
    if repo sync \
      -c \
      --no-clone-bundle \
      --no-tags \
      --optimized-fetch \
      --fail-fast \
      -j"$SYNC_JOBS"; then
      sync_succeeded=1
      break
    fi
  done
  ((sync_succeeded)) || die "repo sync failed after ${SYNC_RETRIES} attempts"
else
  [[ -f "$SOURCE_DIR/build/envsetup.sh" ]] || \
    die "--skip-sync was used, but $SOURCE_DIR is not a complete LineageOS checkout"
fi

log "Validating the ARM64 Chromium WebView prebuilt"
validate_webview_apk

log "Installing the Leaf3 device tree into the source checkout"
mkdir -p "$TARGET_DEVICE_DIR"
if [[ "$SCRIPT_DIR" != "$TARGET_DEVICE_DIR" ]]; then
  rsync -a \
    --exclude='.git/' \
    --exclude='.github/' \
    --exclude='.cache/' \
    --exclude='stock-images/' \
    "$SCRIPT_DIR/" "$TARGET_DEVICE_DIR/"
fi

log "Downloading and extracting checksum-pinned stock boot inputs"
mkdir -p "$STOCK_CACHE_DIR" "$STOCK_IMAGES_DIR"
BOOX_DOWNLOAD_CONNECTIONS="$DOWNLOAD_CONNECTIONS" \
  BOOX_PAGE_35_STOCK_CACHE="$STOCK_CACHE_DIR" \
  "$TARGET_DEVICE_DIR/prepare-stock-images.sh" "$STOCK_IMAGES_DIR"

log "Preparing the stock kernel, DTB, DTBO, and e-ink waveform"
ANDROID_BUILD_TOP="$SOURCE_DIR" \
  "$TARGET_DEVICE_DIR/prepare-kernel.sh" \
  "$STOCK_IMAGES_DIR/boot.img" \
  "$STOCK_IMAGES_DIR/dtbo.img" \
  "$STOCK_IMAGES_DIR/recovery.img"

log "Configuring ccache"
mkdir -p "$CCACHE_DIR_PATH"
export USE_CCACHE=1
export CCACHE_EXEC=/usr/bin/ccache
export CCACHE_DIR="$CCACHE_DIR_PATH"
ccache --max-size="$CCACHE_SIZE"

log "Building Leaf3 first-boot images with ${BUILD_JOBS} jobs"
cd "$SOURCE_DIR"
export JAVA_HOME=/usr/lib/jvm/java-11-openjdk
export PATH="$JAVA_HOME/bin:$PATH"
(
  # Android's shell helpers are not written for Bash nounset mode.
  set +u
  source build/envsetup.sh
  lunch lineage_leaf3-userdebug
  mka -j"$BUILD_JOBS" \
    bootimage \
    dtboimage \
    systemimage \
    productimage \
    systemextimage \
    vbmetaimage
)

log "Verifying build outputs"
readonly PRODUCT_OUT="$SOURCE_DIR/$PRODUCT_OUT_REL"
readonly VERIFY_DIR="$SOURCE_DIR/.leaf3-cache/verify-boot"
readonly RAMDISK_LIST="$SOURCE_DIR/.leaf3-cache/leaf3-ramdisk.list"
readonly OUTPUT_FILES=(boot.img dtbo.img system.img product.img system_ext.img vbmeta.img)

for output_file in "${OUTPUT_FILES[@]}"; do
  [[ -s "$PRODUCT_OUT/$output_file" ]] || die "missing build output: $output_file"
done
[[ "$(stat -c %s "$PRODUCT_OUT/boot.img")" -le 100663296 ]] || \
  die "boot.img exceeds the 96 MiB partition size"
[[ "$(stat -c %s "$PRODUCT_OUT/dtbo.img")" -le 25165824 ]] || \
  die "dtbo.img exceeds the 24 MiB partition size"
cmp "$PRODUCT_OUT/dtbo.img" "$TARGET_DEVICE_DIR/prebuilt/dtbo.img"

rm -rf -- "$VERIFY_DIR"
mkdir -p "$VERIFY_DIR"
python3 "$SOURCE_DIR/system/tools/mkbootimg/unpack_bootimg.py" \
  --boot_img "$PRODUCT_OUT/boot.img" \
  --out "$VERIFY_DIR" >/dev/null
cmp "$VERIFY_DIR/kernel" "$TARGET_DEVICE_DIR/prebuilt/kernel"
cmp "$VERIFY_DIR/dtb" "$TARGET_DEVICE_DIR/prebuilt/dtb/leaf3.dtb"
gzip -dc "$VERIFY_DIR/ramdisk" | cpio -t > "$RAMDISK_LIST"
grep -Fxq 'fstab.emmc' "$RAMDISK_LIST" || die "boot ramdisk is missing fstab.emmc"
grep -Fxq 'waveform/eink_waveform.wbf' "$RAMDISK_LIST" || \
  die "boot ramdisk is missing the e-ink waveform"

(
  cd "$PRODUCT_OUT"
  sha256sum "${OUTPUT_FILES[@]}" > lineage-leaf3-images.sha256sum
)

ccache --show-stats
log "Build complete"
echo "Images: $PRODUCT_OUT"
echo "Checksums: $PRODUCT_OUT/lineage-leaf3-images.sha256sum"
