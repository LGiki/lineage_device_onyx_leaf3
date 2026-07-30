#!/usr/bin/env bash
set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly LINEAGE_MANIFEST="https://github.com/LineageOS/android.git"
readonly LINEAGE_BRANCH="lineage-18.1"
readonly PRODUCT_OUT_REL="out/target/product/leaf3"
readonly REQUIRED_FREE_GIB=200

SOURCE_DIR=""
BUILD_JOBS="$(nproc)"
SYNC_JOBS="4"
DOWNLOAD_CONNECTIONS="8"
CCACHE_SIZE="30G"
PROXY_URL="${BUILD_PROXY:-}"
HTTP_PROXY_URL=""
HTTPS_PROXY_URL=""
NO_PROXY_VALUE=""
ADB_PUBLIC_KEY=""
INSTALL_DEPS=0
SKIP_SYNC=0
SYNC_RETRIES=3

usage() {
  cat <<'EOF'
Build the BOOX Leaf3 LineageOS 18.1 first-boot images and OTA package on
Arch Linux.

Usage:
  ./build-lineage-arch.sh --source-dir PATH [options]

Required:
  --source-dir PATH       LineageOS checkout/build directory
  --adb-public-key PATH   Public key for the computer that will run ADB

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
  ./build-lineage-arch.sh --source-dir /srv/android/lineage-18.1 \
    --adb-public-key /srv/keys/leaf3-adbkey.pub
  ./build-lineage-arch.sh --source-dir /srv/android/lineage-18.1 \
    --proxy http://127.0.0.1:7890 \
    --adb-public-key /srv/keys/leaf3-adbkey.pub \
    --sync-jobs 2 -j 12
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
    --adb-public-key)
      (($# >= 2)) || die "--adb-public-key requires a value"
      ADB_PUBLIC_KEY="$2"
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

if [[ -n "$ADB_PUBLIC_KEY" ]]; then
  [[ -f "$ADB_PUBLIC_KEY" && -r "$ADB_PUBLIC_KEY" ]] || \
    die "ADB public key is not a readable regular file: $ADB_PUBLIC_KEY"
  ADB_PUBLIC_KEY="$(cd -- "$(dirname -- "$ADB_PUBLIC_KEY")" && pwd)/$(basename -- "$ADB_PUBLIC_KEY")"
elif [[ -f "$SCRIPT_DIR/debug-adb-key.pub" ]]; then
  ADB_PUBLIC_KEY="$SCRIPT_DIR/debug-adb-key.pub"
else
  die "an ADB public key is required; pass --adb-public-key with the adbkey.pub from the computer that will debug the device"
fi
[[ "$(wc -l < "$ADB_PUBLIC_KEY")" -le 2 ]] || \
  die "ADB key file has unexpected content; pass the public adbkey.pub, never the private adbkey"
grep -Eq '^[A-Za-z0-9+/=]+([[:space:]]+[^[:space:]]+)?[[:space:]]*$' "$ADB_PUBLIC_KEY" || \
  die "invalid ADB public key format: $ADB_PUBLIC_KEY"

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
  awk bash c++ ccache cpio curl git gzip java make openssl python3 repo rsync \
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

if [[ ! -d "$SOURCE_DIR/.repo" ]]; then
  available_kib="$(df -Pk "$SOURCE_DIR" | awk 'NR == 2 {print $4}')"
  required_kib=$((REQUIRED_FREE_GIB * 1024 * 1024))
  if ((available_kib < required_kib)); then
    available_gib=$((available_kib / 1024 / 1024))
    die "only ${available_gib} GiB is free at $SOURCE_DIR; at least ${REQUIRED_FREE_GIB} GiB is required for a new checkout"
  fi
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
    --delete \
    --exclude='.git/' \
    --exclude='.github/' \
    --exclude='.cache/' \
    --exclude='prebuilt/' \
    --exclude='stock-images/' \
    "$SCRIPT_DIR/" "$TARGET_DEVICE_DIR/"
fi

# Older revisions installed the bridge policy directly under sepolicy/. It was
# later moved to the system_ext private policy directory. Remove the obsolete
# copy even when this script itself is running from TARGET_DEVICE_DIR, where
# the rsync mirror step is intentionally skipped.
readonly OBSOLETE_EPDC_POLICY="$TARGET_DEVICE_DIR/sepolicy/leaf3_epdc_bridge.te"
if [[ -e "$OBSOLETE_EPDC_POLICY" ]]; then
  log "Removing obsolete flat EPDC bridge policy"
  rm -f -- "$OBSOLETE_EPDC_POLICY"
fi

if [[ "$ADB_PUBLIC_KEY" != "$TARGET_DEVICE_DIR/debug-adb-key.pub" ]]; then
  cp -- "$ADB_PUBLIC_KEY" "$TARGET_DEVICE_DIR/debug-adb-key.pub"
fi

log "Patching the LineageOS 18.1 SystemUI AssistManager threading bug"
readonly ASSIST_MANAGER_SOURCE="$SOURCE_DIR/frameworks/base/packages/SystemUI/src/com/android/systemui/assist/AssistManager.java"
[[ -f "$ASSIST_MANAGER_SOURCE" ]] || \
  die "missing SystemUI AssistManager source: $ASSIST_MANAGER_SOURCE"
python3 "$TARGET_DEVICE_DIR/tools/patch-systemui-assist-handler.py" \
  "$ASSIST_MANAGER_SOURCE"

log "Pinning Leaf3 navigation icons dark for the white E-Ink navigation bar"
readonly LIGHT_BAR_CONTROLLER_SOURCE="$SOURCE_DIR/frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/LightBarController.java"
[[ -f "$LIGHT_BAR_CONTROLLER_SOURCE" ]] || \
  die "missing SystemUI LightBarController source: $LIGHT_BAR_CONTROLLER_SOURCE"
python3 "$TARGET_DEVICE_DIR/tools/patch-systemui-dark-navigation-icons.py" \
  "$LIGHT_BAR_CONTROLLER_SOURCE"

log "Adding the optional Leaf3 E-Ink navigation buttons"
readonly NAVIGATION_BAR_INFLATER_SOURCE="$SOURCE_DIR/frameworks/base/packages/SystemUI/src/com/android/systemui/statusbar/phone/NavigationBarInflaterView.java"
[[ -f "$NAVIGATION_BAR_INFLATER_SOURCE" ]] || \
  die "missing SystemUI NavigationBarInflaterView source: $NAVIGATION_BAR_INFLATER_SOURCE"
python3 "$TARGET_DEVICE_DIR/tools/patch-systemui-leaf3-refresh-button.py" \
  "$NAVIGATION_BAR_INFLATER_SOURCE"

log "Adding per-app animation filtering and transient E-Ink view hints"
readonly FRAMEWORKS_BASE_SOURCE="$SOURCE_DIR/frameworks/base"
readonly FRAMEWORK_EINK_PATCHER="$SCRIPT_DIR/tools/patch-framework-leaf3-eink.py"
readonly INSTALLED_FRAMEWORK_EINK_PATCHER="$TARGET_DEVICE_DIR/tools/patch-framework-leaf3-eink.py"
[[ -f "$FRAMEWORK_EINK_PATCHER" ]] || \
  die "missing framework E-Ink patcher: $FRAMEWORK_EINK_PATCHER"
cmp "$FRAMEWORK_EINK_PATCHER" "$INSTALLED_FRAMEWORK_EINK_PATCHER" || \
  die "installed framework E-Ink patcher differs from $FRAMEWORK_EINK_PATCHER"
python3 "$FRAMEWORK_EINK_PATCHER" --version
python3 "$TARGET_DEVICE_DIR/tools/test-framework-leaf3-eink.py"
python3 "$FRAMEWORK_EINK_PATCHER" \
  "$FRAMEWORKS_BASE_SOURCE"

log "Adding the Leaf3 SurfaceFlinger notifier and composer EPDC transport"
readonly FRAMEWORKS_NATIVE_SOURCE="$SOURCE_DIR/frameworks/native"
readonly SURFACEFLINGER_SOURCE="$SOURCE_DIR/frameworks/native/services/surfaceflinger/SurfaceFlinger.cpp"
readonly COMPOSER_EPDC_PATCHER="$SCRIPT_DIR/tools/patch-lineage-composer-epdc.py"
readonly INSTALLED_COMPOSER_EPDC_PATCHER="$TARGET_DEVICE_DIR/tools/patch-lineage-composer-epdc.py"
readonly FRAME_NOTIFIER_PATCHER="$SCRIPT_DIR/tools/patch-surfaceflinger-frame-notifier.py"
readonly INSTALLED_FRAME_NOTIFIER_PATCHER="$TARGET_DEVICE_DIR/tools/patch-surfaceflinger-frame-notifier.py"
[[ -f "$SURFACEFLINGER_SOURCE" ]] || \
  die "missing SurfaceFlinger source: $SURFACEFLINGER_SOURCE"
[[ -f "$COMPOSER_EPDC_PATCHER" ]] || \
  die "missing composer-native EPDC patcher: $COMPOSER_EPDC_PATCHER"
[[ -f "$FRAME_NOTIFIER_PATCHER" ]] || \
  die "missing SurfaceFlinger patcher: $FRAME_NOTIFIER_PATCHER"
cmp "$COMPOSER_EPDC_PATCHER" "$INSTALLED_COMPOSER_EPDC_PATCHER" || \
  die "installed composer EPDC patcher differs from $COMPOSER_EPDC_PATCHER"
cmp "$FRAME_NOTIFIER_PATCHER" "$INSTALLED_FRAME_NOTIFIER_PATCHER" || \
  die "installed SurfaceFlinger patcher differs from $FRAME_NOTIFIER_PATCHER"
python3 "$COMPOSER_EPDC_PATCHER" --version
python3 "$TARGET_DEVICE_DIR/tools/test-composer-epdc-abi.py"
python3 "$COMPOSER_EPDC_PATCHER" \
  "$FRAMEWORKS_NATIVE_SOURCE"
python3 "$FRAME_NOTIFIER_PATCHER" --version
python3 "$TARGET_DEVICE_DIR/tools/test-surfaceflinger-frame-notifier.py"
python3 "$FRAME_NOTIFIER_PATCHER" \
  "$SURFACEFLINGER_SOURCE"

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

  # The preserved ONYX 4.19 kernel deliberately enables CONFIG_PM_AUTOSLEEP.
  # Patch only its generated FCM 5 block, matching the stock system matrix,
  # then rebuild the two images whose hashes are affected.
  python3 "$TARGET_DEVICE_DIR/tools/patch-vintf-kernel-matrix.py" \
    "$SOURCE_DIR/$PRODUCT_OUT_REL/system/etc/vintf/compatibility_matrix.5.xml"
  mka -j"$BUILD_JOBS" systemimage vbmetaimage

  # PRODUCT_VIRTUAL_AB_OTA and AB_OTA_PARTITIONS in device.mk make this a
  # recovery-sideloadable A/B payload.  In particular, do not make a custom
  # updater ZIP: system, product, and system_ext are sparse logical images
  # that must be applied by update_engine rather than written with dd.
  mka -j"$BUILD_JOBS" otapackage
)

log "Verifying build outputs"
readonly PRODUCT_OUT="$SOURCE_DIR/$PRODUCT_OUT_REL"
readonly VERIFY_DIR="$SOURCE_DIR/.leaf3-cache/verify-boot"
readonly VERIFY_BOOT_INFO="$SOURCE_DIR/.leaf3-cache/leaf3-boot-info.txt"
readonly BUILT_DTBO_INFO="$SOURCE_DIR/.leaf3-cache/leaf3-built-dtbo-info.txt"
readonly STOCK_DTBO_INFO="$SOURCE_DIR/.leaf3-cache/leaf3-stock-dtbo-info.txt"
readonly RAMDISK_LIST="$SOURCE_DIR/.leaf3-cache/leaf3-ramdisk.list"
readonly AVBTOOL="$SOURCE_DIR/out/host/linux-x86/bin/avbtool"
readonly OUTPUT_FILES=(boot.img dtbo.img system.img product.img system_ext.img vbmeta.img)

for output_file in "${OUTPUT_FILES[@]}"; do
  [[ -s "$PRODUCT_OUT/$output_file" ]] || die "missing build output: $output_file"
done

# Lineage names OTA packages from the configured version and build date, so
# discover the package instead of duplicating that naming policy here.
mapfile -t OTA_CANDIDATES < <(
  find "$PRODUCT_OUT" -maxdepth 1 -type f -name '*-leaf3.zip' -printf '%T@ %p\n' |
    sort -nr | awk 'NR == 1 { sub(/^[^ ]+ /, ""); print }'
)
[[ ${#OTA_CANDIDATES[@]} -eq 1 && -s "${OTA_CANDIDATES[0]}" ]] || \
  die "missing OTA package in $PRODUCT_OUT"
readonly OTA_PACKAGE="${OTA_CANDIDATES[0]}"
unzip -tqq "$OTA_PACKAGE" || die "OTA package is corrupt: $OTA_PACKAGE"
unzip -Z1 "$OTA_PACKAGE" | grep -Fxq 'payload.bin' || \
  die "OTA package is missing payload.bin"
unzip -Z1 "$OTA_PACKAGE" | grep -Fxq 'payload_properties.txt' || \
  die "OTA package is missing payload_properties.txt"
unzip -p "$OTA_PACKAGE" META-INF/com/android/metadata | \
  grep -Fxq 'ota-type=AB' || die "OTA package is not an A/B OTA"
[[ -s "$PRODUCT_OUT/system/framework/org.lineageos.platform-res.apk" ]] || \
  die "system is missing org.lineageos.platform-res.apk; the product must inherit the Lineage common configuration"
[[ -x "$PRODUCT_OUT/system_ext/bin/leaf3_epdc_bridge" ]] || \
  die "system_ext is missing the Leaf3 SurfaceFlinger-to-EPDC display bridge"
[[ -s "$PRODUCT_OUT/system_ext/etc/init/leaf3_epdc_bridge.rc" ]] || \
  die "system_ext is missing the Leaf3 EPDC bridge init service"
[[ -x "$PRODUCT_OUT/system/bin/leaf3-refresh" ]] || \
  die "system is missing the Leaf3 refresh-mode control tool"
[[ -x "$PRODUCT_OUT/system/bin/leaf3-frontlight" ]] || \
  die "system is missing the Leaf3 frontlight control tool"
[[ -s "$PRODUCT_OUT/system_ext/priv-app/Leaf3Controls/Leaf3Controls.apk" ]] || \
  die "system_ext is missing the Leaf3 Controls app"
readonly LEAF3_PRIVAPP_PERMISSIONS="$PRODUCT_OUT/system_ext/etc/permissions/privapp-permissions-org.lineageos.leaf3controls.xml"
[[ -s "$LEAF3_PRIVAPP_PERMISSIONS" ]] || \
  die "system_ext is missing the Leaf3 Controls privileged-permission allowlist"
grep -Fq 'android.permission.CONTROL_DISPLAY_COLOR_TRANSFORMS' \
  "$LEAF3_PRIVAPP_PERMISSIONS" || \
  die "Leaf3 Controls allowlist is missing display color-transform access"
grep -Fq 'android.permission.REAL_GET_TASKS' "$LEAF3_PRIVAPP_PERMISSIONS" || \
  die "Leaf3 Controls allowlist is missing foreground-task access"
grep -Fq 'android.permission.REBOOT' "$LEAF3_PRIVAPP_PERMISSIONS" || \
  die "Leaf3 Controls allowlist is missing backend-switch reboot access"
python3 "$TARGET_DEVICE_DIR/tools/patch-systemui-assist-handler.py" --check \
  "$ASSIST_MANAGER_SOURCE"
python3 "$TARGET_DEVICE_DIR/tools/patch-systemui-dark-navigation-icons.py" --check \
  "$LIGHT_BAR_CONTROLLER_SOURCE"
python3 "$TARGET_DEVICE_DIR/tools/patch-systemui-leaf3-refresh-button.py" --check \
  "$NAVIGATION_BAR_INFLATER_SOURCE"
python3 "$FRAMEWORK_EINK_PATCHER" --check \
  "$FRAMEWORKS_BASE_SOURCE"
python3 "$COMPOSER_EPDC_PATCHER" --check \
  "$FRAMEWORKS_NATIVE_SOURCE"
python3 "$FRAME_NOTIFIER_PATCHER" --check \
  "$SURFACEFLINGER_SOURCE"
python3 "$TARGET_DEVICE_DIR/tools/patch-vintf-kernel-matrix.py" --check \
  "$PRODUCT_OUT/system/etc/vintf/compatibility_matrix.5.xml"
readonly SURFACEFLINGER_LIBRARY="$PRODUCT_OUT/system/lib64/libsurfaceflinger.so"
[[ -s "$SURFACEFLINGER_LIBRARY" ]] || \
  die "system is missing the 64-bit SurfaceFlinger library"
grep -aFq 'Leaf3 frame notifier registered' "$SURFACEFLINGER_LIBRARY" || \
  die "built SurfaceFlinger library is missing the Leaf3 frame notifier"
grep -aFq 'composer-native EPDC transport is ready' "$SURFACEFLINGER_LIBRARY" || \
  die "built SurfaceFlinger library is missing composer-native EPDC"
grep -Fxq 'ro.adb.secure=1' "$PRODUCT_OUT/system/etc/prop.default" || \
  die "build did not enable authenticated ADB in system/etc/prop.default"
grep -Fxq 'persist.sys.leaf3.capture_mode=notify' \
  "$PRODUCT_OUT/system/etc/prop.default" || \
  die "build did not make frame notification the default capture mode"
grep -Fxq 'persist.sys.leaf3.epdc_backend=bridge' \
  "$PRODUCT_OUT/system/etc/prop.default" || \
  die "build did not keep the direct-EBC bridge as the safe EPDC default"
grep -Fxq 'persist.sys.leaf3.nav_refresh_button=0' \
  "$PRODUCT_OUT/system/etc/prop.default" || \
  die "build did not default the Leaf3 navigation refresh button to disabled"
grep -Fxq 'persist.sys.leaf3.nav_eink_center_button=0' \
  "$PRODUCT_OUT/system/etc/prop.default" || \
  die "build did not default the Leaf3 E-Ink Center button to disabled"
[[ -s "$PRODUCT_OUT/system_ext/etc/selinux/system_ext_sepolicy.cil" ]] || \
  die "system_ext is missing its SELinux policy"
grep -Fq 'leaf3_epdc_bridge' \
  "$PRODUCT_OUT/system_ext/etc/selinux/system_ext_sepolicy.cil" || \
  die "system_ext policy is missing the Leaf3 EPDC bridge domain"
grep -Fq 'leaf3_wakeup_sysfs' \
  "$PRODUCT_OUT/system_ext/etc/selinux/system_ext_sepolicy.cil" || \
  die "system_ext policy is missing the stock-driver wakeup labels"
grep -Fq 'write /sys/power/autosleep off' \
  "$PRODUCT_OUT/system_ext/etc/init/leaf3_epdc_bridge.rc" || \
  die "system_ext init service is missing the ONYX PM wake repair"
grep -Fq 'leaf3_epdc_bridge_exec' \
  "$PRODUCT_OUT/system_ext/etc/selinux/system_ext_file_contexts" || \
  die "system_ext file contexts do not label the Leaf3 EPDC bridge"
grep -Fq 'leaf3_config_prop' \
  "$PRODUCT_OUT/system_ext/etc/selinux/system_ext_property_contexts" || \
  die "system_ext property contexts do not label the Leaf3 controls"
[[ "$(stat -c %s "$PRODUCT_OUT/boot.img")" -le 100663296 ]] || \
  die "boot.img exceeds the 96 MiB partition size"
[[ "$(stat -c %s "$PRODUCT_OUT/dtbo.img")" -le 25165824 ]] || \
  die "dtbo.img exceeds the 24 MiB partition size"
[[ -x "$AVBTOOL" ]] || die "missing host avbtool: $AVBTOOL"
"$AVBTOOL" info_image --image "$PRODUCT_OUT/dtbo.img" > "$BUILT_DTBO_INFO"
"$AVBTOOL" info_image --image "$TARGET_DEVICE_DIR/prebuilt/dtbo.img" \
  > "$STOCK_DTBO_INFO"
grep -Eq '^[[:space:]]*Partition Name:[[:space:]]+dtbo$' \
  "$BUILT_DTBO_INFO" || \
  die "built dtbo.img has no AVB hash descriptor for the dtbo partition"
built_dtbo_payload_size="$(
  awk '/^Original image size:/ { print $4; exit }' "$BUILT_DTBO_INFO"
)"
stock_dtbo_payload_size="$(
  awk '/^Original image size:/ { print $4; exit }' "$STOCK_DTBO_INFO"
)"
[[ "$built_dtbo_payload_size" =~ ^[1-9][0-9]*$ ]] || \
  die "could not determine the built DTBO payload size"
[[ "$built_dtbo_payload_size" == "$stock_dtbo_payload_size" ]] || \
  die "built and stock DTBO payload sizes differ"
cmp -n "$built_dtbo_payload_size" \
  "$PRODUCT_OUT/dtbo.img" "$TARGET_DEVICE_DIR/prebuilt/dtbo.img" || \
  die "built DTBO payload differs from the checksum-pinned stock image"

rm -rf -- "$VERIFY_DIR"
mkdir -p "$VERIFY_DIR"
python3 "$SOURCE_DIR/system/tools/mkbootimg/unpack_bootimg.py" \
  --boot_img "$PRODUCT_OUT/boot.img" \
  --out "$VERIFY_DIR" > "$VERIFY_BOOT_INFO"
if grep -Fq 'androidboot.selinux=permissive' "$VERIFY_BOOT_INFO"; then
  die "boot.img still forces SELinux permissive mode"
fi
cmp "$VERIFY_DIR/kernel" "$TARGET_DEVICE_DIR/prebuilt/kernel"
cmp "$VERIFY_DIR/dtb" "$TARGET_DEVICE_DIR/prebuilt/dtb/leaf3.dtb"
gzip -dc "$VERIFY_DIR/ramdisk" | cpio -t > "$RAMDISK_LIST"
grep -Fxq 'fstab.emmc' "$RAMDISK_LIST" || die "boot ramdisk is missing fstab.emmc"
grep -Fxq 'waveform/eink_waveform.wbf' "$RAMDISK_LIST" || \
  die "boot ramdisk is missing the e-ink waveform"
grep -Fxq 'adb_keys' "$RAMDISK_LIST" || \
  die "boot ramdisk is missing the configured debug ADB key"
readonly VERIFY_RAMDISK_DIR="$VERIFY_DIR/ramdisk-root"
mkdir -p "$VERIFY_RAMDISK_DIR"
(
  cd "$VERIFY_RAMDISK_DIR"
  gzip -dc "$VERIFY_DIR/ramdisk" | cpio -idm fstab.emmc adb_keys 2>/dev/null
)
[[ -f "$VERIFY_RAMDISK_DIR/fstab.emmc" && ! -L "$VERIFY_RAMDISK_DIR/fstab.emmc" ]] || \
  die "boot ramdisk fstab.emmc is missing or is not a regular file"
cmp "$VERIFY_RAMDISK_DIR/fstab.emmc" "$TARGET_DEVICE_DIR/rootdir/etc/fstab.emmc"
cmp "$VERIFY_RAMDISK_DIR/adb_keys" "$TARGET_DEVICE_DIR/debug-adb-key.pub"

(
  cd "$PRODUCT_OUT"
  sha256sum "${OUTPUT_FILES[@]}" > lineage-leaf3-images.sha256sum
  sha256sum "$(basename "$OTA_PACKAGE")" >> lineage-leaf3-images.sha256sum
)

ccache --show-stats
log "Build complete"
echo "Images: $PRODUCT_OUT"
echo "OTA package: $OTA_PACKAGE"
echo "Checksums: $PRODUCT_OUT/lineage-leaf3-images.sha256sum"
