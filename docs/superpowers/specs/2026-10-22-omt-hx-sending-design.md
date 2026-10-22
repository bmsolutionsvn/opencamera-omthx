# OMT-HX sending from Open Camera — design

Date: 2026-10-22
Repo: `bmsolutionsvn/opencamera-omthx` (this one). Receiver side already exists in `bmsolutionsvn/OMT-HX`.
Owner: bmsolutionsvn.

## 1. What we want

The phone sends its camera to a PC over OMT-HX (HEVC or H.264 inside an ordinary OMT stream,
the "High Efficiency" mode that `OMT-HX` and the OBS `omtplugin` already speak), while the
existing Full (VMX) mode stays available. Owner decision: **HX sits beside Full as a Mode
setting**, not a replacement. Resolution and frame rate follow the device, from small sizes up
to 4K120 — nothing is pinned to 1080p30.

The PC end is done and verified: `omtplugin` in OBS and `omthx-recv` both receive HX today.
This design is only about the phone sending it.

## 2. What "HX" is on the wire

An HX stream is a normal OMT stream. The only difference in each video frame is:

- `VideoExtHeader.codec` is the FourCC `'HEVC'` (0x43564548) or `'H264'` (0x34363248)
  instead of `'VMX1'`;
- the payload is one Annex-B access unit, already encoded, not a VMX picture;
- the optional `FrameMetadata` sequence number (used to show drops) may be absent — the
  receiver treats a missing sequence as "no information", not as an error
  (`OMT-HX/src/link.cpp`, `metadataOf()`).

Keyframes are not flagged in the header; HX receivers detect them by parsing the NAL units.

Consequences that shape the whole design:

- **No OMT-HX library is needed on the phone.** OMT-HX's `libomt` is C# compiled with .NET
  Native AOT, which has no Android target. The camera fork already carries a hand-written C++
  port of libomt, and HX only needs that port to stop refusing compressed frames.
- **No FFmpeg, no x264/x265 on the phone.** Android's MediaCodec hardware encoder produces the
  access units. The phone never decodes.

## 3. How it works today

```
Camera2 session ──▶ ImageReader(YUV_420_888)        OMTStreamingManager.java
                        │ onImageAvailable
                        ▼
                  OMTSender (JNI)                   app/src/main/cpp/OMTBridge.cpp
                        ▼
                  libomt C++ sender                 libomt/cpp/OMTSender.cpp
                        │ VMX encodes the picture
                        ▼
                  channel per subscriber            libomt/cpp/OMTChannel.cpp
                        ▼
                  mDNS _omt._tcp:6400               NsdManager, OMTStreamingManager
```

- `OMTStreamingManager.getStreamingSurface()` returns the ImageReader surface;
  `MainActivity` hands it to `Preview.setOmtStreamingSurface()`, which reopens the camera with
  it. That is the one integration point for a new video source.
- `OMTBridge.cpp::nativeInit` creates the sender with `omt_send_create(name, quality)` and sets
  device info; announcements, port, tally and stats are all independent of the codec.
- `libomt/cpp/OMTSender.cpp::send()` validates the frame, creates/updates a VMX encoder,
  encodes, builds the wire headers, and hands the payload to every subscribed channel.
- `validateFrame()` refuses any codec other than BGRA/UYVY/YUY2/NV12 ("this sender encodes
  BGRA/UYVY/YUY2/NV12 only"). This is the gate HX has to pass.

## 4. The change

### 4.1 libomt C++ sender: pass coded frames through

Files: `libomt/cpp/OMTProtocol.h` (codec enum), `libomt/cpp/OMTSender.cpp`,
`libomt/libomt.h` (same enum for the C API).

- Add `HEVC` and `H264` to the `Codec` enum in `OMTProtocol.h` and `libomt.h`, with the FourCC
  values above.
- In `send()`: when `frame.codec` is HEVC/H264, skip `createEncoder()`/`encodeFrame()`, use
  `frame.data`/`frame.dataLength` as the payload, and build the headers with
  `ext.codec = frame.codec`. Everything else (channel fan-out, drop-oldest, stats, tally)
  is unchanged.
- Coded frames get their own validation: width/height > 0 and even, `dataLength > 0`,
  `frameRateN/D > 0`, finite positive aspect ratio, colour space 0/601/709, and the same flag
  refusal as today for Alpha / PreMultiplied / HighBitDepth / Interlaced (the phone sends none
  of those in v1). `stride` is ignored for coded frames.
- Colour space and flags are what the caller declares; nothing re-derives them from the payload.

The receiver needs no change: it already switches on the codec tag.

### 4.2 JNI: one new entry point

File: `app/src/main/cpp/OMTBridge.cpp`.

`nativeSendCoded(byte[] data, int length, int codec, int width, int height, int fpsN, int fpsD,
long ptsNs)` fills an `OMTMediaFrame` (Type=Video, Codec=tag, Data, DataLength,
Width/Height/FrameRate **from the argument, not from the globals**, Stride=Width,
AspectRatio=width/(float)height, ColorSpace, Flags=0, Timestamp=ptsNs/100) and calls `omt_send`
on `g_sender` under `g_senderMutex`.

Two details that are easy to get wrong here:

- **Geometry travels with each frame.** The bridge's `g_width`/`g_height`/`g_frameRateN` are set
  only by `nativeInit`, which runs at announcement time and is skipped while already announcing.
  The stream is started later from `preview.getVideoProfile()`, so after a resolution or fps
  change the globals can be stale while the access units are the new size. OMT-HX compares the
  decoded picture against the header and treats a mismatch as a decode error (silent black
  stream), so the coded path must never take geometry from the globals.
- **The wire timestamp is 100 ns ticks**, not microseconds: `FrameHeader.timestamp` is in
  100 ns units and the existing `postFrame()` writes `ns/100`. MediaCodec reports
  `presentationTimeUs`, so the JVM side passes `presentationTimeUs * 1000` and the JNI divides
  by 100. Passing microseconds straight through makes every frame look 10× too close and breaks
  the receiver's gap detection.

The existing init/announce/tally/stats calls are reused untouched. `frameRateD == 0` is refused
(the fork's own rule).

### 4.3 Java: MediaCodec encoder

New file `app/src/main/java/net/sourceforge/opencamera/OMTHXEncoder.java`.

**Geometry: whatever the device and the profile allow, up to 4K120 — nothing is pinned to
1080p30.** The target comes from the video profile the app already uses
(`preview.getVideoProfile()` in `MainActivity`): device-derived, user-selectable, and already
carries high-speed rates. The encoder then clamps it, in this order:

1. `MediaCodecInfo.VideoCapabilities.areSizeAndRateSupported(w, h, fps)` — if yes, use it.
2. Otherwise the highest rate the codec supports at (w, h) (`getSupportedFrameRatesFor`).
3. Otherwise the largest size the codec supports at the requested rate
   (`getSupportedWidths()`/`getSupportedHeights()`), then that size at its own highest rate.
4. The size must also be a camera output size for a surface target
   (`StreamConfigurationMap.getOutputSizes(SurfaceTexture.class)`); otherwise step down.
5. HEVC first; the same clamping on H.264 if the device has no HEVC encoder; if neither accepts
   even a modest size, refuse the start with a message and leave Full available.

The geometry actually chosen is passed to `nativeSendCoded` per frame and shown in the status
text, so a clamped stream is visible instead of silently wrong. Rates above 60 fps need a
Camera2 constrained high-speed session; the app already picks high-speed profiles for those
rates, and a MediaCodec input surface is one of the targets such a session allows. If the device
refuses the high-speed session with the encoder surface, fall back to the highest non-high-speed
rate and say so.

- `MediaCodec.createEncoderByType("video/hevc")`, falling back to `"video/avc"` when the device
  has no HEVC encoder (`MediaCodecList` check before creating).
- `COLOR_FormatSurface`, profile Main, 8-bit 4:2:0, `KEY_BITRATE_MODE = CBR`,
  `KEY_I_FRAME_INTERVAL = 1` s, `KEY_PREPEND_HEADER_TO_SYNC_FRAMES = 1` so every IDR carries
  SPS/PPS (the receiver cannot start mid-stream otherwise).
- `createInputSurface()` is what `getStreamingSurface()` returns in HX mode.
- A drain thread reads output buffers with `getOutputBuffer`/`dequeueOutputBuffer`; buffers with
  `BUFFER_FLAG_CODEC_CONFIG` are held and prepended to the next keyframe (belt and braces with
  the key above); every other buffer is passed to `nativeSendCoded` with
  `keyframe = (flags & BUFFER_FLAG_KEY_FRAME) != 0` and the buffer's presentation time.
- `stop()` releases the codec and joins the thread.

### 4.4 Settings and wiring

- `PreferenceKeys`: `OmtStreamingModeKey = "preference_omt_streaming_mode"` (values `full`,
  `hx`), `OmtHxCodecKey = "preference_omt_streaming_hx_codec"` (values `hevc`, `h264`).
- `res/xml/preferences_sub_video.xml`: a Mode list in the existing OMT category, and a Codec
  list that depends on Mode = HX. The existing Quality list keeps its meaning for Full; for HX
  it becomes a bitrate preset (see 4.5).
- `OMTStreamingManager`: `startStreaming(..., mode, codec)`. In HX mode create `OMTHXEncoder`
  instead of the ImageReader and return the encoder surface; in Full mode change nothing.
  `stopStreaming`/`stopAll`/`cleanup` stop whichever is active. The existing
  `reopenCamera()` path already handles swapping the surface in and out.
- `MainActivity`: read the two preferences where it already reads quality/name, pass them to
  `startStreaming`, and show the mode and the chosen geometry in the existing status text
  (`omt_status_text`).

**Stop ordering matters.** `Preview.setOmtStreamingSurface(null)` reopens the camera
asynchronously, and `stopOmtStreaming()` calls `stopStreaming()` immediately after — a naive
release would stop/release the MediaCodec input surface while Camera2 still targets it: the
abandoned-BufferQueue crash this repo already documents for the ImageReader. The stop is
sequenced behind camera detach instead: `Preview` gains
`setOmtStreamingSurface(Surface, Runnable afterDetach)` (or an equivalent callback from
`CameraController2` when the reopened session is configured without the OMT surface), and
`stopOmtStreaming()` passes `stopStreaming` as that runnable. The drain thread is joined before
the sender is torn down; `stopAll()` on activity destroy goes through the same path after the
camera is closed. File list grows by `Preview.java` and `CameraController2.java`.

### 4.5 Bitrate (a builder call, tunable)

There is no encoder-quality knob in HX — only a bitrate. Reuse the existing OMT quality preset
so the UI does not grow another control, but scale by pixels *and* frames, because a 1080p30
table would starve 4K120:

```
bitrate = width * height * fps * bpp[quality]      (HEVC, CBR)
bpp: Low 0.06, Medium (and Default) 0.10, High 0.15
H.264: x1.6
clamped to 4..250 Mbps
```

Examples: 1080p30 = 3.7 / 6.2 / 9.3 Mbps; 4K60 = 30 / 50 / 75; 4K120 = 60 / 100 / 150.
One constant table in `OMTHXEncoder`, easy to change after the first measurement.

## 5. Non-goals for v1

- No audio (the app sends video only today).
- No alpha (HX alpha needs a separate VMX plane per frame; the camera has none).
- No HX preview/multiview stream (a second, small encoding; receivers that ask for one get
  program only).
- No reaction to receiver keyframe requests: an IDR every second covers a lost frame within a
  second. `shortcut:` in `OMTHXEncoder` when this proves too slow for a flaky link.
- No 10-bit/HDR, no interlaced.
- Full mode is untouched, including its VMX quality changes while streaming.

## 6. How this gets verified

1. **Host, no device:** a new scenario in the existing harness (`qa/build-host.sh`,
   `qa/*.cpp`, real sockets, ASan/UBSan) that starts the sender, connects a fake subscriber,
   and sends two coded frames with the HEVC tag. It asserts the received header carries the tag,
   the payload is byte-identical, the header geometry equals the per-frame geometry (with a size
   change between the two frames), and the timestamp delta is the frame period in 100 ns ticks.
   Mutants, one per assertion: `ext.codec` forced to VMX1, geometry taken from the sender's
   init-time values, timestamps passed as microseconds. Each mutant must fail, or the scenario
   proves nothing.
2. **Android build:** needs the SDK + NDK 27 + cmake 3.22 (`AGENT-HANDOFF.md`), then
   `./gradlew assembleDebug`. Not possible on the current machine yet — no SDK/NDK installed.
3. **On a phone, against a PC:** install, set Mode = HX, start streaming; on the PC
   `omthx-recv --source "<phone> (...)"` shows HEVC frames, and the OBS plugin shows a picture
   and a plausible bitrate. Then switch back to Full and confirm the VMX stream still plays.
4. Also confirm the negative: a Full-only receiver (plain OMT viewer) shows no picture in HX
   mode. That is expected, and the reason Mode stays explicit.

## 7. Limits and risks

- Device HEVC encoders vary, and so do camera high-speed sessions. The clamp in 4.3 turns an
  unsupported request into a smaller or slower stream that is visible in the status text; the
  H.264 fallback is automatic. 4K60 is common, 4K120 is not.
- 4K120 at ~150 Mbps needs Wi-Fi 6 or wired; on ordinary 5 GHz Wi-Fi the link, not the encoder,
  becomes the limit.
- Encoder capabilities can shrink under thermal load. v1 does not re-clamp live; restart the
  stream if the device throttles.
- The phone announces on port 6400 with `NsdManager`, unchanged. If the PC cannot see the phone
  in HX mode but can in Full mode, the fault is in the codec tag, not discovery.
- MediaCodec's surface encoder applies the camera's sensor orientation; the current YUV path has
  the same property, so this is not a regression, but the picture may arrive rotated on a phone
  held in portrait.
- The camera pipeline now feeds the encoder surface instead of an ImageReader. Preview, photo
  and video capture are untouched, but the extra surface changes session configuration and must
  be checked on the actual device (this is the part most likely to need a fix on first run).
- Android's CBR is nominal; expect the measured bitrate to wander.

## 8. What the builder decides

- Exact MediaCodec keys/levels and whether `KEY_PREPEND_HEADER_TO_SYNC_FRAMES` is enough on the
  target device, or CSD prepending is required.
- Whether the coded validation lives in `validateFrame()` or in a separate `validateCoded()`.
- Threading for `nativeSendCoded` (keep the existing mutex, or give coded sends their own).
- Where the detach callback is carried (`Preview` method parameter vs `CameraController2`
  listener) — the ordering requirement is fixed, the wiring is not.
- The exact order of the capability clamp when both size and rate have to give.
- Whether the Mode list should hide the Quality list in HX mode (cosmetic).