LOCAL_PATH := device/onyx/leaf3

PRODUCT_SHIPPING_API_LEVEL := 30
PRODUCT_TARGET_VNDK_VERSION := 30
PRODUCT_USE_DYNAMIC_PARTITIONS := true
PRODUCT_VIRTUAL_AB_OTA := true
PRODUCT_FULL_TREBLE_OVERRIDE := true

AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += \
    boot \
    dtbo \
    product \
    system \
    system_ext \
    vbmeta \
    vbmeta_system \
    vendor

# First-stage mount configuration.  The generated vendor image receives the
# rest of the stock vendor files through vendor/onyx/leaf3.
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/rootdir/etc/fstab.qcom:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.qcom \
    $(LOCAL_PATH)/rootdir/etc/init.onyx.leaf3.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.onyx.leaf3.rc

PRODUCT_PACKAGES += \
    android.hardware.health@2.1-service \
    android.hardware.health@2.1-impl \
    android.hardware.health.storage@1.0-impl \
    fastbootd

PRODUCT_PROPERTY_OVERRIDES += \
    ro.board.platform=bengal \
    ro.product.device=leaf3 \
    ro.product.model=BOOX Leaf3 \
    ro.product.manufacturer=ONYX \
    ro.virtual_ab.enabled=true

$(call inherit-product-if-exists, vendor/onyx/leaf3/leaf3-vendor.mk)
