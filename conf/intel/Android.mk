LOCAL_PATH:= $(call my-dir)

# find all BT config files
ALL_BT_CONFIG := $(subst /,,$(dir $(subst $(LOCAL_PATH)/,,$(wildcard $(LOCAL_PATH)/*/bt_vendor.conf))))

# $1 is the product_name
# $2 is the source path
define bt_copy_conf
include $(CLEAR_VARS)

LOCAL_MODULE := bt_$(1).conf
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)/bluetooth
LOCAL_SRC_FILES := $(2)/bt_vendor.conf
include $(BUILD_PREBUILT)

ALL_BT_CONFIG_MODULE += bt_$(1).conf
endef

# copy all product bt_vendor.conf to /etc/bluetooth/bt_$(TARGET_PRODUCT).conf
$(foreach bt_conf,$(ALL_BT_CONFIG),$(eval $(call bt_copy_conf,$(bt_conf),$(bt_conf))))

# if REF_PRODUCT_NAME also copy $(REF_PRODUCT_NAME)/bt_vendor.conf
# to /etc/bluetootch/bt_$(TARGET_PRODUCT).conf
# useful for xxxx_next and xxxx_64 target
ifneq ($(TARGET_PRODUCT),$(REF_PRODUCT_NAME))
ifneq ($(wildcard $(LOCAL_PATH)/$(REF_PRODUCT_NAME)/bt_vendor.conf),)
ifeq ($(wildcard $(LOCAL_PATH)/$(TARGET_PRODUCT)/bt_vendor.conf),)
$(eval $(call bt_copy_conf,$(TARGET_PRODUCT),$(REF_PRODUCT_NAME)))
endif
endif
endif

include $(CLEAR_VARS)

LOCAL_MODULE := bt_vendor.conf
LOCAL_MODULE_TAGS := optional

LOCAL_REQUIRED_MODULES := $(ALL_BT_CONFIG_MODULE)

include $(BUILD_PHONY_PACKAGE)

