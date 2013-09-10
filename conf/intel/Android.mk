LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := bt_vendor.conf
LOCAL_MODULE_TAGS := optional
ifeq (, $(filter %_next, $(TARGET_PRODUCT)))
LOCAL_REQUIRED_MODULES := \
	bt_baylake.conf \
	bt_byt_m_crb.conf \
	bt_byt_t_ffrd10.conf \
	bt_byt_t_ffrd8.conf \
	bt_bodegabay.conf \
	bt_ctp7160.conf \
	bt_ctpscalelt.conf \
	bt_redhookbay.conf \
	bt_saltbay_pr0.conf \
	bt_saltbay_pr1.conf \
	bt_victoriabay.conf
else
LOCAL_REQUIRED_MODULES := \
	bt_baylake_next.conf \
	bt_byt_m_crb_next.conf \
	bt_byt_t_ffrd10_next.conf \
	bt_byt_t_ffrd8_next.conf \
	bt_bodegabay_next.conf \
	bt_ctp7160_next.conf \
	bt_ctpscalelt_next.conf \
	bt_redhookbay_next.conf \
	bt_saltbay_pr0_next.conf \
	bt_saltbay_pr1_next.conf \
	bt_victoriabay_next.conf
endif

include $(BUILD_PHONY_PACKAGE)

include $(call all-makefiles-under,$(LOCAL_PATH))
