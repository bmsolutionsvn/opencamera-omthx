# OMT-HX sending from Open Camera — design

Date: 2026-10-22
Repo: `bmsolutionsvn/opencamera-omthx` (this one). Receiver side already exists in `bmsolutionsvn/OMT-HX`.
Owner: bmsolutionsvn.

## 1. What we want

The phone sends its camera to a PC over OMT-HX (HEVC or H.264 inside an ordinary OMT stream,
the "High Efficiency" mode that `OMT-HX` and the OBS `omtplugin` already speak), while the
existing Full (VMX) mode stays available. Owner decision: **HX sits beside Full as a Mode
setting**, not a replacement.

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

## 4. The change, in four parts

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

`nativeSendCoded(byte[] data, int length, int codec, long ptsUs)` fills an `OMTMediaFrame`
(Type=Video, Codec=tag, Data, DataLength, Width/Height/FrameRate from the existing globals,
Stride=Width, ColorSpace, Flags=0, Timestamp=ptsUs) and calls `omt_send` on `g_sender` under
`g_senderMutex`. The existing init/announce/tally/stats calls are reused untouched.
`frameRateD == 0` is refused (the fork's own rule).

### 4.3 Java: MediaCodec encoder

New file `app/src/main/java/net/sourceforge/opencamera/OMTHXEncoder.java`.

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
  `startStreaming`, and show the mode in the existing status text (`omt_status_text`).

### 4.5 Bitrate (a builder call, tunable)

There is no encoder-quality knob in HX — only a bitrate. Reuse the existing OMT quality preset
so the UI does not grow another control:

```
bitrate = base[quality] * (width * height * fps) / (1920 * 1080 * 30)
base: Default/Low 8 Mbps, Medium 20 Mbps, High 40 Mbps   (HEVC, CBR)
H.264: x1.6
```

Clamped to 2..100 Mbps. One constant table in `OMTHXEncoder`, easy to change after the first
measurement.

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
   sends a coded frame with the HEVC tag, and asserts the received header carries the tag and
   the payload is byte-identical. Plus the harness's usual mutant: with `ext.codec` forced to
   VMX1 the scenario must fail, or it proves nothing.
2. **Android build:** needs the SDK + NDK 27 + cmake 3.22 (`AGENT-HANDOFF.md`), then
   `./gradlew assembleDebug`. Not possible on the current machine yet — no SDK/NDK installed.
3. **On a phone, against a PC:** install, set Mode = HX, start streaming; on the PC
   `omthx-recv --source "<phone> (...)"` shows HEVC frames, and the OBS plugin shows a picture
   and a plausible bitrate. Then switch back to Full and confirm the VMX stream still plays.
4. Also confirm the negative: a Full-only receiver (plain OMT viewer) shows no picture in HX
   mode. That is expected, and the reason Mode stays explicit.

## 7. Limits and risks

- Device HEVC encoders vary. Fallback to H.264 is automatic, but 4K60 HEVC is not guaranteed on
  older phones.
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
- Whether the Mode list should hide the Quality list in HX mode (cosmetic).