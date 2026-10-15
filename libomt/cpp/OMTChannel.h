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
 * Why a channel stopped carrying traffic — the *reason*, not just the phase.
 *
 * `connected_ == false` on its own is unfalsifiable: a reviewer, an operator reading logcat, and
 * a test asserting on teardown all need to know WHICH failure produced it. Before this existed a
 * peer that sent dataLength = -16 and a peer that simply hung up were the same observable state,
 * so the test could only assert "something happened" — and the spin/hang defect sat next to the
 * EOF path for a whole audit cycle unnoticed.
 *
 * Values are append-only: they are part of what the host QA asserts on, so renumbering existing
 * entries would silently retarget those assertions.
 */
enum class CloseReason : uint8_t {
    None = 0,
    PeerClosed,          ///< recv() returned 0: the peer closed its write half
    RecvError,           ///< recv() failed with something other than EAGAIN
    PollError,           ///< poll() failed with something other than EINTR
    SendFailed,          ///< a write could not be completed inside its deadline
    BadVersion,          ///< frame header version != 1
    BadFrameType,        ///< a sender only accepts Metadata frames from a peer
    NegativeDataLength,  ///< dataLength < 0 (signed int32 straight off the wire)
    OversizedDataLength, ///< dataLength larger than any conforming control frame
    MetadataLenTooBig,   ///< metadataLength does not fit inside dataLength
    RecvBufferFull,      ///< bytes accumulated that no accepted frame can explain
};

/// Stable static strings: a log line or a UI toast may outlive the Channel, so the reason must
/// never point into freed memory.
inline const char* closeReasonToString(CloseReason r) {
    switch (r) {
        case CloseReason::PeerClosed:         return "peer closed";
        case CloseReason::RecvError:          return "receiver: recv error";
        case CloseReason::PollError:          return "receiver: poll error";
        case CloseReason::SendFailed:         return "sender: write failed or timed out";
        case CloseReason::BadVersion:         return "receiver: bad frame header version";
        case CloseReason::BadFrameType:       return "receiver: unexpected peer frameType";
        case CloseReason::NegativeDataLength: return "receiver: negative dataLength";
        case CloseReason::OversizedDataLength:return "receiver: oversized dataLength";
        case CloseReason::MetadataLenTooBig:  return "receiver: metadataLength larger than dataLength";
        case CloseReason::RecvBufferFull:     return "receiver: receive buffer full without a consumable frame";
        case CloseReason::None:               break;
    }
    return "none";
}

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

    /// Why this channel stopped carrying traffic. `None` until something names it.
    CloseReason closeReason() const { return static_cast<CloseReason>(closeReason_.load()); }
    const char* closeReasonName() const { return closeReasonToString(closeReason()); }
    /// First writer wins: the reason the channel *died* is the one worth reporting, and the
    /// bookkeeping that follows (the sender thread noticing a second later, the EOF that trails a
    /// shutdown) must not overwrite it.
    void noteCloseReason(CloseReason r) {
        uint8_t expected = static_cast<uint8_t>(CloseReason::None);
        closeReason_.compare_exchange_strong(expected, static_cast<uint8_t>(r));
    }
    
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
    /// CloseReason as a raw byte so it can be published from any thread without a lock; the
    /// accessors above are the only readers, so the encoding never escapes the class.
    std::atomic<uint8_t> closeReason_{0};
    
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
