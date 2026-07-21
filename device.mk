LOCAL_PATH := device/onyx/leaf3

PRODUCT_SHIPPING_API_LEVEL := 30
PRODUCT_TARGET_VNDK_VERSION := 30
PRODUCT_USE_DYNAMIC_PARTITIONS := true
PRODUCT_VIRTUAL_AB_OTA := true

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

# The stock boot chain selects fstab.emmc. Put the test fstab and the stock
# e-ink waveform in the boot ramdisk so they are available before first-stage
# init mounts dynamic partitions.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/etc/fstab.emmc:root/fstab.emmc \
    $(LOCAL_PATH)/prebuilt/eink_waveform.wbf:root/waveform/eink_waveform.wbf

PRODUCT_PACKAGES += \
    fastbootd

PRODUCT_SYSTEM_DEFAULT_PROPERTIES += \
    ro.board.platform=bengal \
    ro.virtual_ab.enabled=true
