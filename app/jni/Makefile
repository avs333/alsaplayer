
# use "make android=1" to build executable for android
 
ifdef android
# Tested with Android NDK r10d toolchain. Tweak this if you have other compiler/NDK.
GCCVER=4.9
ANDPLAT=android-18
HOSTARCH=$(shell uname -m)
NDK ?= $(shell dirname $(shell readlink -e $(shell which ndk-build)))
SYSROOT=$(NDK)/platforms/$(ANDPLAT)/arch-arm
BINPATH=$(NDK)/toolchains/arm-linux-androideabi-$(GCCVER)/prebuilt/linux-$(HOSTARCH)/bin
STL=$(NDK)/sources/cxx-stl/gnu-libstdc++/$(GCCVER)
CC=$(BINPATH)/arm-linux-androideabi-gcc
CXX=$(BINPATH)/arm-linux-androideabi-g++
CFLAGS += --sysroot=$(SYSROOT) -Wl,-pie -march=armv7-a -DANDLINUX
CXXFLAGS += -I$(STL)/include -I$(STL)/libs/armeabi-v7a/include
LDFLAGS += -L$(STL)/libs/armeabi-v7a -lgnustl_static
LDFLAGS += -Llib -lselinux
.PHONY: nondk
ifeq ($(strip $(NDK)),)
nondk: ; $(error ndk-build not found in PATH, please try "NDK=/full/path/to/ndk make android=1")
endif
else
LDFLAGS += -lpthread
endif

SRC =	flac/shndec.c  flac/main.c  flac/decoder.c  buffer.c alsa.c alsa_offload.c main.c linux_main.c wav_main.c	\
	ape/entropy.c  ape/filter-pre.c  ape/parser.c   ape/decoder.c  ape/main.c  ape/predictor.c	

ifdef android
CFLAGS += -DCPU_ARM -DARM_ARCH=7 -mfpu=neon -mfloat-abi=softfp
SRC += flac/arm.S ape/predictor-arm.S
endif

SRCX =	tinyxml/tinyxml2.cpp  tinyxml/xmlparser.cpp

OBJ = $(SRC:.c=.o)
OBJX = $(SRCX:.cpp=.o)

CFLAGS += -g -O -Wall -DBUILD_STANDALONE -finline-functions -I. -Iinclude
CXXFLAGS += $(CFLAGS)

exe_file = alsaplayer

all = $(exe_file)

all: $(OBJ) $(OBJX)
	$(CXX) $(CXXFLAGS) $(OBJ) $(OBJX) -o $(exe_file) $(LDFLAGS)
	@find . -name \*.o -exec rm '{}' \;

clean:
	@find . -name \*.o -exec rm '{}' \;
	@rm -rf $(exe_file)
	

