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
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
  --install-deps \
  --jobs 12
```

Do not run the build as root. The source directory must be writable by the
build user. The initial sync and build can take several hours.

`--adb-public-key` must point to the public `adbkey.pub` belonging to the
computer that will debug the device. For example, copy
`~/.android/adbkey.pub` from the macOS debugging computer to a protected
location on the Arch build server. Never copy or pass the private `adbkey`
file. The public key is embedded as `/adb_keys` in `boot.img`; the build stops
if the key is absent or does not match. Some system-as-root boot flows do not
retain that ramdisk path in Android's running root, so a clean-data install
can still show the standard authorization prompt.

ADB authentication remains enabled with `ro.adb.secure=1`. The matching
private key stays only on the debugging computer, and any unrecognized
computer remains unauthorized until its key is accepted on the device.

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
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
  --skip-sync \
  --jobs 12
```

## Proxy support

Use `--proxy` when one HTTP-compatible proxy should handle all source, stock
firmware, Git, curl, and Python package downloads:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
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
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
  --jobs 12
unset BUILD_PROXY
```

Separate proxies and exclusions can also be configured:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
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

## Flashing the experimental build

> [!CAUTION]
> This is an untested first-boot bring-up, not a finished ROM release. Flashing
> can erase all user data or leave the device unable to boot. Do not proceed
> without an unlocked bootloader, a known-working recovery/EDL path, and
> verified backups of the complete stock firmware and both slots.

Install a recent Android platform-tools package on the host:

```sh
sudo pacman -S android-tools
```

The build intentionally preserves the stock `vendor` partition and does not
produce an OTA ZIP. Never flash or erase `vendor`, `super`, `recovery`,
`vbmeta_system`, `persist`, or `onyxconfig` using these instructions.

The BOOX bootloader fastboot implementation may reject partition writes.
The procedure below does not depend on bootloader-fastboot flashing: it uses
TWRP fastbootd for dynamic logical partitions and TWRP's root ADB shell for
the physical boot-chain partitions. Fastbootd is a userspace service inside
recovery and is different from bootloader fastboot.

### 1. Verify the images and preserve stock partitions

```sh
ROM_DIR=/home/lgiki/leaf3-build/out/target/product/leaf3
cd "$ROM_DIR"
sha256sum -c lineage-leaf3-images.sha256sum
```

Before changing partitions, use the confirmed-working Leaf3/Page TWRP or an
EDL workflow to copy at least these stock partitions to another machine:

```text
boot_a, boot_b
dtbo_a, dtbo_b
vbmeta_a, vbmeta_b
vbmeta_system_a, vbmeta_system_b
recovery_a, recovery_b
super
persist
onyxconfig
```

Also back up personal files. Bootloader unlocking and the factory reset below
erase user data. The stock OTA cached by the build is not a substitute for a
backup of the exact firmware currently installed on the device.

### 2. Boot TWRP and choose a slot

Boot the confirmed-working Leaf3/Page TWRP, connect ADB, and inspect the active
slot and partition nodes:

```sh
adb devices
adb shell getprop ro.boot.slot_suffix
adb shell ls -l /dev/block/bootdevice/by-name/boot_a
adb shell ls -l /dev/block/bootdevice/by-name/boot_b
```

Stop if ADB is not running as root in TWRP, the slot suffix is missing, or any
required partition node is absent. TWRP does not bypass AVB signature
enforcement: the bootloader must already be unlocked or otherwise accept the
test vbmeta image.

Set the target explicitly after reading `ro.boot.slot_suffix`. Using the
current slot keeps the currently booted stock vendor, but overwrites that
slot's working boot and system partitions:

```sh
TARGET_SLOT=a  # Change to the slot you intentionally selected.
```

Using the inactive slot preserves a bootable fallback only if that slot has a
complete, matching stock vendor partition. Do not assume its vendor is valid
or matches the pinned boot inputs.

### 3. Flash logical partitions through TWRP fastbootd

In TWRP, select **Advanced → Enter Fastboot**, or run:

```sh
adb reboot fastboot
```

This switches recovery into fastbootd. Confirm that the host sees userspace
fastboot and that all target partitions are logical:

```sh
fastboot devices
fastboot getvar is-userspace
fastboot getvar "is-logical:system_${TARGET_SLOT}"
fastboot getvar "is-logical:product_${TARGET_SLOT}"
fastboot getvar "is-logical:system_ext_${TARGET_SLOT}"
```

All three `is-logical` checks must report `yes`. Flash only the generated
logical images:

```sh
fastboot --slot "$TARGET_SLOT" flash system "$ROM_DIR/system.img"
fastboot --slot "$TARGET_SLOT" flash product "$ROM_DIR/product.img"
fastboot --slot "$TARGET_SLOT" flash system_ext "$ROM_DIR/system_ext.img"
```

Do not use `--force`, delete logical partitions, resize partitions manually, or
flash the physical `super` partition. Stop and restore the stock backup if any
image does not fit.

### 4. Write the physical boot chain from TWRP

Return directly from fastbootd to TWRP recovery:

```sh
fastboot reboot recovery
adb wait-for-device
```

Copy the small physical images to TWRP's RAM-backed `/tmp`:

```sh
adb push "$ROM_DIR/boot.img" /tmp/boot.img
adb push "$ROM_DIR/dtbo.img" /tmp/dtbo.img
adb push "$ROM_DIR/vbmeta.img" /tmp/vbmeta.img
```

Before writing, verify that the selected block devices exist and are large
enough:

```sh
adb shell "blockdev --getsize64 /dev/block/bootdevice/by-name/boot_${TARGET_SLOT}"
adb shell "blockdev --getsize64 /dev/block/bootdevice/by-name/dtbo_${TARGET_SLOT}"
adb shell "blockdev --getsize64 /dev/block/bootdevice/by-name/vbmeta_${TARGET_SLOT}"
stat -c '%n %s' "$ROM_DIR/boot.img" "$ROM_DIR/dtbo.img" "$ROM_DIR/vbmeta.img"
```

Each block-device size must be greater than or equal to its image size. Write
only the explicitly selected slot:

```sh
adb shell "dd if=/tmp/boot.img of=/dev/block/bootdevice/by-name/boot_${TARGET_SLOT} bs=4096"
adb shell "dd if=/tmp/dtbo.img of=/dev/block/bootdevice/by-name/dtbo_${TARGET_SLOT} bs=4096"
adb shell "dd if=/tmp/vbmeta.img of=/dev/block/bootdevice/by-name/vbmeta_${TARGET_SLOT} bs=4096"
adb shell sync
```

Do not use `dd` for `system.img`, `product.img`, or `system_ext.img`; those are
sparse images for logical partitions and must be handled by fastbootd. The
generated `vbmeta.img` already has AVB verification disabled. Do not relock the
bootloader while any test image is installed.

### 5. Factory reset, activate the slot, and boot

Changing platform signing keys generally makes the stock encrypted data
incompatible. Re-enter TWRP fastbootd:

```sh
adb reboot fastboot
fastboot getvar is-userspace
```

The following command irreversibly formats user data and metadata:

```sh
fastboot -w
fastboot set_active "$TARGET_SLOT"
fastboot reboot
```

If `fastboot -w` is unsupported, return to TWRP, use **Wipe → Format Data**,
then re-enter fastbootd to run `fastboot set_active "$TARGET_SLOT"` and
`fastboot reboot`.

Allow extra time for the first boot. If the device reaches ADB, immediately
collect diagnostics:

```sh
adb wait-for-device
adb shell getprop > leaf3-getprop.txt
adb logcat -b all -d > leaf3-logcat.txt
adb shell dmesg > leaf3-dmesg.txt
```

### Roll back

If an untouched slot contains a complete matching stock installation, return
to TWRP fastbootd and activate it:

```sh
adb reboot fastboot
fastboot set_active a  # Replace with the known-good stock slot.
fastboot reboot
```

Otherwise, restore the exact saved `boot`, `dtbo`, `vbmeta`, and logical
partition data for the affected slot using TWRP/fastbootd or restore the full
stock firmware through the previously tested EDL procedure. Do not experiment
with slot switching after the bootloader marks slots unbootable; restore the
matching images and explicitly set the known-good slot.

## Troubleshooting

### Blank screen and `Can't find service: display`

`dumpsys display` queries the framework `DisplayManager` service, not the
vendor display HAL. If it prints:

```text
Can't find service: display
```

check whether Android's framework started:

```sh
adb shell getprop init.svc.zygote
adb shell getprop sys.boot_completed
adb logcat -b all -d |
  grep -E 'org\.lineageos\.platform-res|System zygote died|Service .zygote.'
```

The Leaf3 product must inherit
`vendor/lineage/config/common_mini_tablet_wifionly.mk`. The compact profile
is used because the system partition is only about 877 MiB. Without a Lineage
common configuration, `/system/framework/org.lineageos.platform-res.apk` is
omitted, zygote fails while creating the system `AssetManager`, and
`system_server` never publishes `display`. The build script now rejects an
output missing this APK.

After updating the device tree, rebuild with `--skip-sync`, flash the newly
built `system.img` and the other logical images in TWRP fastbootd, then flash
the regenerated boot-chain images from TWRP as described above. Do not reuse
the previous `system.img`.

### Android works in scrcpy but the E-Ink panel stays white

This is a separate failure from a missing framework display service. On the
stock kernel, connector `DSI-1` is an intentional dummy Qualcomm primary
display and `DSI-2` is the real ONYX EPDC panel. Standard Qualcomm HWC renders
Android to the dummy connector, which is why scrcpy works, but it does not
submit those frames through the private `/dev/ebc` update interface.

The stock ROM solves this with ONYX modifications embedded directly in
`framework.jar` and `services.jar` (`OECService` and `OnyxDeviceService`).
Those core stock jars are not binary-compatible replacements for LineageOS.
This device tree instead builds `leaf3_epdc_bridge`, a small native service
which captures the composed primary display and forwards changed rectangles
to `/dev/ebc`. It uses one full GC16 refresh at startup. After that it monitors
the Cypress `cyttsp5_mt` input device, wakes immediately for touch events, and
waits 32 ms for SurfaceFlinger to compose the resulting frame before capturing
it. Timer polling remains as a fallback for non-touch changes.

The bridge exposes the stock ONYX waveform strategies globally. The values
were recovered from the stock `ViewUpdateHelper` implementation rather than
guessed:

| Mode | Stock waveform | Intended use |
| --- | --- | --- |
| `balanced` | A2 while interacting, AUTO after settling | Default; responsive with automatic ghost cleanup |
| `normal` | AUTO | Highest general UI quality |
| `speed` | DU | Faster monochrome page and list changes |
| `a2` | ANIM/A2 | Fastest interaction, with more ghosting and less grayscale |
| `regal` | REGAL | Text-oriented partial updates with reduced ghosting |

Fast updates mark the panel as needing cleanup. After four unchanged capture
cycles (about 320–560 ms depending on mode), or 20 consecutive fast updates,
the bridge submits one
full-screen GC16 update. A partial AUTO update proved unable to remove retained
launcher and Settings frames after a long A2 scroll. The full cleanup flashes
more visibly, but restores a readable panel after interaction. This mirrors
the stock ROM's fast-mode plus periodic GC strategy without importing its
binary-incompatible framework.

The ONYX ioctl embeds an `mxcfb_rect`, whose binary field order is `top`,
`left`, `width`, `height`. An earlier bridge revision used `left`, `top`; full
updates appeared to work because both coordinates were zero, but scrolling
produced invalid requests such as `x=1537, width=1264`. The kernel rejected
those requests and the panel appeared frozen during list movement. The bridge
now uses the correct ABI order. Speed and A2 poll compositor changes every
80 ms while content remains active, Balanced uses 100 ms, and the slower
Normal and Regal quality modes use 140 ms.

After any touch event, the bridge keeps the active capture cadence for six
frames. Reader applications such as KOReader may publish a completed page
later than the initial 32 ms touch-settle delay; without this probe burst, a
missed first capture falls immediately back to the 500 ms idle interval.
Balanced treats a frame found during the burst as interaction, displays it
quickly with A2, then performs the usual GC16 cleanup after the page settles.

For partial updates, unchanged rows are rejected with an optimized memory
comparison. Only the changed bounding rectangle is copied into the persistent
EBC framebuffer and previous-frame cache. Earlier revisions copied the entire
8 MiB screen into both buffers after every change, even when only a button or
status icon changed. The persistent buffer still contains a complete current
frame, so periodic full GC16 cleanup remains correct without another
full-screen copy.

Rebuild the images. The refresh/frontlight bridge changes `system.img`, the
Settings shortcut fix changes `system_ext.img`, and the build regenerates
`vbmeta.img` for their new hashes. `boot.img`, `dtbo.img`, and `product.img`
are unchanged by these fixes:

```sh
./build-lineage-arch.sh \
  --source-dir /srv/android/lineage-18.1 \
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
  --skip-sync \
  --jobs 12
```

After boot completes, verify the bridge:

```sh
adb wait-for-device
adb shell getprop init.svc.leaf3-epdc-bridge
adb shell ps -AZ | grep leaf3_epdc_bridge
adb logcat -b all -d -s leaf3_epdc_bridge
adb shell ls -lZ /dev/ebc
adb shell leaf3-refresh status
```

The service property should be `running` and the log should contain:

```text
mapped EBC buffer for 1264x1680
touch wake-up enabled on /dev/input/event3 (cyttsp5_mt)
```

Change the global strategy without rebooting:

```sh
adb shell leaf3-refresh balanced
adb shell leaf3-refresh normal
adb shell leaf3-refresh speed
adb shell leaf3-refresh a2
adb shell leaf3-refresh regal
```

The selection persists across reboots. Request a one-time full GC16 cleanup
after visible ghosting with:

```sh
adb shell leaf3-refresh full
```

These are currently global modes. Stock ONYX firmware can select modes per
application because it modifies SurfaceFlinger, `framework.jar`, and
`services.jar`. Adding a per-app settings UI is possible later, but copying
those stock jars into LineageOS is not safe or ABI-compatible.

### The frontlight does not follow Android brightness

Leaf3 has a frontlight behind the display bezel, rather than an LCD
backlight. The stock kernel exposes its two controls as:

```text
/sys/class/backlight/onyx_bl_br/brightness  # total brightness, 0-28
/sys/class/backlight/onyx_bl_ct/brightness  # color temperature, 0-24
```

The preserved Qualcomm light HAL only knows about the dummy DSI display, so
changing Android's brightness does not directly update either ONYX control.
The platform-signed Leaf3 Controls service observes Android's 0-255 brightness
and interactive state and publishes them through the labeled
`sys.leaf3.android_brightness` and `sys.leaf3.interactive` properties. This
avoids reading the dummy panel's vendor-labeled sysfs nodes from the
`system_ext` bridge, which would cross Android's Treble SELinux boundary.

`leaf3_epdc_bridge` maps the relayed brightness to the real ONYX brightness
control. Android's visible slider is gamma-corrected before the light HAL
receives its 10-255 value, so the bridge first applies the inverse of Android
11's standard `BrightnessUtils` curve. It then spreads the result across all
28 ONYX steps. A direct linear conversion makes almost every visible change
occur in the last 10% of the slider.

The relay also reports when Android becomes non-interactive, so the frontlight
turns off and restores with the screen. While Android is asleep, the bridge
stops SurfaceFlinger capture and pixel comparison entirely and polls only its
cheap input/property state. It resumes with one full GC16 refresh after wake.
This avoids spending CPU and battery taking two full-screen screenshots per
second while the device is asleep. The ordinary Settings and Quick Settings
brightness slider remains the primary control.

The stock Cypress driver powers `cyttsp5_mt` off while entering deep suspend,
but on this userspace it can miss its matching power-on callback. The input
device then remains registered and enabled while its IRQ count stops changing
and it emits no events. A hardware reset alone cannot recover the unpowered
controller. On every real display off-to-on transition, the bridge closes its
old event handle and rebinds only the Cypress I2C device. This reruns the
driver's power initialization and probe. Because the bind operation can return
before Android publishes the replacement event node, the bridge retries
discovery for up to three seconds before falling back to timer polling. Access
is restricted to the driver's two labeled bind controls.

E-Ink retains its last image without power. By default the bridge replaces
that image with a white GC16 frame when Android turns the display off. Without
this step, KOReader's last page remains visible while Android is asleep, which
makes the navigation bar and notification shade appear frozen even though
input is correctly disabled. A second power-button press wakes Android and the
bridge then displays the current Android frame with a full refresh. Retaining
the last page is still available as an explicit preference:

```sh
adb shell leaf3-refresh clear-on-sleep
adb shell leaf3-refresh retain-on-sleep
```

After rebuilding and flashing the new `system.img` and matching `vbmeta.img`,
verify the mapping:

```sh
adb shell settings put system screen_brightness 128
sleep 1
adb shell leaf3-frontlight status
```

Because `128` is a linear light-HAL value, it corresponds to a high position
on Android's gamma-space slider and should produce approximately step 24 of
28. The helper accepts hardware percentages when a precise ADB control is
more convenient:

```sh
adb shell leaf3-frontlight brightness 35
adb shell leaf3-frontlight off
adb shell leaf3-frontlight on
adb shell leaf3-frontlight auto
```

Android clamps the brightness setting to its configured minimum while the
screen is awake. The helper therefore uses a separate persistent on/off flag
instead of relying on a slider value of zero. `brightness` installs a manual
hardware-percentage override; `auto` removes it and returns control to the
Android slider.

Color temperature is independent of brightness. Zero is coolest and 100 is
warmest:

```sh
adb shell leaf3-frontlight cool
adb shell leaf3-frontlight temperature 50
adb shell leaf3-frontlight warm
adb shell leaf3-frontlight status
```

The temperature selection persists across reboots. This first implementation
also includes the preinstalled **Leaf3 Controls** app because standard
LineageOS 18.1 exposes only one brightness slider.

### Leaf3 Controls app

The launcher contains a platform-signed system app named **Leaf3 Controls**.
It provides:

- Balanced, Normal, Speed, A2, and Regal refresh modes.
- A one-tap full GC16 screen cleanup.
- Frontlight on/off.
- A switch to follow Android's standard brightness slider.
- A manual frontlight percentage override.
- Cool-to-warm color-temperature control.
- A switch to disable Android window, transition, and animator effects.
- A switch to clear the retained application frame while sleeping.

The controls use the same persistent properties as `leaf3-refresh` and
`leaf3-frontlight`, so changes made in the app are visible to the command-line
tools and survive a reboot. Manual brightness disables Android-slider
following until **Follow Android brightness slider** is enabled again.
Window and transition animations default to off for new users because
intermediate LCD animation frames waste CPU and create E-Ink ghosting. The app
switch also controls animator effects and can restore all three Android scales.

The app is installed in `system_ext.img`. After rebuilding, flash the new
`system.img`, `system_ext.img`, and matching `vbmeta.img`. Confirm installation
and launch it directly if the launcher has not refreshed its app list:

```sh
adb shell pm path org.lineageos.leaf3controls
adb shell am start -n \
  org.lineageos.leaf3controls/.MainActivity
```

To inspect the values applied by the app:

```sh
adb shell leaf3-refresh status
adb shell leaf3-frontlight status
```

### Navigation buttons are missing

The generic framework resource defaults `config_showNavigationBar` to false
for this product even though `config_navBarInteractionMode` is already `0`
(three-button navigation). As a result, SystemUI never creates a navigation
bar window. The Leaf3 framework overlay now enables the software navigation
bar because the device has no reliable hardware navigation keys.

After flashing the rebuilt `system.img`, verify both values and the window:

```sh
adb shell cmd overlay lookup android android:bool/config_showNavigationBar
adb shell settings get secure navigation_mode
adb shell dumpsys SurfaceFlinger --list | grep NavigationBar
```

The expected resource value is `true`, the interaction mode is `0`, and at
least one `NavigationBar` layer should be listed.

### KOReader remains visible after pressing power

The E-Ink panel retaining KOReader's page does not mean Android is still
awake. While `dumpsys power` reports `mWakefulness=Asleep`, Android disables
touch dispatch and hides the navigation and status bars. Press power again to
wake the device; touch alone is not configured as a system wake source.

Android's framework default enables Keyguard, and this tree clears the retained
panel frame on sleep. A device preserving stock or older ROM user data can
still carry a per-user locksettings override in `/data`. Fix that once without
erasing user data:

```sh
adb shell locksettings set-disabled false
adb shell locksettings get-disabled
```

The final command must print `false`. After rebuilding, flash `system.img`,
`system_ext.img`, and their matching `vbmeta.img`. The framework overlay is in
`system.img`; the EPDC bridge and Leaf3 Controls switch are in
`system_ext.img`.

Android's default window and transition animation scales are both `1.0`.
Animations generate many intermediate frames and ghosting without adding much
value on E-Ink. Disable them for the current user with:

```sh
adb shell leaf3-refresh animations-off
```

Restore the standard Android behavior if needed:

```sh
adb shell leaf3-refresh animations-on
```

The bridge runs as UID `system` in the dedicated
`u:r:leaf3_epdc_bridge:s0` SELinux domain. Its private policy ships from
`system_ext`, so the preserved stock vendor partition does not need to be
modified. The policy grants only the interfaces used by the bridge:
SurfaceFlinger capture, graphics-buffer mapping, read-only touch monitoring,
the labeled `/dev/ebc` node, two explicitly labeled writable frontlight paths,
the Cypress driver's explicitly labeled bind and unbind controls, and its
system properties. Read-only labels cover the wakeup nodes exposed by the
preserved Qualcomm and ONYX drivers so Android's system-suspend service can
inspect them without gaining generic sysfs access. The Leaf3 Controls system
app publishes Android's interactive and brightness state through those
properties, avoiding access to the preserved vendor policy's generic
`vendor_sysfs_graphics` type.

The kernel command line no longer forces permissive mode. After flashing the
new `boot.img`, `system.img`, `system_ext.img`, and matching `vbmeta.img`,
verify enforcement, authenticated ADB, the service domain, and its device
label:

```sh
adb shell getenforce
adb shell getprop ro.adb.secure
adb shell getprop sys.leaf3.interactive
adb shell getprop sys.leaf3.android_brightness
adb shell ps -AZ | grep leaf3_epdc_bridge
adb shell ls -lZ /dev/ebc
adb shell ls -lZ /sys/bus/i2c/drivers/cyttsp5_i2c_adapter/{bind,unbind}
```

Expected values include `Enforcing`, `1`, a `0` or `1` interactive state, an
Android brightness in the `0`-`255` range,
`u:r:leaf3_epdc_bridge:s0`, and
`u:object_r:leaf3_epdc_device:s0`. Both driver controls should be labeled
`u:object_r:leaf3_touch_driver_sysfs:s0` and owned by `system:graphics`. After
one sleep/wake cycle, the bridge log should contain:

```text
touchscreen driver rebound after display wake
```

Collect new denials after exercising screen refresh, frontlight adjustment,
sleep/wake, USB, Wi-Fi, and KOReader:

```sh
adb logcat -b kernel -d |
  grep 'avc:.*denied' > leaf3-enforcing-avc.txt
```

If the service is `stopped`, collect these diagnostics before rebooting:

```sh
adb logcat -b all -d -s leaf3_epdc_bridge > leaf3-epdc-bridge.txt
adb shell dmesg |
  grep -Ei 'avc:|denied|ebc|epdc' > leaf3-epdc-kernel.txt
adb shell dumpsys SurfaceFlinger > leaf3-surfaceflinger.txt
```

This bridge is still a bring-up implementation. A production port should
integrate damage and E-Ink waveform selection directly with the compositor
instead of periodically capturing its output.

### Settings shortcut crashes SystemUI

If Settings opens when started with `adb shell am start -a
android.settings.SETTINGS` but tapping the Settings shortcut makes the status
bar disappear, inspect the crash buffer:

```sh
adb logcat -b crash -d
```

The LineageOS 18.1 failure fixed by this tree has this signature:

```text
Process: com.android.systemui
java.lang.RuntimeException: Can't create handler inside thread
at com.android.systemui.assist.AssistManager.<init>
at com.android.systemui.statusbar.phone.StatusBar.startActivityDismissingKeyguard
```

`StatusBar` lazily creates `AssistManager` from an `AsyncTask`, while that
Android 11 implementation constructs an unqualified `Handler`. The build
script applies `tools/patch-systemui-assist-handler.py`, pinning the handler to
the main looper. The patcher is strict and idempotent, so it fails instead of
silently changing an unexpected source revision.

This fix changes `SystemUI.apk` in `system_ext.img`. After rebuilding, flash
both the new `system.img` (refresh bridge) and `system_ext.img` (SystemUI fix)
through TWRP fastbootd. A direct ActivityManager launch is useful as a
temporary workaround on an older build:

```sh
adb shell am start -a android.settings.SETTINGS
```

### “There's an internal problem with your device”

Android displays this warning when its VINTF build-consistency check fails.
On the Leaf3, the stock ONYX 4.19 kernel has `CONFIG_PM_AUTOSLEEP=y`, while the
standard Android 11 framework compatibility matrix requires it to be disabled.
The boot log identifies the mismatch as:

```text
No compatible kernel requirement found (kernel FCM version = 5)
For config CONFIG_PM_AUTOSLEEP, value = y but required n
```

ONYX's stock level-5 matrix contains a device-specific exception requiring
`y` for the 4.19 kernel. The build script applies that same exception only to
the generated 4.19 matrix block and then rebuilds `system.img` and
`vbmeta.img`. It does not modify the shared `kernel/configs` source repository.

After flashing the regenerated `system.img`, verify that the warning is gone
and that no compatibility error was logged:

```sh
adb logcat -b all -d |
  grep -E 'Vendor interface is incompatible|CONFIG_PM_AUTOSLEEP'
```

### Boot returns directly to bootloader fastboot

If TWRP boots but the generated system immediately returns to bootloader
fastboot, verify that first-stage files are present in `boot.img`. Android 11
uses `$(TARGET_COPY_OUT_RAMDISK)` for device-specific boot-ramdisk files;
copying them to `root/` can produce a minimal ramdisk containing only `init`.

The build script extracts the completed boot image and refuses to finish unless
`fstab.emmc` is a regular file with exactly the device-tree contents and the
stock e-ink waveform is present. To inspect an image manually:

```sh
mkdir -p /tmp/leaf3-boot
python3 system/tools/mkbootimg/unpack_bootimg.py \
  --boot_img out/target/product/leaf3/boot.img \
  --out /tmp/leaf3-boot
gzip -dc /tmp/leaf3-boot/ramdisk | cpio -it |
  grep -E '^(fstab\.emmc|waveform/eink_waveform\.wbf)$'
```

Both paths must be printed. A zeroed first 32 bytes of `misc` excludes a stale
bootloader-control-block command; it does not make a ramdisk without an fstab
bootable.

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
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
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
  --adb-public-key /srv/keys/leaf3-adbkey.pub \
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
but SELinux and ADB authentication remain enabled. A maintained port still
requires a matching GPL kernel source tree, audited policy under broader
runtime testing, release keys, and verified boot.

The optional `extract-files.sh` creates a complete stock-vendor inventory for
later vendor-source development. Its output is not inherited by this build and
is not a finished proprietary blob list.
