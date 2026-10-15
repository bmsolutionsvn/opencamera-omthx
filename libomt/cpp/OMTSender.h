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

#include "OMTConstants.h"
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
    /// Frames the encoder refused. Without this they left the camera and were counted nowhere.
    long long encodeFailures() const { return encodeFailures_.load(); }
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
    int currentProfile_ = 0;  // VMX_PROFILE stored as int
    std::vector<uint8_t> encodeBuffer_;
    std::mutex encoderMutex_;
    
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
    void createEncoder(int width, int height, int frameRate, int colorSpace);
    void destroyEncoder();
    int encodeFrame(const MediaFrame& frame);
    
    // Buffer size calculation
    static int calculateOptimalBuffer(int width, int height, int profile);

    // Congestion Control
    void checkCongestion();
    
    // IChannelListener implementation
    void onDisconnected(Channel* channel) override;
    void onTallyChanged(Channel* channel) override;
    void onQualityChanged(Channel* channel) override;
    
    // Profile selection
    int selectProfile(Quality quality) const;
};

} // namespace omt

#endif // OMT_SENDER_H
