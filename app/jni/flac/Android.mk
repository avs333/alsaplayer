
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := flac

LOCAL_SRC_FILES += decoder.c main.c
LOCAL_CFLAGS += -O3 -Wall -DBUILD_STANDALONE -finline-functions -fPIC -I. -Iinclude

ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
LOCAL_CFLAGS += -DCPU_ARM
LOCAL_SRC_FILES += arm.S
endif

#-DDBG_TIME
#LOCAL_ARM_MODE := arm

include $(BUILD_STATIC_LIBRARY)
# include $(BUILD_SHARED_LIBRARY)

