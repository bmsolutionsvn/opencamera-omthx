/**
 * OMTConstants.h - OMT Constants and Enums
 * Port of libomtnet/src/OMTConstants.cs
 */

#ifndef OMT_CONSTANTS_H
#define OMT_CONSTANTS_H

#include <cstdint>

namespace omt {

// =============================================================
// Constants (from OMTConstants.cs)
// =============================================================
namespace Constants {
    constexpr int NETWORK_SEND_BUFFER = 65536;
    constexpr int NETWORK_RECEIVE_BUFFER = 8388608;  // 8MB
    // How long one frame may wait for a subscriber's TCP window before it is dropped and the
    // channel evicted. A live stream has no second chances: a frame that cannot leave in this
    // budget is already too late to display. Without a bound the sender thread sits in send()
    // until the kernel gives up on the peer (~11 min with only TCP_KEEPIDLE set), holding every
    // AsyncPool slot it owns so that subscriber's stream is dead meanwhile.
    constexpr int NETWORK_SEND_TIMEOUT_MS = 1000;
    /// How often a write in progress re-checks that the channel is still alive and still inside its
    /// deadline (P0 #7). This is the poll() slice; NETWORK_SEND_TIMEOUT_MS is the bound on the
    /// WHOLE write. They were the same number when the timeout only bounded each poll(), which is
    /// what let a peer that drains a few bytes every second hold a 1.5 MB frame in flight forever:
    /// poll kept succeeding, the "timeout" kept resetting, the sender thread never returned to the
    /// queue and the AsyncPool slot it owns never came back.
    constexpr int NETWORK_SEND_POLL_SLICE_MS = 100;
    /// How long sendMetadataSync waits for the per-channel socket-write lock before it gives up on
    /// the subscriber (P0 #7). The invariant that matters: this must exceed NETWORK_SEND_TIMEOUT_MS
    /// with room, because the async video writer holds that lock for the whole of one write — a
    /// budget below the write's own bound declares a subscriber dead for a video frame that was
    /// still perfectly inside its deadline (the first draft of qa/socket-write-order.cpp S1 did
    /// exactly that at 2000 ms against a 5000 ms write). Desktop's copy uses 6000 ms against its
    /// 5000 ms write; there the metadata writer also holds the sender-wide channelsMutex_ while it
    /// waits, here Sender::sendMetadata snapshots the channel list and releases that lock first, so
    /// this budget only ever delays the thread that called it.
    constexpr int NETWORK_METADATA_LOCK_BUDGET_MS = 3000;
    constexpr int NETWORK_KEEPALIVE_IDLE_S = 5;    // first probe (matches libomtnet)
    constexpr int NETWORK_KEEPALIVE_INTVL_S = 2;   // repeat probes: unset kernel default is 75s
    constexpr int NETWORK_KEEPALIVE_CNT = 3;       // failed probes before the peer is declared gone

    constexpr int NETWORK_ASYNC_COUNT = 4;           // Official: 4 (matches libomtnet)
    constexpr int NETWORK_ASYNC_BUFFER_AV = 1048576; // Official: 1MB (matches libomtnet)
    constexpr int NETWORK_ASYNC_COUNT_META_ONLY = 64;
    constexpr int NETWORK_ASYNC_BUFFER_META_ONLY = 1024;
    
    constexpr int VIDEO_FRAME_POOL_COUNT = 4;
    constexpr int VIDEO_MIN_SIZE = 65536;
    constexpr int VIDEO_MAX_SIZE = 10485760;         // 10MB max frame
    
    constexpr int AUDIO_FRAME_POOL_COUNT = 10;
    constexpr int AUDIO_MIN_SIZE = 65536;
    constexpr int AUDIO_MAX_SIZE = 1048576;
    constexpr int AUDIO_SAMPLE_SIZE = 4;
    
    constexpr int NETWORK_PORT_START = 6400;
    constexpr int NETWORK_PORT_END = 6600;
    constexpr int DISCOVERY_SERVER_PORT = 6399;
    
    constexpr int METADATA_MAX_COUNT = 60;
    constexpr int METADATA_FRAME_SIZE = 65536;
    
    constexpr const char* URL_PREFIX = "omt://";
}

// =============================================================
// Metadata Constants (from OMTMetadata.cs)
// =============================================================
namespace MetadataConstants {
    // Subscription commands
    constexpr const char* CHANNEL_SUBSCRIBE_VIDEO = "<OMTSubscribe Video=\"true\" />";
    constexpr const char* CHANNEL_SUBSCRIBE_AUDIO = "<OMTSubscribe Audio=\"true\" />";
    constexpr const char* CHANNEL_SUBSCRIBE_METADATA = "<OMTSubscribe Metadata=\"true\" />";
    
    // Preview mode commands
    constexpr const char* CHANNEL_PREVIEW_VIDEO_ON = "<OMTSettings Preview=\"true\" />";
    constexpr const char* CHANNEL_PREVIEW_VIDEO_OFF = "<OMTSettings Preview=\"false\" />";
    
    // Tally commands
    constexpr const char* TALLY_PREVIEW = "<OMTTally Preview=\"true\" Program==\"false\" />";
    constexpr const char* TALLY_PROGRAM = "<OMTTally Preview=\"false\" Program==\"true\" />";
    constexpr const char* TALLY_PREVIEWPROGRAM = "<OMTTally Preview=\"true\" Program==\"true\" />";
    constexpr const char* TALLY_NONE = "<OMTTally Preview=\"false\" Program==\"false\" />";
    
    // Quality suggestion prefix
    constexpr const char* SUGGESTED_QUALITY_PREFIX = "<OMTSettings Quality=";
    
    // Sender info
    constexpr const char* SENDER_INFO_PREFIX = "<OMTInfo";
    constexpr const char* REDIRECT_PREFIX = "<OMTRedirect";
}

// =============================================================
// Enums
// =============================================================

// Subscription flags (matching OMTFrameType)
enum class Subscription : uint8_t {
    None = 0,
    Video = 1,
    Audio = 2,
    Metadata = 4
};

// Bitwise OR operator for Subscription
inline Subscription operator|(Subscription a, Subscription b) {
    return static_cast<Subscription>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline Subscription& operator|=(Subscription& a, Subscription b) {
    a = a | b;
    return a;
}

inline bool hasFlag(Subscription sub, Subscription flag) {
    return (static_cast<uint8_t>(sub) & static_cast<uint8_t>(flag)) != 0;
}

// Quality levels
enum class Quality {
    Default = 0,
    Low = 1,
    Medium = 2,
    High = 3
};

// Frame types
enum class FrameType : uint8_t {
    Metadata = 1,
    Video = 2,
    Audio = 3
};

// Event types (for callbacks)
enum class EventType {
    None,
    Connected,
    Disconnected,
    TallyChanged,
    RedirectChanged,
    QualityChanged
};

} // namespace omt

#endif // OMT_CONSTANTS_H
