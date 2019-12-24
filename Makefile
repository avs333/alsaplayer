SIGNED_APK = app/build/outputs/apk/app-release.apk
LIB = app/libs/armeabi-v7a/liblossless.so
JAVA_SRC = $(wildcard app/src/main/java/net/avs234/alsaplayer/*.java) $(wildcard app/src/main/aidl/net/avs234/alsaplayer/*.aidl)
LIB_SRC = $(wildcard app/jni/*) $(wildcard app/jni/flac/*) $(wildcard app/jni/ape/*) $(wildcard app/jni/tinyxml/*) $(wildcard app/jni/include/*)

all:	$(SIGNED_APK)

$(SIGNED_APK):	$(LIB) $(JAVA_SRC)
	@perl -i settime.pl app/src/main/res/values/strings.xml 
	@perl -i settime.pl app/src/main/res/values-ru/strings.xml
	@echo "building java source" 
	./gradlew assembleRelease

$(LIB): $(LIB_SRC)
	@echo "building library"
	(cd app/jni; /mnt/d/android/android-ndk-r17b/ndk-build; cd ../..)

install: $(SIGNED_APK)
	adb uninstall net.avs234.alsaplayer
	adb install $(SIGNED_APK)

