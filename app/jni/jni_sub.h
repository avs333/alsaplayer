#ifndef ANDROID
typedef void * JNIEnv;
typedef void * jobject;
typedef void * jintArray;
typedef void * JNIEnv;
typedef char * jstring;
typedef int jint;
typedef long jlong;
typedef bool jboolean;
#define JNIEXPORT
#define JNICALL
#else
#include <jni.h>
#endif

