/**
 * OMTSender.cpp - OMT Video Sender Implementation
 * Port of libomtnet/src/OMTSend.cs
 */

#include "OMTSender.h"
#include "OMTProtocol.h"
#include "../../libvmx/src/vmxcodec.h"
// VMX_MIN/MAX_WIDTH/HEIGHT: the codec's own dimension limits. validateFrame() quotes them rather
// than repeating the numbers, so the sender's gate and VMX_Create() cannot drift apart.
#include "../../libvmx/src/vmxcodec_common.h"

#include <cstring>
#include <algorithm>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sstream>
#include <cstdint>
#include <climits>   // INT_MAX: VMX_SaveTo() takes its capacity as an int
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#define LOG_TAG "omt-sender"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) 
#define LOGE(...)
#define LOGW(...)
#define LOGD(...)
#endif

namespace omt {

namespace {
// The encode buffer is sized for the largest payload VMX can produce for one frame. The lower
// bound is the desktop value (a 1 MB floor: an intra-frame codec can expand a tiny, noisy frame);
// the upper bound is what the largest frame the sender admits could ever need, so the resize can
// never be asked for an absurd size by a caller that slipped past validateFrame().
constexpr size_t kMinEncodeBufferBytes = 1024ull * 1024ull;
constexpr size_t kMaxEncodeBufferBytes =
    static_cast<size_t>(VMX_MAX_WIDTH) * static_cast<size_t>(VMX_MAX_HEIGHT) * 4ull;
// VMX_SaveTo() takes its capacity as an int; if the ceiling ever outgrew that the call below
// would pass a negative length and the codec would write wherever it liked.
static_assert(kMaxEncodeBufferBytes <= static_cast<size_t>(INT_MAX),
              "encode buffer must stay addressable by VMX_SaveTo's int maxLen");

inline const char* codecName(int codec) {
    if (codec == Codec::BGRA) return "BGRA";
    if (codec == Codec::UYVY) return "UYVY";
    if (codec == Codec::YUY2) return "YUY2";
    if (codec == Codec::NV12) return "NV12";
    return "unknown";
}
} // namespace

Sender::Sender(const SenderConfig& config) : config_(config) {
    LOGI("Sender: creating '%s' quality=%d", config_.name.c_str(), static_cast<int>(config_.quality));
    currentProfile_ = selectProfile(config_.quality);
}

Sender::~Sender() {
    LOGI("Sender: destroying");
    stop();
    destroyEncoder();
}

bool Sender::start() {
    if (running_.load()) return true;
    
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ < 0) {
        LOGE("Sender: failed to create socket");
        return false;
    }
    
    int one = 1;
    setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);
    
    if (bind(serverSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("Sender: failed to bind port %d", config_.port);
        close(serverSocket_);
        serverSocket_ = -1;
        return false;
    }
    
    listen(serverSocket_, 5);
    running_ = true;
    
    LOGI("Sender: listening on port %d", config_.port);
    
    acceptThread_ = std::thread(&Sender::acceptLoop, this);
    return true;
}

void Sender::stop() {
    if (!running_.load()) return;
    
    running_ = false;
    
    if (serverSocket_ > 0) {
        shutdown(serverSocket_, SHUT_RDWR);
        close(serverSocket_);
        serverSocket_ = -1;
    }
    
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    
    // Disconnect all channels
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        channels_.clear();
    }
    
    LOGI("Sender: stopped");
}

void Sender::acceptLoop() {
    LOGI("acceptLoop: started");
    
    while (running_) {
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        
        int clientFd = accept(serverSocket_, 
                              reinterpret_cast<sockaddr*>(&clientAddr), 
                              &addrLen);
        
        if (clientFd < 0) {
            if (errno == EINVAL || errno == EBADF) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, sizeof(clientIP));
        LOGI("New connection from %s:%d", clientIP, ntohs(clientAddr.sin_port));
        
        // Get current optimal buffer size
        int bufferSize = config_.sendBufferSize;
        {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            if (currentWidth_ > 0 && currentHeight_ > 0) {
                bufferSize = calculateOptimalBuffer(currentWidth_, currentHeight_, currentProfile_);
            }
        }
        
        auto channel = std::make_shared<Channel>(clientFd, clientAddr, bufferSize, this);
        
        // Send configured sender info immediately
        channel->sendMetadataSync(config_.senderInfoXml.c_str());

        if (onConnect_) {
            onConnect_(channel.get());
        }
        
        addChannel(std::move(channel));
    }
    
    LOGI("acceptLoop: exiting");
}

void Sender::addChannel(std::shared_ptr<Channel> channel) {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    channels_.push_back(std::move(channel));
}

void Sender::removeChannel(Channel* channel) {
    // Unlink under the lock; ~Channel joins two threads, and the camera thread takes this same
    // mutex for every frame it fans out.
    std::vector<std::shared_ptr<Channel>> doomed;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (auto it = channels_.begin(); it != channels_.end(); ++it) {
            if (it->get() == channel) { doomed.push_back(std::move(*it)); channels_.erase(it); break; }
        }
    }
}

int64_t Sender::getRecentDropsAndReset() {
    // omt_send_get_recent_drops_and_reset() was a stub that returned 0 with a comment saying it
    // "would need to track in Sender". The consequence: OMTStreamingManager's whole 1-second
    // congestion alert (onFramesDropped) was unreachable code — the UI could never tell the user
    // that the stream was dropping frames. Sum the live channels and compare against the baseline
    // taken at the previous call, so a caller polling once a second sees per-second drops.
    int64_t total = 0;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (const auto& ch : channels_) total += ch->framesDropped();
    }
    const int64_t delta = total - drops_baseline_.exchange(total);
    return delta > 0 ? delta : 0;
}

void Sender::cleanupDisconnected() {
    // Same rule as removeChannel: unlink fast, join outside the critical section.
    std::vector<std::shared_ptr<Channel>> doomed;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (auto it = channels_.begin(); it != channels_.end(); ) {
            if (!it->get()->isConnected()) { doomed.push_back(std::move(*it)); it = channels_.erase(it); }
            else ++it;
        }
    }
}

int Sender::connectionCount() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    return static_cast<int>(channels_.size());
}

SenderStats Sender::getStats() {
    cleanupDisconnected();
    
    SenderStats stats;
    stats.totalEncodedBytes = totalEncodedBytes_.load();
    
    std::lock_guard<std::mutex> lock(channelsMutex_);
    stats.connectionCount = static_cast<int>(channels_.size());
    
    for (const auto& ch : channels_) {
        auto chStats = ch->getStats();
        stats.totalFramesSent += chStats.framesSent;
        stats.totalFramesDropped += chStats.framesDropped;
        stats.totalBytesSent += chStats.bytesSent;
    }
    
    return stats;
}

int Sender::selectProfile(Quality quality) const {
    switch (quality) {
        case Quality::Low: return VMX_PROFILE_OMT_LQ;
        case Quality::Medium: return VMX_PROFILE_OMT_SQ;
        case Quality::High: return VMX_PROFILE_OMT_HQ;
        default: return VMX_PROFILE_OMT_SQ;
    }
}

void Sender::setQuality(Quality quality) {
    LOGI("Updating quality to %d", static_cast<int>(quality));
    
    std::lock_guard<std::mutex> lock(encoderMutex_);
    config_.quality = quality;
    
    if (quality != Quality::Default) {
        int newProfile = selectProfile(quality);
        if (newProfile != currentProfile_) {
            currentProfile_ = newProfile;
            destroyEncoder();
            LOGI("Profile changed to %d on manual request", newProfile);
        }
    }
}

int Sender::calculateOptimalBuffer(int width, int height, int profile) {
    int rawSize = width * height * 3 / 2;
    
    float compressionRatio;
    switch (profile) {
        case VMX_PROFILE_OMT_HQ: compressionRatio = 0.25f; break;
        case VMX_PROFILE_OMT_SQ: compressionRatio = 0.18f; break;
        case VMX_PROFILE_OMT_LQ: compressionRatio = 0.12f; break;
        default: compressionRatio = 0.20f; break;
    }
    
    int bufferSize = static_cast<int>(rawSize * compressionRatio * 1.5f) + 4096;
    
    if (bufferSize < 256 * 1024) bufferSize = 256 * 1024;
    if (bufferSize > 2 * 1024 * 1024) bufferSize = 2 * 1024 * 1024;
    
    return bufferSize;
}

size_t Sender::encodeBufferCapacity(int width, int height) {
    // The desktop sender computes this in 64-bit and clamps it; the Android copy wrote
    // `int maxSize = width * height * 4`, a signed multiply that wraps. At 32768x16385 it lands on
    // a negative number, the `if (maxSize < 1 MB)` clamp below then "repairs" it to 1 MB, and the
    // buffer no longer has anything to do with the frame it is supposed to hold — the codec either
    // truncates or, before the SaveTo contract was fixed, wrote past it. validateFrame() bounds the
    // dimensions today, so this is the belt to that brace: the arithmetic stays right even if the
    // gate in front of it is removed.
    if (width <= 0 || height <= 0) return kMinEncodeBufferBytes;
    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    const uint64_t maxPixels =
        static_cast<uint64_t>(VMX_MAX_WIDTH) * static_cast<uint64_t>(VMX_MAX_HEIGHT);
    // Anything at or above the largest admissible frame takes the ceiling; the multiply below can
    // only be performed on values that cannot overflow 64 bits.
    if (pixels >= maxPixels) return kMaxEncodeBufferBytes;
    uint64_t bytes = pixels * 4ull;
    if (bytes < kMinEncodeBufferBytes) bytes = kMinEncodeBufferBytes;
    return static_cast<size_t>(bytes);
}

// Returns false when the encoder could not be created, with the reason in `why`. The size cache is
// written ONLY on success: the old code cached currentWidth_/currentHeight_ even when VMX_Create
// returned NULL, so acceptLoop() sized every later subscriber's pool from a frame that does not
// exist, and the failure surfaced as the generic "encoder produced no payload" — the one line an
// operator needed was the one that was missing.
bool Sender::createEncoder(int width, int height, int frameRate, int colorSpace, std::string& why) {
    std::lock_guard<std::mutex> lock(encoderMutex_);

    if (vmxInstance_ && currentWidth_ == width && currentHeight_ == height) {
        return true;  // Already have correct encoder. Recreated via destroyEncoder() if profile changed.
    }

    destroyEncoder();

    VMX_SIZE size = {width, height};
    VMX_COLORSPACE cspace = static_cast<VMX_COLORSPACE>(colorSpace);

    vmxInstance_ = VMX_Create(size, static_cast<VMX_PROFILE>(currentProfile_), cspace);
    if (!vmxInstance_) {
        why = "VMX_Create refused " + std::to_string(width) + "x" + std::to_string(height) +
              " (profile " + std::to_string(currentProfile_) + ", colorspace " +
              std::to_string(colorSpace) + ", fps " + std::to_string(frameRate) +
              "): no encoder, refusing the frame instead of encoding into nothing";
        LOGE("Sender::createEncoder: %s", why.c_str());
        // currentWidth_/currentHeight_ were cleared by destroyEncoder() above and stay cleared:
        // there is no encoder to describe.
        return false;
    }

    currentWidth_ = width;
    currentHeight_ = height;

    encodeBuffer_.resize(encodeBufferCapacity(width, height));

    LOGI("Encoder created: %dx%d, profile=%d", width, height, static_cast<int>(currentProfile_));
    return true;
}

void Sender::destroyEncoder() {
    if (vmxInstance_) {
        VMX_Destroy(vmxInstance_);
        vmxInstance_ = nullptr;
    }
    currentWidth_ = 0;
    currentHeight_ = 0;
}

int Sender::encodeFrame(const MediaFrame& frame) {
    std::lock_guard<std::mutex> lock(encoderMutex_);
    
    if (!vmxInstance_) return 0;
    
    int interlaced = (frame.flags & VideoFlags::Interlaced) ? 1 : 0;
    VMX_ERR err = VMX_ERR_UNKNOWN;
    
    BYTE* data = static_cast<BYTE*>(frame.data);
    
    // Encode based on input codec
    switch (frame.codec) {
        case Codec::UYVY:
            err = VMX_EncodeUYVY(vmxInstance_, data, frame.stride, interlaced);
            break;
        case Codec::YUY2:
            err = VMX_EncodeYUY2(vmxInstance_, data, frame.stride, interlaced);
            break;
        case 0x3231564E:  // NV12
            {
                BYTE* y = data;
                BYTE* uv = data + (frame.stride * frame.height);
                err = VMX_EncodeNV12(vmxInstance_, y, frame.stride, uv, frame.stride, interlaced);
            }
            break;
        case Codec::BGRA:
            err = VMX_EncodeBGRA(vmxInstance_, data, frame.stride, interlaced);
            break;
        default:
            LOGE("Unknown codec: 0x%08X", frame.codec);
            return 0;
    }
    
    if (err != VMX_ERR_OK) {
        LOGE("VMX encode failed: %d", err);
        return 0;
    }
    
    // P0 #10 (from the desktop vendored copy): VMX_SaveTo() now answers every failure with 0, but
    // the sender is the party that stamps the FrameHeader, so the sender is also the party that
    // must not put a payload length on the wire that it cannot point at. Anything outside
    // (0, capacity] is a failure whatever the codec chose to call it — before this line existed,
    // VMX_SaveTo's null-instance path returned VMX_ERR_INVALID_INSTANCE (5), encodeFrame() passed
    // it straight back, send() only tested `encodedLen <= 0`, and a 5-byte "video frame" with a
    // full FrameHeader + VideoExtHeader claiming 64x64@30 went out to every subscriber on the
    // link. Desktop rejects the same two conditions in encodeFrameLocked
    // (vendor/libomtcpp/src/OMTSender.cpp:546).
    const size_t capacity = encodeBuffer_.size();
    // VMX_SaveTo() takes its capacity as an int. Rounding a (never observed) oversized buffer DOWN
    // in that direction can only make the codec write less, never more.
    const int capacityInt = capacity > (size_t)INT_MAX ? INT_MAX : (int)capacity;
    int encodedLen = VMX_SaveTo(vmxInstance_, encodeBuffer_.data(), capacityInt);
    if (!encodedLenAcceptable(encodedLen, capacity)) {
        LOGE("Sender::encodeFrame: VMX_SaveTo returned %d for a %zu byte encode buffer — refused",
             encodedLen, capacity);
        return 0;
    }
    return encodedLen;
}

// The publishable range, spelled out so the host suite can assert it directly: a positive length
// that fits inside the buffer we actually own. qa/build-host.sh neutralises this one line and the
// suite must go red.
bool Sender::encodedLenAcceptable(int encodedLen, size_t capacity) {
    return encodedLen > 0 && static_cast<size_t>(encodedLen) <= capacity;
}

// The wire carries these numbers to every receiver in the ecosystem, and this project has
// previously carried a second, contradictory set (BT601=1, BT709=2) in OMTProtocol.h. Pin the
// values here so a silent renumbering is a compile error rather than a colour shift on OBS.
static_assert(omt::ColorSpace::Undefined == 0 && omt::ColorSpace::BT601 == 601 &&
              omt::ColorSpace::BT709 == 709,
              "VideoExtHeader::colorSpace must use the OMT/VMX numbering (0/601/709)");

// P0 #9 / P0 #16, ported from the desktop vendored sender and adapted to this tree. The
// colour-space clause uses THIS project's names (ColorSpace::Undefined, not desktop's Unknown)
// because the static_assert above and qa/colorspace-gate.cpp pin the wire to 0/601/709; restoring a
// second "Unknown = 0" constant would recreate the contradictory numbering that assert exists to
// prevent.
//
// Every field below arrives from a caller that filled a plain struct by hand. What the pre-fix code
// did with one from Camera2: OMTBridge.cpp computes
//
//     dataLength = height*yStride + (height/2)*uvStride
//
// but OMTMediaFrame has room for ONE stride, and encodeFrame() passes that single value to VMX as
// both srcStrideY and srcStrideUV. The encoder therefore walks the chroma plane at
//
//     data + height*yStride + row*yStride     for row in [0, height/2)
//
// — it needs (height/2)*yStride bytes after the luma plane, while the caller only reserved
// (height/2)*uvStride. Any device reporting uvStride < yStride (ordinary ImageReader row padding,
// which Camera2 exposes as two separate rowStride values) put that read past the end of the
// ImageReader allocation: a SIGSEGV in the camera process on an untested device, with no log line
// and no counter anywhere. The codec-specific `need` below is what catches it, computed with the
// stride the encoder will actually use — not the one it was told the buffer has.
bool Sender::validateFrame(const MediaFrame& frame, std::string& why) {
    auto reject = [&why](const std::string& reason) {
        why = "frame rejected: " + reason;
        return false;
    };

    if (frame.data == nullptr) return reject("null data pointer");

    // The codec's own limits, quoted from vmxcodec_common.h so this gate and VMX_Create() cannot
    // drift apart: a size VMX_Create refuses would otherwise return NULL from createEncoder().
    if (frame.width < VMX_MIN_WIDTH || frame.width > VMX_MAX_WIDTH)
        return reject("width " + std::to_string(frame.width) + " is outside " +
                      std::to_string(VMX_MIN_WIDTH) + ".." + std::to_string(VMX_MAX_WIDTH) +
                      " (VMX_Create cannot build an encoder for it)");
    if (frame.height < VMX_MIN_HEIGHT || frame.height > VMX_MAX_HEIGHT)
        return reject("height " + std::to_string(frame.height) + " is outside " +
                      std::to_string(VMX_MIN_HEIGHT) + ".." + std::to_string(VMX_MAX_HEIGHT) +
                      " (VMX_Create cannot build an encoder for it)");
    if ((frame.width & 1) != 0)
        return reject("odd width " + std::to_string(frame.width) +
                      " (VMX_Create rejects it, and 8x8 blocks cannot tile an odd row)");
    if (frame.stride <= 0)
        return reject("stride " + std::to_string(frame.stride) +
                      " (the encoder adds stride*row to the data pointer; 0 or negative walks off the buffer)");
    if (frame.dataLength < 0)
        return reject("dataLength " + std::to_string(frame.dataLength) + " is negative");

    // frameRateD is a divisor twice over in this path (createEncoder's ratio and the bitrate
    // estimate in send()), and MediaFrame is a plain struct a caller fills by hand — leaving the
    // denominator at 0 used to be a SIGFPE that took the camera process with it. A negative one is
    // just as invalid and would advertise a negative rate on the wire.
    if (frame.frameRateD <= 0)
        return reject("frameRateD is " + std::to_string(frame.frameRateD) +
                      " (frameRateN/frameRateD is a ratio; refusing to divide by zero or a negative)");
    // P0 #16: the numerator is the other half of the same advertised ratio. 0 or negative produced
    // a header every receiver reads as a stream that never advances, and a bitrate estimate of 0.
    if (frame.frameRateN <= 0)
        return reject("frameRateN is " + std::to_string(frame.frameRateN) +
                      " (a non-positive frame rate cannot be advertised on the wire)");

    const uint64_t w = static_cast<uint64_t>(frame.width);
    const uint64_t h = static_cast<uint64_t>(frame.height);
    const uint64_t s = static_cast<uint64_t>(frame.stride);

    // The required byte count is CODEC-SPECIFIC, and it is computed with the stride the encoder
    // will really use. Applying one formula to every codec (as the first desktop version did) both
    // rejected valid tightly-packed frames and accepted truncated padded ones.
    uint64_t need = 0;
    if (frame.codec == Codec::BGRA) {
        const uint64_t rowBytes = w * 4ull;
        if (s < rowBytes)
            return reject("stride " + std::to_string(frame.stride) + " is narrower than one BGRA row (" +
                          std::to_string(rowBytes) + " bytes at width " + std::to_string(frame.width) + ")");
        need = (h - 1ull) * s + rowBytes;
    } else if (frame.codec == Codec::UYVY || frame.codec == Codec::YUY2) {
        const uint64_t rowBytes = w * 2ull;                       // packed 4:2:2
        if (s < rowBytes)
            return reject("stride " + std::to_string(frame.stride) + " is narrower than one " +
                          std::string(codecName(frame.codec)) + " row (" + std::to_string(rowBytes) +
                          " bytes at width " + std::to_string(frame.width) + ")");
        need = (h - 1ull) * s + rowBytes;
    } else if (frame.codec == Codec::NV12) {
        if (s < w)
            return reject("stride " + std::to_string(frame.stride) + " cannot hold a luma row of " +
                          std::to_string(frame.width) + " bytes");
        // Chroma is subsampled vertically (height/2 rows), so an odd height leaves the final luma
        // row without chroma; width evenness is checked above, height evenness is NV12's own.
        if ((frame.height & 1) != 0)
            return reject("NV12 with an odd height " + std::to_string(frame.height) +
                          " (the chroma plane is height/2 rows, so the last luma row has no chroma)");
        // Y plane (h rows * stride) + interleaved UV plane (h/2 rows * stride) — both walked with
        // frame.stride by encodeFrame(), which is why the UV half is charged at yStride here and not
        // at whatever uvStride the caller happened to allocate with.
        need = h * s + (h / 2ull) * s;
    } else {
        char hexBuf[12];
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%08X", static_cast<unsigned>(frame.codec));
        return reject(std::string("unknown codec ") + hexBuf +
                      " (this sender encodes BGRA/UYVY/YUY2/NV12 only)");
    }

    if (static_cast<uint64_t>(frame.dataLength) < need) {
        std::string detail = "buffer too small for " + std::string(codecName(frame.codec)) + " " +
                             std::to_string(frame.width) + "x" + std::to_string(frame.height) +
                             " stride=" + std::to_string(frame.stride) + ": the encoder reads " +
                             std::to_string(need) + " bytes, dataLength is " +
                             std::to_string(frame.dataLength);
        if (frame.codec == Codec::NV12)
            detail += " (a Camera2 device whose uv rowStride is smaller than its y rowStride lands here: "
                      "the chroma plane is walked with the luma stride, so the read would run past the "
                      "ImageReader allocation — refuse the frame, do not read it)";
        return reject(detail);
    }

    // Fail closed on a colour space the encoder cannot honour, BEFORE anything else and before
    // the no-clients shortcut. MediaFrame::colorSpace is a plain int and the wire header field is
    // int32_t, so nothing in the type system stops a caller passing 2020 (which the OMT spec
    // defines and this codec does not): createEncoder() would static_cast it straight into
    // VMX_COLORSPACE — whose real values are only 0/601/709 — and send() would then stamp the
    // same number into extHeader.colorSpace. The payload would be encoded with whatever the codec
    // fell back to while the header told every receiver a different story. A same-codec loopback
    // cannot see that: both ends share the fallback. Only the boundary can refuse it.
    if (frame.colorSpace != ColorSpace::Undefined &&
        frame.colorSpace != ColorSpace::BT601 &&
        frame.colorSpace != ColorSpace::BT709) {  // wire numbering: 0/601/709, see OMTProtocol.h
        return reject("unsupported colorspace " + std::to_string(frame.colorSpace) +
                      " (VMX encodes 601/709 only; the wire cannot carry a claim the payload does not match)");
    }
    return true;
}

int Sender::send(const MediaFrame& frame) {
    // One failure exit for everything the sender refuses to emit a payload for. b66ab347 added
    // lastError()/encodeFailures() precisely because a frame that left the camera and produced
    // nothing was invisible to every counter the UI reads — "a healthy stream that nobody watched".
    // The new gates below would otherwise have recreated that bug with a second silent `return 0`,
    // so validation, encoder creation and the encode itself all land here, counted and named.
    std::string why;
    int encodedLen = 0;
    bool havePayload = false;

    // Runs BEFORE the no-clients shortcut, like the colour-space gate it absorbs: a frame the
    // buffer cannot honour is refused whether or not anyone is watching, so the device that would
    // have crashed on air is refused on the bench instead.
    if (!validateFrame(frame, why)) {
        // `why` is filled in; fall through to the shared failure exit.
    } else {
        // Cleanup disconnected first
        cleanupDisconnected();

        if (connectionCount() == 0) return 1;  // No clients, but not an error

        // Create/update encoder. A frame is only encoded against an encoder that really exists:
        // the old code called this and ignored whether VMX_Create returned NULL.
        if (createEncoder(frame.width, frame.height,
                          frame.frameRateN / frame.frameRateD,
                          frame.colorSpace, why)) {
            // Encode frame
            encodedLen = encodeFrame(frame);
            havePayload = encodedLen > 0;
            if (!havePayload) {
                why = "encoder produced no payload (codec " +
                      std::to_string(frame.codec) + ", " + std::to_string(frame.width) + "x" +
                      std::to_string(frame.height) + ")";
            }
        }
    }

    if (!havePayload) {
        setLastError(why);
        encodeFailures_++;
        LOGE("Sender::send: %s (total encode failures %lld)", why.c_str(),
             (long long)encodeFailures_.load());
        return 0;
    }
    
    totalEncodedBytes_ += encodedLen;
    int count = frameCount_++;
    
    // Log periodically
    if (count % 30 == 0) {
        float avgBytes = static_cast<float>(totalEncodedBytes_.load()) / (count + 1);
        float mbps = (avgBytes * 8.0f * frame.frameRateN / frame.frameRateD) / 1000000.0f;
        LOGI("Frame %d: %d bytes (~%.1f Mbps)", count, encodedLen, mbps);
        checkCongestion();
    }
    
    // Build headers
    FrameHeader header{};
    header.version = 1;
    header.frameType = static_cast<uint8_t>(FrameType::Video);
    header.timestamp = frame.timestamp;
    header.metadataLength = 0;
    header.dataLength = VideoExtHeader::SIZE + encodedLen;
    
    VideoExtHeader extHeader{};
    extHeader.codec = Codec::VMX1;
    extHeader.width = frame.width;
    extHeader.height = frame.height;
    extHeader.frameRateN = frame.frameRateN;
    extHeader.frameRateD = frame.frameRateD;
    extHeader.aspectRatio = frame.aspectRatio;
    extHeader.flags = frame.flags;
    extHeader.colorSpace = frame.colorSpace;
    
    // Send to all subscribed clients
    int totalSent = 0;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        for (const auto& ch : channels_) {
            if (!ch->isConnected()) continue;
            
            // Only send if client has subscribed to video (per OMT protocol)
            if (!ch->isVideoSubscribed()) continue;
            
            int sent = ch->sendAsync(&header, sizeof(header),
                                     &extHeader, sizeof(extHeader),
                                     encodeBuffer_.data(), encodedLen);
            if (sent > 0) {
                totalSent += sent;
            }
        }
    }
    
    return totalSent > 0 ? encodedLen : 0;
}

void Sender::sendMetadata(const std::string& xml) {
    // Snapshot the list, release the lock, THEN write to sockets. sendMetadataSync used to block
    // without a deadline while channelsMutex_ was held - the same mutex the camera thread needs
    // in send() - so one stalled viewer froze the whole stream instead of just itself. The
    // shared_ptr keeps each channel alive while we write to it even if another thread prunes the
    // list meanwhile.
    std::vector<std::shared_ptr<Channel>> snapshot;
    {
        std::lock_guard<std::mutex> lock(channelsMutex_);
        snapshot.reserve(channels_.size());
        for (const auto& ch : channels_) if (ch->isConnected()) snapshot.push_back(ch);
    }
    for (const auto& ch : snapshot) ch->sendMetadataSync(xml.c_str());
}

Quality Sender::getBestSuggestedQuality() const {
    Quality best = Quality::Default;
    
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected()) {
            Quality q = ch->getSuggestedQuality();
            if (static_cast<int>(q) > static_cast<int>(best)) {
                best = q;
            }
        }
    }
    
    return best;
}

bool Sender::hasTallyPreview() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected() && ch->isTallyPreview()) {
            return true;
        }
    }
    return false;
}

bool Sender::hasTallyProgram() const {
    std::lock_guard<std::mutex> lock(channelsMutex_);
    for (const auto& ch : channels_) {
        if (ch->isConnected() && ch->isTallyProgram()) {
            return true;
        }
    }
    return false;
}

// IChannelListener implementation

void Sender::onDisconnected(Channel* channel) {
    LOGI("Channel disconnected");
    if (onDisconnect_) {
        onDisconnect_(channel);
    }
    // Cleanup will happen on next send or getStats call
}

void Sender::onTallyChanged(Channel* channel) {
    LOGI("Tally changed: preview=%d, program=%d",
         channel->isTallyPreview(), channel->isTallyProgram());
}

void Sender::onQualityChanged(Channel* channel) {
    LOGI("Quality suggested: %d", static_cast<int>(channel->getSuggestedQuality()));
    
    // If using auto quality, update encoder
    if (config_.quality == Quality::Default) {
        Quality best = getBestSuggestedQuality();
        if (best != Quality::Default) {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            int newProfile = selectProfile(best);
            if (newProfile != currentProfile_) {
                currentProfile_ = newProfile;
                // Encoder will be recreated on next send
                destroyEncoder();
                LOGI("Profile changed to %d based on receiver suggestion", 
                     static_cast<int>(newProfile));
            }
        }
    }
}

void Sender::setSenderInfo(const std::string& productName, const std::string& manufacturer) {
    std::ostringstream ss;
    ss << "<OMTInfo ProductName=\"" << productName << "\" Manufacturer=\"" << manufacturer << "\" Version=\"1.0\" />";
    config_.senderInfoXml = ss.str();
    
    // Broadcast to existing channels
    sendMetadata(config_.senderInfoXml);
}

void Sender::checkCongestion() {
    SenderStats stats = getStats();
    if (stats.totalFramesSent < 30) return; // Too early

    double dropRate = (double)stats.totalFramesDropped / (double)(stats.totalFramesSent + stats.totalFramesDropped);
    
    if (dropRate > 0.05) { // > 5% drops
        LOGW("High drop rate detected: %.2f%%", dropRate * 100.0);
        
        // Downgrade profile if possible
        if (config_.quality == Quality::Default || config_.quality == Quality::High || config_.quality == Quality::Medium) {
            std::lock_guard<std::mutex> lock(encoderMutex_);
            int current = currentProfile_;
            int lower = VMX_PROFILE_OMT_LQ;
            
            if (current == VMX_PROFILE_OMT_HQ) lower = VMX_PROFILE_OMT_SQ;
            else if (current == VMX_PROFILE_OMT_SQ) lower = VMX_PROFILE_OMT_LQ;
            
            if (lower != current) {
                currentProfile_ = lower;
                destroyEncoder();
                LOGI("Congestion Control: Downgraded profile to %d", currentProfile_);
            }
        }
    }
}

// ... existing code ...

} // namespace omt
