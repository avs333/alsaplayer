SIGNED_APK = app/build/outputs/apk/app-release.apk
LIB = app/libs/armeabi/liblossless.so
JAVA_SRC = $(wildcard app/src/main/java/net/avs234/alsaplayer/*.java) $(wildcard app/src/main/aidl/net/avs234/alsaplayer/*.aidl)
LIB_SRC = $(wildcard app/jni/*) $(wildcard app/jni/flac/*) $(wildcard app/jni/ape/*) $(wildcard app/jni/tinyxml/*) $(wildcard app/jni/include/*)

all:	$(SIGNED_APK)

$(SIGNED_APK):	$(LIB) $(JAVA_SRC)
	./gradlew assembleRelease

$(LIB): $(LIB_SRC)
	(cd app/jni; ndk-build; cd ../..)

install: $(SIGNED_APK)
	adb uninstall net.avs234.alsaplayer
	adb install $(SIGNED_APK)

