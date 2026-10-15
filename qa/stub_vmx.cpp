/*
 * stub_vmx.cpp — a link-time stand-in for libvmx, used ONLY by qa/encoder-create-failure.cpp.
 *
 * Why a stand-in is needed: VMX_Create() returns NULL when its own dimension checks fail, and
 * Sender::validateFrame() now enforces the very same limits (both quote vmxcodec_common.h), so no
 * frame a caller can construct reaches that branch through the real codec any more. Leaving the
 * "the encoder could not be created" path untested would leave the most interesting failure of the
 * whole path — what the sender does when the codec says no — unproven. So the codec is replaced at
 * link time with a recorder that reports what the sender asked for.
 *
 * This is deliberately NOT a fake encoder. VMX_SaveTo hands back a fixed blob and nothing here
 * understands the bitstream; the scenarios that need the real codec (frame-validation,
 * colorspace-gate, drop-policy, slow-subscriber) link libvmx/src directly.
 */
#include "vmxcodec.h"

#include <cstdlib>
#include <cstring>

namespace qa_stub {
    int failWidth = 0, failHeight = 0;   // the size VMX_Create must refuse; 0/0 = never
    int payloadBytes = 4096;             // what VMX_SaveTo claims to have written; <= 0 = nothing
    int createCalls = 0, destroyCalls = 0, encodeCalls = 0, saveCalls = 0;
    void reset() {
        failWidth = failHeight = 0;
        payloadBytes = 4096;
        createCalls = destroyCalls = encodeCalls = saveCalls = 0;
    }
}

extern "C" {

VMX_API VMX_INSTANCE* VMX_Create(VMX_SIZE dimensions, VMX_PROFILE, VMX_COLORSPACE) {
    ++qa_stub::createCalls;
    if (dimensions.width == qa_stub::failWidth && dimensions.height == qa_stub::failHeight)
        return nullptr;      // exactly what the real codec does for a size it cannot build
    return reinterpret_cast<VMX_INSTANCE*>(std::calloc(1, sizeof(VMX_INSTANCE)));
}

VMX_API void VMX_Destroy(VMX_INSTANCE* instance) {
    ++qa_stub::destroyCalls;
    std::free(instance);
}

static VMX_ERR stub_encode() { ++qa_stub::encodeCalls; return VMX_ERR_OK; }

VMX_API VMX_ERR VMX_EncodeBGRA(VMX_INSTANCE*, BYTE*, int, int) { return stub_encode(); }
VMX_API VMX_ERR VMX_EncodeBGRX(VMX_INSTANCE*, BYTE*, int, int) { return stub_encode(); }
VMX_API VMX_ERR VMX_EncodeUYVY(VMX_INSTANCE*, BYTE*, int, int) { return stub_encode(); }
VMX_API VMX_ERR VMX_EncodeYUY2(VMX_INSTANCE*, BYTE*, int, int) { return stub_encode(); }
VMX_API VMX_ERR VMX_EncodeNV12(VMX_INSTANCE*, BYTE*, int, BYTE*, int, int) { return stub_encode(); }

VMX_API int VMX_SaveTo(VMX_INSTANCE* instance, BYTE* dst, int maxLen) {
    ++qa_stub::saveCalls;
    if (!instance || !dst || maxLen <= 0) return 0;
    const int n = qa_stub::payloadBytes;
    if (n <= 0 || n > maxLen) return 0;      // "0 = failure", the contract the codec now honours
    std::memset(dst, 0x5A, (size_t)n);
    return n;
}

} // extern "C"
