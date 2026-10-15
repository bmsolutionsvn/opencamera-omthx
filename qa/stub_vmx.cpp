/*
 * stub_vmx.cpp — a link-time stand-in for libvmx, used by qa/encoder-create-failure.cpp and
 * qa/wire-declaration.cpp.
 *
 * Why a stand-in is needed: VMX_Create() returns NULL when its own dimension checks fail, and
 * Sender::validateFrame() now enforces the very same limits (both quote vmxcodec_common.h), so no
 * frame a caller can construct reaches that branch through the real codec any more. Leaving the
 * "the encoder could not be created" path untested would leave the most interesting failure of the
 * whole path — what the sender does when the codec says no — unproven. So the codec is replaced at
 * link time with a recorder that reports what the sender asked for.
 *
 * It is a recorder, not a fake encoder. Two things it answers, both of which the real codec keeps to
 * itself:
 *   - every VMX_Create() argument, in order (qa/wire-declaration.cpp W-A/W-B/W-C: did the sender ask
 *     for a new encoder when the colour space changed, or reuse one?);
 *   - the identity of the instance that wrote a given payload, stamped into the payload itself
 *     (W-A2: the header's declared colour space must equal the colour space of the encoder that
 *     produced the bytes — the declaration/producer agreement, seen from the receiver's side).
 * VMX_SaveTo hands back a fixed blob and nothing here understands the bitstream; the scenarios that
 * need the real codec (frame-validation, colorspace-gate, drop-policy, slow-subscriber,
 * encoder-reconfiguration) link libvmx/src instead.
 */
#include "vmxcodec.h"

#include <cstdlib>
#include <cstring>

namespace qa_stub {
    int failWidth = 0, failHeight = 0;   // the size VMX_Create must refuse; 0/0 = never
    int payloadBytes = 4096;             // what VMX_SaveTo claims to have written; <= 0 = nothing
    int createCalls = 0, destroyCalls = 0, encodeCalls = 0, saveCalls = 0;

    // Every create, in order, with the arguments the sender actually passed.
    struct Create { int width, height, profile, colorSpace; };
    Create creates[32];
    int createRecords = 0;

    // not constexpr: the test links against it, and a const at namespace scope has internal
    // linkage in C++ (the same trap the AsyncPool constants avoid).
    unsigned kPayloadMagic = 0x56584D51u;   // "QMXV": this blob came out of the recorder

    void reset() {
        failWidth = failHeight = 0;
        payloadBytes = 4096;
        createCalls = destroyCalls = encodeCalls = saveCalls = 0;
        createRecords = 0;
        std::memset(creates, 0, sizeof(creates));
    }
}

extern "C" {

VMX_API VMX_INSTANCE* VMX_Create(VMX_SIZE dimensions, VMX_PROFILE profile, VMX_COLORSPACE colorSpace) {
    ++qa_stub::createCalls;
    if (dimensions.width == qa_stub::failWidth && dimensions.height == qa_stub::failHeight)
        return nullptr;      // exactly what the real codec does for a size it cannot build
    if (qa_stub::createRecords < (int)(sizeof(qa_stub::creates) / sizeof(qa_stub::creates[0]))) {
        qa_stub::creates[qa_stub::createRecords++] =
            qa_stub::Create{dimensions.width, dimensions.height, (int)profile, (int)colorSpace};
    }
    auto* inst = reinterpret_cast<VMX_INSTANCE*>(std::calloc(1, sizeof(VMX_INSTANCE)));
    if (!inst) return nullptr;
    // The recorder keeps the identity on the instance, so VMX_SaveTo can stamp which encoder — with
    // which colour space — wrote which payload. Nothing in the sender can reach in here and change
    // the story afterwards: it only ever calls the four entry points below.
    inst->Planes[0].Size = dimensions;
    inst->ColorSpace = colorSpace;
    inst->Profile = profile;
    inst->Quality = qa_stub::createRecords;      // 1-based sequence number of this create
    return inst;
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
    // Provenance stamp: which encoder wrote this, and what it was created as.
    if (n >= 20) {
        const unsigned magic = qa_stub::kPayloadMagic;
        const int cs = (int)instance->ColorSpace;
        const int w = instance->Planes[0].Size.width;
        const int h = instance->Planes[0].Size.height;
        const int seq = instance->Quality;
        std::memcpy(dst + 0,  &magic, 4);
        std::memcpy(dst + 4,  &cs, 4);
        std::memcpy(dst + 8,  &w, 4);
        std::memcpy(dst + 12, &h, 4);
        std::memcpy(dst + 16, &seq, 4);
    }
    return n;
}

} // extern "C"
