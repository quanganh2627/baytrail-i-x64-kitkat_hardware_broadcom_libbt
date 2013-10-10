LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := bt_vendor.conf
LOCAL_MODULE_TAGS := optional
ifeq (, $(filter %_next, $(TARGET_PRODUCT)))
LOCAL_REQUIRED_MODULES := \
	bt_baylake.conf \
	bt_baylake_edk2.conf \
	bt_byt_m_crb.conf \
	bt_byt_t_ffrd8.conf \
	bt_ctp7160.conf \
	bt_redhookbay.conf \
	bt_saltbay.conf
else
LOCAL_REQUIRED_MODULES := \
	bt_baylake_next.conf \
	bt_byt_m_crb_next.conf \
	bt_byt_t_ffrd8_next.conf \
	bt_ctp7160_next.conf \
	bt_redhookbay_next.conf \
	bt_saltbay_next.conf
endif

include $(BUILD_PHONY_PACKAGE)

include $(call all-makefiles-under,$(LOCAL_PATH))
