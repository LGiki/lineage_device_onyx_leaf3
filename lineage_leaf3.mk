$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)
$(call inherit-product, device/onyx/leaf3/device.mk)

# Include the Lineage framework resources and the compact tablet package set.
# The mini profile is intentional: Leaf3's system partition is only 877 MiB.
# Without a Lineage common profile, framework-res references
# org.lineageos.platform-res.apk but the APK is not installed, so zygote exits.
$(call inherit-product, vendor/lineage/config/common_mini_tablet_wifionly.mk)

PRODUCT_DEVICE := leaf3
PRODUCT_NAME := lineage_leaf3
PRODUCT_BRAND := ONYX
PRODUCT_MODEL := BOOX Leaf3
PRODUCT_MANUFACTURER := ONYX

PRODUCT_BUILD_PROP_OVERRIDES += \
    PRIVATE_BUILD_DESC="BOOX-userdebug 11 RKQ1.210614.002 200 release-keys"

BUILD_FINGERPRINT := ONYX/BOOX/BOOX:11/RKQ1.210614.002/200:userdebug/release-keys
