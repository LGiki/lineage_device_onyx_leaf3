# LineageOS 18.1 device bring-up: ONYX BOOX Leaf3

This is an Android 11 / LineageOS 18.1 first-boot device tree for the BOOX
Leaf3 (Snapdragon 662, `bengal`).  It uses the supplied stock Android 11
kernel and vendor partition as bring-up inputs.  It is not suitable for an
official build until the kernel source and SELinux policy are completed.

## Source layout

Place this directory at `device/onyx/leaf3` in a LineageOS 18.1 checkout. The
provided stock images should be available as `../stock-images` while preparing
the tree, or pass their explicit paths to the scripts below.

## Prepare proprietary inputs

On a Linux host, install `e2fsprogs`, then run:

```sh
device/onyx/leaf3/prepare-kernel.sh /path/to/stock-images/boot.img
device/onyx/leaf3/extract-files.sh /path/to/stock-images/vendor.img
```

`extract-files.sh` produces the untracked `vendor/onyx/leaf3` tree. Do not
commit or redistribute those proprietary files.

## Build

```sh
source build/envsetup.sh
lunch lineage_leaf3-userdebug
mka bacon
```

## GitHub Actions

Push this directory as its own Git repository and run **Build LineageOS 18.1**
from the Actions tab. The workflow downloads the checksum-pinned stock OTA,
extracts only the required partitions on the runner, and uploads any build
outputs as an artifact. No stock image or blob is committed to the repository.

The first build should be treated as a boot test only. Flash only from a
verified recovery/fastboot workflow, preserve the complete stock firmware,
and collect `adb logcat`, `dmesg`, and `getprop` before changing any partition
layout. The stock kernel is a temporary bring-up mechanism; a maintained
device port requires a matching GPL kernel source tree and enforcing SELinux.
