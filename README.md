# LineageOS 18.1 device bring-up: ONYX BOOX Leaf3

This is an Android 11 / LineageOS 18.1 first-boot device tree for the BOOX
Leaf3 (Snapdragon 662, `bengal`). It uses the stock Android 11 kernel, DTB,
DTBO and e-ink waveform, and deliberately preserves the complete stock vendor
partition on the device. It is not suitable for an official build until the
kernel source, complete device integration and enforcing SELinux are finished.

## Source layout

Place this directory at `device/onyx/leaf3` in a LineageOS 18.1 checkout. Pass
explicit stock-image paths as shown below, or place locally prepared images in
the ignored `device/onyx/leaf3/stock-images` directory.

## Prepare stock boot inputs

On a Linux host, install `e2fsprogs`, `cpio`, and `gzip`, then run:

```sh
device/onyx/leaf3/prepare-kernel.sh \
  /path/to/stock-images/boot.img \
  /path/to/stock-images/dtbo.img \
  /path/to/stock-images/recovery.img
```

The recovery ramdisk supplies the panel waveform used before Android userspace
is available. The generated files under `prebuilt/` are ignored by Git.

## Build

```sh
source build/envsetup.sh
lunch lineage_leaf3-userdebug
mka bootimage dtboimage systemimage productimage systemextimage vbmetaimage
```

## GitHub Actions

Push this directory as its own Git repository and run **Build LineageOS 18.1**
from the Actions tab. The workflow downloads the checksum-pinned stock OTA,
extracts only boot, DTBO and recovery, and uploads the first-boot images as an
artifact. No stock image or blob is committed to the repository.

The workflow intentionally does not build a vendor image or an OTA zip. The
device's stock vendor partition must remain intact; it contains the matching
Qualcomm HALs, firmware configuration, ONYX init files, ownership/capability
metadata and vendor SELinux policy.

The automated boot inputs come from the checksum-pinned BOOX Page 3.5 OTA used
by the working TWRP tree. Before testing on a different Leaf3/Page firmware,
compare its boot, DTBO and vendor versions and keep the matching stock images
available for immediate restoration.

Treat every output as a development image. Preserve the complete stock
firmware and both active-slot states, and collect `adb logcat`, `dmesg` and
`getprop` before replacing more partitions. The test vbmeta has verification
disabled and the kernel command line is permissive. A maintained port requires
a matching GPL kernel source tree, complete device policy and enforcing
SELinux.

The optional `extract-files.sh` produces a complete stock-vendor inventory for
later vendor-source development. Its output is not inherited by this build
and must not be treated as a finished proprietary blob list.
