/**
 * OMTProtocol.h - OMT Protocol Structures
 * Port of libomtnet frame header structures
 */

#ifndef OMT_PROTOCOL_H
#define OMT_PROTOCOL_H

#include <cstdint>

namespace omt {

#pragma pack(push, 1)

/**
 * Frame header - 16 bytes
 * Port of OMTFrame header structure
 */
struct FrameHeader {
    uint8_t version;           // Always 1
    uint8_t frameType;         // 1=Metadata, 2=Video, 3=Audio
    int64_t timestamp;         // Timestamp in 100ns ticks
    uint16_t metadataLength;   // Length of metadata
    int32_t dataLength;        // Includes extended header + payload
    
    static constexpr int SIZE = 16;
};

/**
 * Video extended header - 32 bytes
 * Contains video-specific information
 */
struct VideoExtHeader {
    int32_t codec;             // Codec fourCC (e.g., VMX1 = 0x31584D56)
    int32_t width;
    int32_t height;
    int32_t frameRateN;        // Numerator
    int32_t frameRateD;        // Denominator
    float aspectRatio;
    int32_t flags;             // OMTVideoFlags
    int32_t colorSpace;        // OMTColorSpace
    
    static constexpr int SIZE = 32;
};

/**
 * Audio extended header - 20 bytes
 * Contains audio-specific information
 */
struct AudioExtHeader {
    int32_t codec;             // Codec fourCC
    int32_t sampleRate;
    int32_t channels;
    int32_t samplesPerChannel;
    int32_t flags;
    
    static constexpr int SIZE = 20;
};

#pragma pack(pop)

// Codec FourCC values
namespace Codec {
    constexpr int32_t VMX1 = 0x31584D56;  // "VMX1"
    constexpr int32_t UYVY = 0x59565955;  // "UYVY"
    constexpr int32_t YUY2 = 0x32595559;  // "YUY2"
    constexpr int32_t NV12 = 0x3231564E;  // "NV12"
    constexpr int32_t BGRA = 0x41524742;  // "BGRA"
    constexpr int32_t PCM_F32 = 0x32334650; // "FP32" (floating point audio)
}

// Video flags — the numbers MUST match the authority in libomt.h:135-144 (OMTVideoFlags), which is
// what every conforming receiver decodes: Interlaced=1, Alpha=2, PreMultiplied=4, Preview=8,
// HighBitDepth=16.
//
// HighBitDepth was 4 here — and 4 is PreMultiplied on the wire — while PreMultiplied was absent.
// Nothing in this tree read the constant, so the mistake stayed latent; Sender::validateFrame() now
// reads the flag set to decide what may be advertised, which is exactly when a wrong constant turns
// into a wrong verdict (it would have rejected a legitimate PreMultiplied frame and waved through a
// HighBitDepth claim no payload can back). This is the same second-numbering trap the colour block
// below once fell into, where the header that defines the wire struct carried BT601=1/BT709=2.
// qa/wire-declaration.cpp pins every one of these against the public C enum, and the constants are
// pinned at compile time in OMTSender.cpp next to the colour-space assert.
namespace VideoFlags {
    constexpr int32_t None = 0;
    constexpr int32_t Interlaced = 1;
    constexpr int32_t Alpha = 2;
    constexpr int32_t PreMultiplied = 4;
    constexpr int32_t Preview = 8;
    constexpr int32_t HighBitDepth = 16;
}

// Colour space — the values that go into VideoExtHeader::colorSpace.
//
// These are NOT 0/1/2/3. The wire carries the OMTColorSpace numbering from libomt/libomt.h:112-115
// (0, 601, 709), which is what VMX_COLORSPACE in libvmx/src/vmxcodec.h:84-88 also uses, and what
// every conforming receiver (libomtnet's OMTAddress/OMTReceive, OBS) expects to read back. An
// earlier version of this block said BT601 = 1, BT709 = 2 — a second, contradictory numbering
// sitting inside the very header that defines the wire struct, so writing the obvious
// `extHeader.colorSpace = ColorSpace::BT709` emitted 2 and every receiver saw a colour space that
// does not exist. The app avoided visible breakage only because OMTBridge.cpp takes its values
// from the C enum instead of from here. qa/colorspace-gate.cpp asserts these numbers now.
namespace ColorSpace {
    constexpr int32_t Undefined = 0;
    constexpr int32_t BT601 = 601;
    constexpr int32_t BT709 = 709;
    // BT.2020 is defined by the spec but cannot be encoded: VMX_Create only knows 0/601/709, so
    // Sender::send refuses it rather than shipping a header that misdescribes the payload.
}

} // namespace omt

#endif // OMT_PROTOCOL_H
