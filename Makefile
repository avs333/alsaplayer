SIGNED_APK = alsaplayer.apk
LIB = app/libs/armeabi/liblossless.so
APK = app/build/outputs/apk/app-release-unsigned.apk
JAVA_SRC = $(wildcard app/src/main/java/net/avs234/alsaplayer/*.java) $(wildcard app/src/main/aidl/net/avs234/alsaplayer/*.aidl)
LIB_SRC = $(wildcard app/jni/*) $(wildcard app/jni/flac/*) $(wildcard app/jni/ape/*) $(wildcard app/jni/tinyxml/*) $(wildcard app/jni/include/*)
ALIAS = cert
PASS = verybadpassword
CERT_FILE = cert.keystore

all:	$(SIGNED_APK)

$(SIGNED_APK):	$(CERT_FILE) $(LIB) $(JAVA_SRC)
	./gradlew assembleRelease
	jarsigner -keystore $(CERT_FILE) -keypass $(PASS) -storepass $(PASS) -signedjar tmp.apk $(APK) $(ALIAS)
	zipalign -f 4 tmp.apk $(SIGNED_APK)
	rm tmp.apk

# jdk1.7:
# jarsigner -tsa http://timestamp.digicert.com -keystore $(CERT_FILE) -keypass $(PASS) -storepass $(PASS) -signedjar tmp.apk $(APK) $(ALIAS)

$(LIB): $(LIB_SRC)
	(cd app/jni; ndk-build; cd ../..)

$(CERT_FILE):
	keytool -genkey -keystore $(CERT_FILE) -alias $(ALIAS) -keyalg RSA -keysize 512 -validity 20000 \
		 -keypass $(PASS) -storepass $(PASS)  -dname "cn=nobody, ou=none, o=none, c=US"

install: $(SIGNED_APK)
	adb uninstall net.avs234.alsaplayer
	adb install alsaplayer.apk

