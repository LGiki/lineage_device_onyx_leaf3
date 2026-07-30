# LineageOS 18.1 for ONYX BOOX Leaf3

An experimental Android 11 / LineageOS 18.1 device tree for the BOOX Leaf3 (`bengal`). It reuses the stock Android 11 kernel, DTB, DTBO, e-ink waveform, and vendor partition.

This is an experimental ROM that is suitable for daily use, but it is not an official LineageOS release. The build creates flashable images and a recovery-sideloadable A/B OTA; it does not create or replace `vendor.img`.

## Status

### Working

- Builds flashable images and a recovery-sideloadable A/B OTA while preserving the stock vendor partition.
- Uses the default screenshot-to-EBC display bridge with damage-aware updates, refresh modes, and periodic-capture fallback.
- Provides frontlight brightness and color-temperature control through the command-line tools and platform-signed Leaf3 Controls app.

### Not ready or unavailable

- The stock vendor partition is required and is neither built nor replaced by this tree.
- The composer-native E-Ink backend is experimental and disabled by default.

## Build

Build on an x86_64 Arch, Debian/Ubuntu, or Red Hat-family host with at least 200 GiB free disk space (250–300 GiB recommended), 32 GiB RAM recommended, and a non-root user with `sudo` access. `--install-deps` detects the distro from `/etc/os-release` and uses `pacman`, `apt-get`, or `dnf`/`yum` respectively. Arch builders must enable multilib; Red Hat-family builders may need EPEL and their distro's development repository enabled.

LineageOS 18.1 requires JDK 11 and its legacy RenderScript tool needs the `libtinfo.so.5` ABI. The build script automatically uses an installed JDK 11 for its own process, so it does not require changing the system-wide Java alternative. On Arch, install the AUR package `ncurses5-compat-libs`; on Debian/Ubuntu, use a release that provides the `libtinfo5` compatibility package; and on Red Hat-family systems install `ncurses-compat-libs`. The script checks both requirements before syncing or building.

```sh
git clone https://github.com/LGiki/lineage_device_onyx_leaf3.git
cd lineage_device_onyx_leaf3
./build-lineage.sh \
  --source-dir /srv/android/lineage-18.1 \
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
  --install-deps \
  --jobs 12
```

Pass the public `adbkey.pub` from the debugging computer; never copy the private ADB key to the build server. Subsequent builds can use `--skip-sync`. Run `./build-lineage.sh --help` for all options, including proxy and download controls.

Development builds use Android's public test keys and LineageOS Trust will
report that fact. For a privately signed build, generate and securely store the
standard `releasekey`, `platform`, `shared`, `media`, and `networkstack`
`.pk8`/`.x509.pem` pairs outside this repository, then pass
`--release-keys-dir /secure/path/to/keys`. The helper re-signs the target-files
archive, builds the OTA from it, and exports the matching signed partition
images. Back up the keys permanently: future OTAs must use the same keys, and
changing platform keys requires a factory reset.

## Installation

> [!CAUTION]
> Flashing experimental images can erase data or leave the device unbootable. Proceed only with an unlocked bootloader, a known-working TWRP/EDL recovery path, and verified backups of the complete stock firmware and both slots.

Install the generated OTA with the compatible [Leaf3 TWRP recovery](https://github.com/LGiki/twrp_device_onyx_leaf3), using **Advanced → ADB Sideload**. It updates the inactive A/B slot and preserves the stock vendor partition. Do not flash or erase `vendor`, `super`, `recovery`, `vbmeta_system`, `persist`, or `onyxconfig`.

```sh
cd /path/to/out/target/product/leaf3
sha256sum -c lineage-leaf3-images.sha256sum
adb sideload lineage-18.1-*-UNOFFICIAL-leaf3.zip
```

## Important device warning

Keep the stock vendor partition intact: it contains the matching Qualcomm HALs, firmware configuration, ONYX init files, and vendor SELinux policy. Treat all outputs as development images and retain matching stock images for recovery. A maintained release still needs audited policy, release keys, verified boot, and a matching GPL kernel source tree.
