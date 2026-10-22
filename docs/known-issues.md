# Known issues (found during the 2026-10-15 audit, deliberately not fixed in that pass)

Each of these was verified against the code with file:line evidence. They are
listed rather than silently patched because several change app behaviour or
require a device to validate, and one is upstream Open Camera code rather than
OMT code.

## OMT HX (v1 limits, by design)

- **No audio.** The app has never sent audio; HX is video only.
- **No alpha, no 10-bit.** The coded path is 8-bit 4:2:0 progressive, and the
  sender refuses an access unit that claims otherwise.
- **No incoming keyframe request handling.** A lost frame is recovered by the
  next 1 s IDR rather than an on-demand keyframe. On a flaky link, lower the
  frame rate or use Full mode if that recovery window is too long.
- **No preview/multiview stream.** A receiver asking for one gets program only.
- **No live re-clamp.** Encoder capabilities can shrink under thermal load; the
  stream keeps the geometry chosen at start. Restart streaming if the device
  throttles.
- **The announcement still advertises the video profile's geometry**, which can
  differ from the clamped HX geometry (mDNS metadata only; the frames carry
  their own size).

## Lifecycle / streaming

1. **No foreground service behind the stream.**
   `AndroidManifest.xml` declares CAMERA/RECORD_AUDIO/INTERNET/WIFI/WAKE_LOCK and
   there is no `startForeground` anywhere in `app/src/main`. The sender therefore
   runs as an invisible background process holding an untimed `PARTIAL_WAKE_LOCK`
   (`OMTStreamingManager`): OEM background killers may freeze it mid-stream with
   no cleanup hook, and the user gets no notification with a stop action.
   *Fix direction:* a started foreground service (`camera`/`mediaProjection`
   type) owning `OMTStreamingManager`, with a stop action; wake lock only while
   actually streaming, and with a timeout.

2. **Changing the stream name or disabling OMT while announcing does nothing.**
   The advertised name is captured once at announcement time;
   `onSharedPreferenceChanged` handles only the quality key. Result: the old
   record lives forever, and after a process restart you can get two `_omt._tcp`
   records for one phone — one of them pointing at a dead port — which can then
   fail the duplicate-name probe.
   *Fix direction:* handle the name and enabled keys: `stopAll()` then
   `startAnnouncing()` (or a full stop when disabled).

3. **Activity recreation shares one native sender.**
   `OMTBridge.cpp` holds `static omt_send_t* g_sender`; `OMTSender.java`'s
   `isInitialized` is per-instance and `MainActivity`'s
   `omtAnnouncementStarted` is an activity field reset by `recreate()`. Night
   mode / locale changes not covered by `android:configChanges`, or "don't keep
   activities", produce two managers over one sender — whoever cleans up last
   closes port 6400 under a registration that is still answering queries.
   *Fix direction:* key the bridge by a returned handle and validate it on every
   call, or make the manager a process singleton with refcounting.

4. **`ImageReader` is closed while the capture session still targets its surface.**
   `setOmtStreamingSurface(null)` triggers an asynchronous `reopenCamera()`, and
   `stopStreaming()` closes the reader immediately — the still-configured session
   keeps queueing buffers to a dead Surface ("BufferQueue has been abandoned"),
   which lands as a camera error and then as issue 5.
   *Fix direction:* close the reader after the replacement session configures, or
   keep it alive and gate sending on `isStreaming`.

5. **A dead camera still reports "streaming".**
   `onCameraError` only toasts; `Preview.onPause` closes the camera and the
   high-speed path drops the surface, all without touching OMT. `isStreaming`
   stays true and the TCP channel stays open, so the receiver lists a source with
   no frames — and pressing the OMT button hits an early `return true` that never
   creates a new `ImageReader`.
   *Fix direction:* tear down from `onCameraError`/`closeCamera`/high-speed, and
   in `startStreaming` verify the reader exists and matches the requested size
   instead of trusting the flag.

## Transport / correctness

6. **Fixed port, no fallback.** `NETWORK_PORT_END = 6600` in `OMTConstants.h` is
   referenced nowhere; `Sender::start()` binds 6400 or fails, and the Java side
   advertises a hardcoded `6400` literal that must agree with the native constant
   by hand. *Fix direction:* return the bound port from the native layer and
   advertise that.

7. **`YUV_420_888` is treated as NV12 without checking `pixelStride`.** A device
   reporting a fully planar layout produces wrong chroma rather than an error.
   *Fixed here:* `omtpack::packNv12()` (`app/src/main/cpp/omtnv12.h`) asks the HAL
   which chroma layout it chose — `planes[1].getPixelStride()`, 2 for interleaved
   NV12, 1 for planar I420 — interleaves Cb/Cr when it has to, and the frame is now
   charged for the bytes it actually wrote instead of for two strides that need not
   agree. Measured on the emulator (Android 15, `sdk_gphone64_x86_64`, back camera):
   y `rowStride=1920 pixelStride=1`, chroma `rowStride=960 pixelStride=1` — a planar
   device, so every frame came up `1080*1920 + 540*960 = 2592000` bytes against the
   `3110400` the encoder reads, and `Sender::validateFrame()` refused all of them:
   the wire carried the greeting, the metadata, and 0 video frames. Both layouts are
   under test in `qa/planar-chroma.cpp`, with the planar case pinned at that geometry.

8. **`SO_SNDBUF` is forced**, which switches off kernel auto-tuning; on a healthy
   link that is usually worse than letting the stack decide.

9. **Encoding runs on the capture thread, under `g_senderMutex`.** One slow encode
   stalls the producer path for every subscriber.

## Security / hygiene (upstream Open Camera surface)

10. **`test_project` intent extra is honoured from any app.**
    `MainActivity` is exported and reads `getIntent().getExtras().getBoolean("test_project")`,
    which then gates runtime-permission and device-specific behaviour. An external
    app can start the camera in a test mode never meant to be reachable.

11. **`MyDebug.LOG = true` ships.** The file's own comment says it should always be
    false in released versions; with it on, capture URIs, file paths and internal
    state go to logcat. (PR #8 flips it, but inside an unrelated security change.)

12. **`MediaStore.EXTRA_OUTPUT` on the still-image path is unvalidated** — the
    primary arbitrary-file-overwrite vector for an exported capture activity on
    API ≤ 28, and the one PR #8 does not cover (it guards the video path only).
