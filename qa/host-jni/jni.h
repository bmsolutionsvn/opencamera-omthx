/*
 * jni.h — host stand-in for the Android NDK's <jni.h>, for qa/jni-boundary.cpp ONLY.
 *
 * WHY THIS FILE EXISTS. The defect it exists to measure is in app/src/main/cpp/OMTBridge.cpp, in the
 * handful of lines between the JNI arguments and omtpack::packNv12(). Testing those lines against a
 * transcription of them proves nothing about them — a transcription drifts, and the transcription is
 * what gets fixed. So this header lets the QA suite compile and call the REAL production translation
 * unit on the host, under ASan, with no device and no JVM.
 *
 * WHAT IT IS NOT. It is not a JVM: there is no object model, no GC, no exceptions, no local frame.
 * jobject/jstring are opaque handles the test hands straight back to itself, and the JNIEnv below is
 * a struct the test constructs. Only the methods OMTBridge.cpp actually calls exist, with the
 * signatures copied from the NDK header so the bridge compiles unchanged:
 *
 *   GetDirectBufferAddress  — returns the base the test registered for that buffer
 *   GetDirectBufferCapacity — returns the capacity the test registered for that buffer
 *   GetStringUTFChars / ReleaseStringUTFChars / NewStringUTF
 *
 * The two buffer methods are the whole point. Registering capacity separately from the region the
 * test actually malloc'd is what turns "the packer trusted a caller-supplied length" into an ASan
 * heap-buffer-overflow instead of a passing assertion: the plane allocation is EXACTLY the capacity
 * the bridge was told about, so any read the boundary did not vet walks off a real redzone.
 *
 * The APK build never sees this file. It lives under qa/, and only qa/build-host.sh adds qa/host-jni
 * to the include path, for the jb-* scenarios.
 */
#ifndef OMT_QA_HOST_JNI_JNI_H
#define OMT_QA_HOST_JNI_JNI_H

#include <cstddef>
#include <cstdint>

#define JNIEXPORT
#define JNICALL

typedef int8_t   jboolean;
typedef uint8_t  jbyte;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef uint32_t jchar;
typedef int16_t  jshort;
typedef float    jfloat;
typedef double   jdouble;
typedef jint     jsize;

#define JNI_TRUE  1
#define JNI_FALSE 0

/* Distinct opaque types, as in the real header: a jstring is not a jobject and the compiler keeps
 * the bridge honest about which is which. */
struct _jobject;
struct _jstring;
struct _jbyteArray;
typedef _jobject*   jobject;
typedef _jstring*   jstring;
typedef _jbyteArray* jbyteArray;
typedef jobject     jclass;
typedef jobject     jthrowable;
typedef void*       jmethodID;
typedef void*       jfieldID;

/* The methods OMTBridge.cpp calls, and nothing else. A member that does not exist here is a compile
 * error the day the bridge starts calling it — which is the right failure: this file is a test
 * fixture, and a fixture that silently grows is how a boundary test stops testing the boundary. */
struct JNIEnv {
    void*  GetDirectBufferAddress(jobject buf) const;
    jlong  GetDirectBufferCapacity(jobject buf) const;
    const char* GetStringUTFChars(jstring s, jboolean* isCopy) const;
    void   ReleaseStringUTFChars(jstring s, const char* chars) const;
    jstring NewStringUTF(const char* bytes) const;
};

#endif /* OMT_QA_HOST_JNI_JNI_H */
