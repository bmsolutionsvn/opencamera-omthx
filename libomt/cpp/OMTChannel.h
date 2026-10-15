/**
 * OMTChannel.h - Client Connection Channel
 * Port of libomtnet/src/OMTChannel.cs
 * 
 * Represents a single client connection with:
 * - Async send via buffer pool
 * - Receiver thread for metadata processing
 * - Subscription, tally, and quality state
 */

#ifndef OMT_CHANNEL_H
#define OMT_CHANNEL_H

#include <atomic>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>
#include <functional>
#include <arpa/inet.h>

#include "OMTConstants.h"
#include "OMTProtocol.h"
#include "OMTAsyncPool.h"

namespace omt {

// Forward declarations
class Sender;

/**
 * Channel statistics
 */
struct ChannelStats {
    int64_t framesSent = 0;
    int64_t framesDropped = 0;
    int64_t bytesSent = 0;
    int64_t bytesReceived = 0;
};

/**
 * Callback interface for channel events
 */
class IChannelListener {
public:
    virtual ~IChannelListener() = default;
    virtual void onDisconnected(class Channel* channel) = 0;
    virtual void onTallyChanged(class Channel* channel) = 0;
    virtual void onQualityChanged(class Channel* channel) = 0;
};

/**
 * OMTChannel - Single client connection
 * 
 * Design Pattern: Active Object
 * - Sender thread handles async sends from queue
 * - Receiver thread handles incoming metadata
 * - Thread-safe state management via atomics
 */
class Channel {
public:
    Channel(int socketFd, struct sockaddr_in address, 
            int sendBufferSize = Constants::NETWORK_SEND_BUFFER,  // Official: 65536 (64KB)
            IChannelListener* listener = nullptr);
    
    ~Channel();
    
    // Non-copyable
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    
    // Connection state
    bool isConnected() const { return connected_.load(); }
    int64_t framesDropped() const { return framesDropped_.load(); }
    int64_t framesSent() const { return framesSent_.load(); }
    int64_t staleEvicted() const { return staleEvicted_.load(); }
    int64_t tornSends() const { return tornSends_.load(); }
    void disconnect();
    
    // Subscriptions
    Subscription getSubscriptions() const { return subscriptions_.load(); }
    bool isVideoSubscribed() const { return hasFlag(subscriptions_.load(), Subscription::Video); }
    bool isAudioSubscribed() const { return hasFlag(subscriptions_.load(), Subscription::Audio); }
    bool isMetadataSubscribed() const { return hasFlag(subscriptions_.load(), Subscription::Metadata); }
    
    // Quality negotiation
    Quality getSuggestedQuality() const { return suggestedQuality_.load(); }
    
    // Tally state
    bool isTallyPreview() const { return tallyPreview_.load(); }
    bool isTallyProgram() const { return tallyProgram_.load(); }
    
    // Preview mode
    bool isPreviewMode() const { return previewMode_.load(); }
    
    // Send methods
    int sendAsync(const void* headerData, size_t headerLen,
                  const void* extHeaderData, size_t extHeaderLen,
                  const void* payloadData, size_t payloadLen);
    
    bool sendMetadataSync(const char* xml);
    
    // Statistics
    ChannelStats getStats() const;
    
    // Endpoint info
    std::string getRemoteAddress() const;
    int getRemotePort() const;

private:
    // Socket
    int socketFd_;
    struct sockaddr_in address_;
    std::atomic<bool> connected_{true};
    std::atomic<bool> running_{true};
    
    // Async send infrastructure
    std::unique_ptr<AsyncPool> sendPool_;
    std::queue<AsyncBuffer*> pendingQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::thread senderThread_;
    std::thread receiverThread_;

    /// Serialises EVERY write to socketFd_ — the video frames drained by senderLoop and the
    /// synchronous metadata of sendMetadataSync — for the whole of each write, not just per send()
    /// call. Without it a metadata broadcast (JNI/UI thread, or the accept loop at handshake) can
    /// interleave its bytes with a half-written video record on the single TCP stream, and the
    /// subscriber's stream is mis-framed permanently: every record after the tear is parsed off a
    /// shifted offset, which OBS reports as a stream that dies and never recovers while the socket
    /// itself stays up.
    ///
    /// Plain std::mutex, not std::timed_mutex: the metadata writer bounds acquisition with a
    /// try_lock() loop, and std::mutex::try_lock is the path ThreadSanitizer models correctly
    /// (std::timed_mutex's timed path produces spurious "unlock of an unlocked mutex" reports).
    ///
    /// Rule: sendWithDeadline() must only ever be called with this mutex held, so that the
    /// connected_ = false that a failed write sets cannot be observed by a second writer in the
    /// gap between the torn frame and the channel being marked dead.
    std::mutex sendSocketMutex_;
    
    // Subscriptions and state
    std::atomic<Subscription> subscriptions_{Subscription::None};
    std::atomic<Quality> suggestedQuality_{Quality::Default};
    std::atomic<bool> tallyPreview_{false};
    std::atomic<bool> tallyProgram_{false};
    std::atomic<bool> previewMode_{false};
    
    // Statistics
    std::atomic<int64_t> framesSent_{0};
    std::atomic<int64_t> framesDropped_{0};
    /// Queued frames evicted to make room for a newer one (see sendAsync drop-oldest).
    std::atomic<int64_t> staleEvicted_{0};
    /// Writes that began and failed partway — a distinct failure mode from a clean drop.
    std::atomic<int64_t> tornSends_{0};
    std::atomic<int64_t> bytesSent_{0};
    std::atomic<int64_t> bytesReceived_{0};
    
    // Listener
    IChannelListener* listener_;
    
    // Thread functions
    void senderLoop();
    void receiverLoop();
    
    // Internal send
    bool sendAllBlocking(const uint8_t* data, size_t length);
    
    // Metadata processing (port of ProcessMetadata)
    void processMetadata(const std::string& xml);
    
    // Socket helpers
    void setSendBuffer(int size);
    void closeSocket();
    /// Writes the whole buffer or gives up after timeoutMs — timeoutMs bounds the ENTIRE write,
    /// not one poll() — and stops as soon as the channel is no longer alive. The fd is passed in,
    /// read once by a caller that already holds sendSocketMutex_, rather than re-read from the
    /// member on every iteration. Callers MUST hold sendSocketMutex_.
    bool sendWithDeadline(int fd, const uint8_t* data, size_t length, int timeoutMs);
};

} // namespace omt

#endif // OMT_CHANNEL_H
