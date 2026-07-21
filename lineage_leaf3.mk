$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)
$(call inherit-product, device/onyx/leaf3/device.mk)

PRODUCT_DEVICE := leaf3
PRODUCT_NAME := lineage_leaf3
PRODUCT_BRAND := ONYX
PRODUCT_MODEL := BOOX Leaf3
PRODUCT_MANUFACTURER := ONYX

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRIVATE_BUILD_DESC="BOOX-userdebug 11 RKQ1.210614.002 200 release-keys"

BUILD_FINGERPRINT := ONYX/BOOX/BOOX:11/RKQ1.210614.002/200:userdebug/release-keys
