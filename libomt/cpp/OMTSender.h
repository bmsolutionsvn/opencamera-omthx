/**
 * OMTSender.h - OMT Video Sender
 * Port of libomtnet/src/OMTSend.cs
 * 
 * Main sender class that:
 * - Accepts TCP connections
 * - Manages channels (clients)
 * - Encodes video with VMX
 * - Sends frames to subscribers
 */

#ifndef OMT_SENDER_H
#define OMT_SENDER_H

#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <memory>
#include <string>
#include <functional>
#include <cstddef>

#include "OMTConstants.h"
#include "OMTProtocol.h"   // FrameHeader / VideoExtHeader: the declaration built by buildVideoHeaders
#include "OMTChannel.h"

// Forward declare VMX instance only (struct is fine)
struct VMX_INSTANCE;

namespace omt {

/**
 * Sender statistics
 */
struct SenderStats {
    int64_t totalFramesSent = 0;
    int64_t totalFramesDropped = 0;
    int64_t totalBytesSent = 0;
    int64_t totalEncodedBytes = 0;
    int connectionCount = 0;
};

/**
 * Video frame information (matches libomt.h OMTMediaFrame)
 */
struct MediaFrame {
    void* data;
    int dataLength;
    int width;
    int height;
    int stride;
    int codec;
    int frameRateN;
    int frameRateD;
    float aspectRatio;
    int flags;
    int colorSpace;
    int64_t timestamp;
};

/**
 * The identity of the VMX instance that is currently alive — everything that changes the pixels the
 * codec hands back, and therefore the only thing the wire declaration may be derived from.
 *
 * Colour space belongs in this key: VMX picks its RGB<->YUV conversion table from the create-time
 * colour space (vmxcodec.cpp:1448/1466/1654/1673), so an encoder created as 601 really does produce
 * different bytes from one created as 709 for the same BGRA pixels. It used to be missing from
 * createEncoder()'s reuse test while send() stamped frame.colorSpace into VideoExtHeader, so a
 * same-size 601->709 transition advertised the new matrix over pixels made with the old one — the
 * declaration/payload family the lying Alpha flag belongs to.
 *
 * `valid` is false when no encoder exists; nothing may be published from an invalid identity.
 */
struct EncoderIdentity {
    int width = 0;
    int height = 0;
    int colorSpace = 0;
    int profile = 0;
    bool valid = false;
};

/**
 * Sender configuration
 */
struct SenderConfig {
    std::string name = "OMT Sender";
    Quality quality = Quality::Default;
    int port = Constants::NETWORK_PORT_START;
    int sendBufferSize = Constants::NETWORK_SEND_BUFFER;  // Official: 65536 (64KB)
    std::string senderInfoXml = "<OMTInfo ProductName=\"OMT Android Sender\" Manufacturer=\"Open Media Transport\" Version=\"1.0\" />";
};

/**
 * OMTSender - Main sender class
 * ...
 */
class Sender : public IChannelListener {
public:
    /**
     * Create sender with name and quality
     */
    explicit Sender(const SenderConfig& config = SenderConfig{});
    
    ~Sender();
    
    // Non-copyable
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;
    
    /**
     * Start accepting connections
     * @return true if started successfully
     */
    bool start();
    
    /**
     * Stop sender and disconnect all clients
     */
    void stop();
    
    /**
     * Send a video frame to all subscribed clients
     * @return encoded frame size in bytes, or 0 on failure
     */
    int send(const MediaFrame& frame);

    /**
     * Why the last send() failed, or empty if it did not. The counters say what state we are in;
     * this says why, so an operator can read the reason from the UI instead of attaching a
     * debugger to a phone. (Codex-style rule: Status is the current phase, LastError is the reason.)
     */
    std::string lastError() const {
        std::lock_guard<std::mutex> lock(errorMutex_);
        return lastError_;
    }
    /// Frames the sender refused to emit a payload for — an invalid frame, an encoder that could
    /// not be created, or an encode that produced nothing. Without this they left the camera and
    /// were counted nowhere.
    long long encodeFailures() const { return encodeFailures_.load(); }

    // Test seams for the host QA suite. Each of these gates is static, pure and side-effect-free,
    // so the suite can assert a clause on its own instead of inferring it from socket timing. No
    // production path calls them.
    static bool validateFrameForTest(const MediaFrame& frame, std::string& why) {
        return validateFrame(frame, why);
    }
    static size_t encodeBufferCapacityForTest(int width, int height) {
        return encodeBufferCapacity(width, height);
    }
    // qa/vmx-saveto-contract.cpp: the codec bounds its own output length, so the only way to watch
    // this belt is to assert the predicate directly.
    static bool encodedLenAcceptableForTest(int encodedLen, size_t capacity) {
        return encodedLenAcceptable(encodedLen, capacity);
    }
    // qa/wire-declaration.cpp: the wire declaration is a PURE function of (the frame, the encoder
    // that produced the payload). Driving it with a contradictory pair — a frame claiming one matrix
    // against an encoder created as the other — is the only way to watch the derivation itself,
    // because once the reuse key is correct the two can never disagree on the live path, and a gate
    // that cannot be made to fail is not evidence.
    static bool buildVideoHeadersForTest(const MediaFrame& frame, const EncoderIdentity& encoder,
                                        int encodedLen, FrameHeader& outHeader,
                                        VideoExtHeader& outExt, std::string& why) {
        return buildVideoHeaders(frame, encoder, encodedLen, outHeader, outExt, why);
    }
    static bool interlacedHeightSupportedForTest(int height) { return interlacedHeightSupported(height); }
    /// The identity of the encoder that is currently alive (valid=false when there is none). Read
    /// under encoderMutex_, so a test can ask what the next payload will really be made by.
    EncoderIdentity encoderIdentity() const;
    /// Drops across all subscribers since the previous call (per-second congestion view).
    int64_t getRecentDropsAndReset();
    
    /**
     * Send metadata to all connected clients
     */
    void sendMetadata(const std::string& xml);

    /**
     * Set sender information (product, manufacturer) for handshake.
     */
    void setSenderInfo(const std::string& productName, const std::string& manufacturer);
    
    /**
     * Get current connection count
     */
    int connectionCount() const;
    
    /**
     * Get aggregated statistics
     */
    SenderStats getStats();
    
    /**
     * Get the best suggested quality from all clients
     */
    Quality getBestSuggestedQuality() const;

    /**
     * Get the current VMX profile ID.
     */
    int getCurrentProfile() const { return currentProfile_; }
    
    /**
     * Check if any client has tally
     */
    bool hasTallyPreview() const;
    bool hasTallyProgram() const;
    
    // Callbacks
    using ConnectionCallback = std::function<void(Channel*)>;
    void setOnConnect(ConnectionCallback cb) { onConnect_ = cb; }
    void setOnDisconnect(ConnectionCallback cb) { onDisconnect_ = cb; }
    
    /**
     * Update the quality configuration and reset encoder.
     */
    void setQuality(Quality quality);

private:
    // Configuration
    SenderConfig config_;
    
    // Network
    int serverSocket_ = -1;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};
    
    // Channels
    // shared_ptr, not unique_ptr: a fan-out that releases the lock before touching a socket
    // must be able to hold a reference while it does so. With unique_ptr the only safe pattern
    // was to keep channelsMutex_ held across blocking socket writes, which let one stalled
    // subscriber stop the camera thread.
    std::vector<std::shared_ptr<Channel>> channels_;
    mutable std::mutex channelsMutex_;
    
    // VMX Encoder
    VMX_INSTANCE* vmxInstance_ = nullptr;
    int currentWidth_ = 0;
    int currentHeight_ = 0;
    // -1, not 0: 0 is VMX_COLORSPACE_UNDEFINED, a colour space a caller may legitimately ask for.
    // The reuse key compares this against the frame's colour space, and validateFrame() admits only
    // 0/601/709, so -1 can never be mistaken for a live encoder's setting.
    int currentColorSpace_ = -1;
    int currentProfile_ = 0;  // VMX_PROFILE stored as int
    std::vector<uint8_t> encodeBuffer_;
    // mutable so encoderIdentity() can be const: reading what the encoder currently IS must take the
    // same lock that writing it takes, and a test (or an operator polling state for a UI) should not
    // have to hold a Sender it cannot mutate to ask the question.
    mutable std::mutex encoderMutex_;
    
    // Statistics
    std::atomic<int64_t> totalEncodedBytes_{0};
    std::atomic<int> frameCount_{0};
    std::atomic<int64_t> encodeFailures_{0};
    std::atomic<int64_t> drops_baseline_{0};
    // Written by whichever thread failed a send, read by the UI's stats poll — so it needs its own
    // lock rather than riding on channelsMutex_, which the fan-out path holds while it queues.
    mutable std::mutex errorMutex_;
    std::string lastError_;
    void setLastError(const std::string& why) {
        std::lock_guard<std::mutex> lock(errorMutex_);
        lastError_ = why;
    }
    
    // Callbacks
    ConnectionCallback onConnect_;
    ConnectionCallback onDisconnect_;
    
    // Accept loop
    void acceptLoop();
    
    // Add/remove channels
    void addChannel(std::shared_ptr<Channel> channel);
    void removeChannel(Channel* channel);
    void cleanupDisconnected();
    
    // Encoder management
    // createEncoder reports why it could not create via `why` (send() owns lastError), and only
    // publishes the size cache once an encoder actually exists. Returns false on failure.
    bool createEncoder(int width, int height, int frameRate, int colorSpace, std::string& why);
    void destroyEncoder();
    // `used` receives the identity of the instance that really encoded this frame, read under the
    // same encoderMutex_ hold as the encode itself. send() publishes a declaration only from that,
    // never from the caller's struct, so the header cannot describe an encoder that did not make the
    // payload. Left null by the callers that do not need it.
    int encodeFrame(const MediaFrame& frame, EncoderIdentity* used = nullptr);

    // The one place the two wire headers are assembled. Pure, so the suite can feed it a frame and
    // an encoder identity that disagree and watch the refusal happen (see the test seam above).
    static bool buildVideoHeaders(const MediaFrame& frame, const EncoderIdentity& encoder,
                                 int encodedLen, FrameHeader& outHeader, VideoExtHeader& outExt,
                                 std::string& why);
    // VMX_ConfigureInterlaced (libvmx/src/vmxcodec.cpp:501-510) only switches to
    // VMX_FORMAT_INTERLACED at these heights; anywhere else it silently stays progressive, which is
    // why an Interlaced claim at another height is refused rather than advertised.
    static bool interlacedHeightSupported(int height);

    // Every field of MediaFrame is filled by hand by whoever calls omt_send() — the JNI bridge
    // copies Camera2 plane row-strides straight in — and encodeFrame() then hands frame.data to
    // VMX, which walks it using frame.stride for BOTH planes. A device whose UV row stride differs
    // from its Y row stride (ordinary ImageReader padding) therefore made the encoder read past
    // the end of the ImageReader allocation: a SIGSEGV in the camera process, on an unknown
    // device, with nothing in the log. This gate runs before the first allocation or VMX call.
    // (P0 #9 / P0 #16, from the desktop vendored sender.)
    static bool validateFrame(const MediaFrame& frame, std::string& why);
    static size_t encodeBufferCapacity(int width, int height);
    // P0 #10: what VMX_SaveTo() is allowed to hand back for it to be published as a frame at all.
    static bool encodedLenAcceptable(int encodedLen, size_t capacity);
    
    // Buffer size calculation
    static int calculateOptimalBuffer(int width, int height, int profile);

    // Congestion Control
    void checkCongestion();
    
    // IChannelListener implementation.
    //
    // protected, not private: these are the override points of a public interface. The host QA
    // scenario in qa/stop-deadlock.cpp has to observe the receiver thread ENTER onQualityChanged()
    // and then hand control back to the production implementation; with the override private, a
    // derived class has to re-implement the callback instead of testing it, and a re-implementation
    // is exactly how a deadlock test ends up proving nothing.
protected:
    void onDisconnected(Channel* channel) override;
    void onTallyChanged(Channel* channel) override;
    void onQualityChanged(Channel* channel) override;

private:
    // Profile selection
    int selectProfile(Quality quality) const;
};

} // namespace omt

#endif // OMT_SENDER_H
