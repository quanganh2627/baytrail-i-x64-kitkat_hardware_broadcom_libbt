LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := bt_$(notdir $(LOCAL_PATH)).conf
$(call print-vars $(LOCAL_MODULE))
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/bluetooth
LOCAL_SRC_FILES := bt_vendor.conf
include $(BUILD_PREBUILT)
