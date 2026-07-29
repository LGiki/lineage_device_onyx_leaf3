LOCAL_PATH := device/onyx/leaf3

# Android.bp declares an explicit namespace. Export it to Make so modules in
# this device tree can be resolved from PRODUCT_PACKAGES.
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

PRODUCT_SHIPPING_API_LEVEL := 30
PRODUCT_TARGET_VNDK_VERSION := 30
PRODUCT_USE_DYNAMIC_PARTITIONS := true
PRODUCT_VIRTUAL_AB_OTA := true

DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay

# First-boot images deliberately use the complete stock vendor partition on
# the device. Rebuilding vendor from a broad blob dump creates duplicate
# platform targets and loses stock ownership, capabilities and SELinux data.
PRODUCT_BUILD_VENDOR_IMAGE := false

AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += \
    boot \
    dtbo \
    product \
    system \
    system_ext \
    vbmeta

# The stock boot chain selects fstab.emmc. Android 11 has a dedicated ramdisk
# output directory; using root/ here installs into the product root instead of
# boot.img and leaves first-stage init without an fstab.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/app/privapp-permissions-org.lineageos.leaf3controls.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/permissions/privapp-permissions-org.lineageos.leaf3controls.xml \
    $(LOCAL_PATH)/rootdir/etc/fstab.emmc:$(TARGET_COPY_OUT_RAMDISK)/fstab.emmc \
    $(LOCAL_PATH)/prebuilt/eink_waveform.wbf:$(TARGET_COPY_OUT_RAMDISK)/waveform/eink_waveform.wbf

# PRODUCT_ADB_KEYS is installed into TARGET_ROOT_OUT, which is not packaged in
# this device's dedicated Android 11 boot ramdisk. During bring-up, explicitly
# copy an optional ignored public key to /adb_keys in boot.img.
ifneq ($(wildcard $(LOCAL_PATH)/debug-adb-key.pub),)
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/debug-adb-key.pub:$(TARGET_COPY_OUT_RAMDISK)/adb_keys
endif

PRODUCT_PACKAGES += \
    fastbootd \
    Leaf3Controls \
    leaf3_epdc_bridge \
    leaf3_frontlight \
    leaf3_refresh

PRODUCT_SYSTEM_DEFAULT_PROPERTIES += \
    ro.board.platform=bengal \
    ro.virtual_ab.enabled=true \
    persist.sys.leaf3.epdc_backend=bridge \
    persist.sys.leaf3.capture_mode=notify \
    persist.sys.leaf3.page_interval=10 \
    persist.sys.leaf3.settle_quality=1 \
    persist.sys.leaf3.contrast=0 \
    persist.sys.leaf3.gamma=100 \
    persist.sys.leaf3.dither=1
    persist.sys.leaf3.nav_refresh_button=0
