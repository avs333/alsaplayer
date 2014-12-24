LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := ape

LOCAL_SRC_FILES +=  predictor.c decoder.c predictor-arm.S entropy.c parser.c filter-pre.c main.c
#	filter_1280_15.c filter_16_11.c filter_256_13.c filter_32_10.c filter_64_11.c main.c

LOCAL_CFLAGS += -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp -O3 -Wall -DBUILD_STANDALONE -DCPU_ARM \
 -fPIC -DARM_ARCH=7 -UDEBUG -DNDEBUG -fomit-frame-pointer -ffreestanding -Iinclude -I.

#LOCAL_CFLAGS += -O3 -Wall -DBUILD_STANDALONE -DCPU_ARM \
# -fPIC -DARM_ARCH=5 -UDEBUG -DNDEBUG -fomit-frame-pointer -ffreestanding -Iinclude 

#LOCAL_ARM_MODE := arm

include $(BUILD_STATIC_LIBRARY)

