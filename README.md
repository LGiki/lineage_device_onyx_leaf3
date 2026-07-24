# LineageOS 18.1 device bring-up: ONYX BOOX Leaf3

This repository contains an Android 11 / LineageOS 18.1 first-boot device tree
for the BOOX Leaf3 (Snapdragon 662, `bengal`). It uses the stock Android 11
kernel, DTB, DTBO, and e-ink waveform. The stock vendor partition is
deliberately preserved.

This is an experimental bring-up, not an official LineageOS port. The build
produces individual first-boot images; it does **not** produce a vendor image
or an installable OTA ZIP.

## Server requirements

Use a current x86_64 Arch Linux installation with:

- At least 200 GiB of free disk space; 250–300 GiB is recommended.
- 32 GiB RAM recommended. Add swap if the server has less memory.
- The Arch `multilib` repository enabled in `/etc/pacman.conf`.
- A normal, non-root build user with `sudo` access for dependency installation.

The script installs packages only when `--install-deps` is specified. One
legacy library must be installed separately from the AUR because LineageOS
18.1 includes a prebuilt RenderScript tool linked to `libtinfo.so.5`:

```sh
# Using an installed AUR helper:
yay -S ncurses5-compat-libs
```

Review AUR package build files before installing them. If no AUR helper is
installed, build
[`ncurses5-compat-libs`](https://aur.archlinux.org/packages/ncurses5-compat-libs)
using the standard
[Arch AUR procedure](https://wiki.archlinux.org/title/Arch_User_Repository).

## Build on Arch Linux

Clone this repository somewhere outside the large LineageOS checkout:

```sh
git clone https://github.com/LGiki/lineage_device_onyx_leaf3.git
cd lineage_device_onyx_leaf3
chmod +x build-lineage-arch.sh
```

Run the complete setup, source sync, stock-image preparation, build, and
verification:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --install-deps \
  --jobs 12
```

Do not run the build as root. The source directory must be writable by the
build user. The initial sync and build can take several hours.

The script:

1. Checks the Arch host, dependencies, legacy ncurses library, and free space.
2. Initializes and syncs the shallow LineageOS 18.1 source checkout.
3. Copies this device tree to `device/onyx/leaf3`.
4. Downloads the checksum-pinned BOOX Page 3.5 OTA with eight resumable aria2
   connections, then extracts only the stock boot, DTBO, and recovery inputs.
5. Prepares the kernel, DTB, DTBO, and panel waveform.
6. Builds and verifies the six first-boot images.

Completed images are placed in:

```text
/srv/android/lineage-18.1/out/target/product/leaf3/
├── boot.img
├── dtbo.img
├── product.img
├── system.img
├── system_ext.img
├── vbmeta.img
└── lineage-leaf3-images.sha256sum
```

Subsequent runs reuse the source checkout, downloaded stock OTA, extracted
stock images, and ccache. To build without contacting the source remotes:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --skip-sync \
  --jobs 12
```

## Proxy support

Use `--proxy` when one HTTP-compatible proxy should handle all source, stock
firmware, Git, curl, and Python package downloads:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --proxy http://127.0.0.1:7890 \
  --download-connections 8 \
  --sync-jobs 2 \
  --jobs 12
```

Use `--download-connections 1` to disable segmented stock ROM downloading, or
choose a value up to 16. The server can limit the number of connections
actually used. An interrupted download resumes on the next build. If aria2 is
unavailable, the stock preparation script falls back to resumable curl.

Authenticated proxy URLs are supported, but placing a password on the command
line may expose it in shell history. An environment variable avoids saving the
value in the command itself:

```sh
read -rsp 'Proxy URL: ' BUILD_PROXY
export BUILD_PROXY
./build-lineage-arch.sh --source-dir /srv/android/lineage-18.1 --jobs 12
unset BUILD_PROXY
```

Separate proxies and exclusions can also be configured:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --http-proxy http://proxy.example:3128 \
  --https-proxy http://proxy.example:3128 \
  --no-proxy localhost,127.0.0.1,.example.internal
```

The standard `http_proxy`, `https_proxy`, and `no_proxy` environment variables
are accepted as well. Command-line options take precedence. If sync is
unstable, reduce `--sync-jobs` and increase `--sync-retries`.

## Manual build from an existing checkout

Place this repository at `device/onyx/leaf3` in a LineageOS 18.1 checkout.
Prepare the stock inputs and build:

```sh
device/onyx/leaf3/prepare-stock-images.sh \
  device/onyx/leaf3/stock-images

device/onyx/leaf3/prepare-kernel.sh \
  device/onyx/leaf3/stock-images/boot.img \
  device/onyx/leaf3/stock-images/dtbo.img \
  device/onyx/leaf3/stock-images/recovery.img

source build/envsetup.sh
lunch lineage_leaf3-userdebug
mka bootimage dtboimage systemimage productimage systemextimage vbmetaimage
```

The generated files under `prebuilt/`, the stock images, and the stock OTA
cache are ignored by Git.

## Troubleshooting

### Invalid Chromium WebView APK

An error such as the following means the Git LFS-managed WebView APK was not
downloaded correctly:

```text
webview.apk: error: failed opening zip: Invalid file.
```

The build script now validates this APK before compiling. If it is invalid,
the script downloads the LFS object again, restores only `webview.apk`, and
checks the ZIP structure. Rerun the failed build without another full source
sync:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --skip-sync \
  --jobs 12
```

The existing proxy options and environment variables also apply to the Git LFS
repair. To diagnose the file manually:

```sh
cd /srv/android/lineage-18.1
git -C external/chromium-webview/prebuilt/arm64 lfs pull --include=webview.apk
git -C external/chromium-webview/prebuilt/arm64 checkout -- webview.apk
git -C external/chromium-webview/prebuilt/arm64 lfs checkout webview.apk
unzip -t external/chromium-webview/prebuilt/arm64/webview.apk
```

### `set_selinux_xattr` error for a custom root path

The device tree includes explicit SELinux file contexts for the stock
`/onyxconfig` mount point and the `/waveform` first-stage ramdisk directory. If
an older device-tree copy produced an error such as:

```text
set_selinux_xattr: No such file or directory searching for label "/onyxconfig"
set_selinux_xattr: No such file or directory searching for label "/waveform"
```

rerun the script with `--skip-sync`. It copies the updated device tree into the
LineageOS checkout and Ninja resumes the failed image target:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --skip-sync \
  --jobs 12
```

## Important device warning

The stock vendor partition must remain intact. It contains the matching
Qualcomm HALs, firmware configuration, ONYX init files, ownership/capability
metadata, and vendor SELinux policy.

The automated boot inputs come from the checksum-pinned BOOX Page 3.5 OTA used
by the working TWRP tree. Before testing against another Leaf3/Page firmware,
compare its boot, DTBO, and vendor versions and retain matching stock images
for immediate restoration.

Treat every output as a development image. Preserve the complete stock
firmware and both active-slot states. The test vbmeta disables verification,
and the kernel command line is permissive. A maintained port still requires a
matching GPL kernel source tree, complete device policy, and enforcing SELinux.

The optional `extract-files.sh` creates a complete stock-vendor inventory for
later vendor-source development. Its output is not inherited by this build and
is not a finished proprietary blob list.
