/*
 * android/log.h — host stand-in for the NDK's logging header, for qa/jni-boundary.cpp ONLY.
 *
 * OMTBridge.cpp logs the reason it refused a frame, and that reason string is part of the contract
 * this suite scores: "refuse with a named reason" is only provable if the test can read the name.
 * So __android_log_print is declared here and DEFINED BY THE TEST, which appends every line to a
 * buffer the assertions search. A test that threw the logs away could not tell "refused for the
 * right reason" from "refused for some reason", which is the vacuity this file exists to close.
 *
 * The APK build never sees this file: it lives under qa/ and only qa/build-host.sh puts qa/host-jni
 * on the include path, for one scenario.
 */
#ifndef OMT_QA_HOST_JNI_ANDROID_LOG_H
#define OMT_QA_HOST_JNI_ANDROID_LOG_H

#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG   3
#define ANDROID_LOG_INFO    4
#define ANDROID_LOG_WARN    5
#define ANDROID_LOG_ERROR   6
#define ANDROID_LOG_FATAL   7

/* Same shape as the NDK's (varargs, printf-checked), so OMTBridge.cpp compiles unchanged and a
 * broken format string in the bridge is still a compile error rather than a silent mistranslation. */
#ifdef __cplusplus
extern "C" {
#endif
int __android_log_print(int prio, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
#ifdef __cplusplus
}
#endif

#endif /* OMT_QA_HOST_JNI_ANDROID_LOG_H */
