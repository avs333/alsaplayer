
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := flac

LOCAL_SRC_FILES += decoder.c shndec.c arm.S main.c
LOCAL_CFLAGS += -O3 -Wall -DBUILD_STANDALONE -DCPU_ARM -finline-functions -fPIC -march=armv7-a -mthumb -I. -Iinclude
#-DDBG_TIME
#LOCAL_ARM_MODE := arm

include $(BUILD_STATIC_LIBRARY)
# include $(BUILD_SHARED_LIBRARY)

