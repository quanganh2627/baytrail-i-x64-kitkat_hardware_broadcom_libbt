ifneq ($(BOARD_USES_WCS),true)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

BDROID_DIR := $(TOP_DIR)external/bluetooth/bluedroid

include $(TOP_DIR)device/intel/common/ComboChipVendor.mk

LOCAL_MODULE := libbt-vendor
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR_SHARED_LIBRARIES)

# BCM configuration
ifeq ($(COMBO_CHIP_VENDOR), bcm)
    LOCAL_C_INCLUDES := \
        $(BDROID_DIR)/hci/include \
        $(LOCAL_PATH)/include
    LOCAL_SRC_FILES := \
        src/bt_vendor_brcm.c \
        src/hardware.c \
        src/userial_vendor.c \
        src/upio.c \
        src/conf.c
    LOCAL_SHARED_LIBRARIES := libcutils
    LOCAL_MODULE_OWNER := broadcom
    include $(LOCAL_PATH)/vnd_buildcfg.mk
endif
# end of BCM configuration


# TI configuration
ifeq ($(COMBO_CHIP_VENDOR), ti)
    LOCAL_PATH := $(ANDROID_BUILD_TOP)
    TI_BT_VENDOR_PATH := hardware/ti/wpan/bluedroid_wilink
    LOCAL_C_INCLUDES := $(BDROID_DIR)/hci/include
    LOCAL_SRC_FILES := $(TI_BT_VENDOR_PATH)/libbt-vendor-ti.c
    LOCAL_SHARED_LIBRARIES := \
        libcutils \
        liblog \
        libnativehelper \
        libutils
    LOCAL_MODULE_OWNER := ti
endif
# end of TI configuration

include $(BUILD_SHARED_LIBRARY)

# LOCAL_PATH needs to be redefine in case TI configuration is used
LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_PRODUCT), full_maguro)
    include $(LOCAL_PATH)/conf/samsung/maguro/Android.mk
endif
ifeq ($(TARGET_PRODUCT), full_crespo)
    include $(LOCAL_PATH)/conf/samsung/crespo/Android.mk
endif
ifeq ($(TARGET_PRODUCT), full_crespo4g)
    include $(LOCAL_PATH)/conf/samsung/crespo4g/Android.mk
endif
ifeq ($(TARGET_PRODUCT), full_wingray)
    include $(LOCAL_PATH)/conf/moto/wingray/Android.mk
endif

endif # BOARD_USES_WCS != true
