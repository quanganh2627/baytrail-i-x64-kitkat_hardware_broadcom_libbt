LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := bt_vendor.conf
LOCAL_MODULE_TAGS := optional
LOCAL_REQUIRED_MODULES := \
	bt_baylake.conf \
	bt_byt_m_crb.conf \
	bt_byt_t_ffrd8.conf \
	bt_ctp7160.conf \
	bt_redhookbay.conf \
	bt_saltbay.conf

include $(BUILD_PHONY_PACKAGE)

include $(call all-makefiles-under,$(LOCAL_PATH))
