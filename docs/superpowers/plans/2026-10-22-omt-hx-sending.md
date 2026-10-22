# OMT-HX sending from Open Camera — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The phone streams its camera to a PC as OMT-HX (hardware-encoded HEVC, H.264 fallback), selectable beside the existing Full/VMX mode.

**Architecture:** The phone's MediaCodec encoder writes Annex-B access units into a JNI call that hands them to the vendored C++ libomt sender. libomt is taught to pass coded frames through instead of VMX-encoding them, tagging the wire header `'HEVC'`/`'H264'`. Announcement, discovery, ports, tally and stats are untouched; the receiver side already exists.

**Tech Stack:** C++17 (libomt port), JNI, Android MediaCodec/Camera2, Gradle 9.8.1 + AGP 9.4.1, JUnit 4.

**Spec:** `docs/superpowers/specs/2026-10-22-omt-hx-sending-design.md`

## Global Constraints

- compileSdk 36, minSdk 21, targetSdk 36, `ndkVersion "27.0.12077973"`, CMake 3.22.1, ABIs `arm64-v8a` + `x86_64`.
- Codec tags on the wire: `'HEVC'` = `0x43564548`, `'H264'` = `0x34363248`, `'VMX1'` = `0x31584D56`.
- Wire timestamps are **100 ns ticks**; the JVM passes nanoseconds and the JNI divides by 100.
- Coded payloads are one Annex-B access unit per OMT video frame, 8-bit 4:2:0, no alpha, no interlace.
- Colour space: `height >= 720 ? BT709 : BT601` (same rule as `OMTBridge.cpp:81`).
- Bitrate: `width * height * fps * bpp`, bpp Low 0.06 / Medium 0.10 / High 0.15 (quality `0` maps to Medium), H.264 ×1.6, clamped to 4_000_000..250_000_000.
- Settings keys: `preference_omt_streaming_mode` (`full`|`hx`), `preference_omt_streaming_hx_codec` (`hevc`|`h264`).
- Full/VMX mode must behave exactly as today; nothing in this plan changes its path.
- Host QA uses real sockets on fixed ports; the new ones are **45348** (wire) and **45349** (JNI).

## Review Focus

1. **Portrait rotation** — the phone held in portrait sends the sensor orientation; a reasonable user expects the PC picture upright. Device checklist only (Task 7).
2. **High-speed refusal** — a 4K120 request on a device whose camera refuses the encoder surface in a constrained session must fall back to a rate that works and say so, not fail silently (Task 5 status text, Task 7).
3. **Fast stop/start** — releasing the MediaCodec input surface while Camera2 still targets it kills the process; the release is sequenced after `CloseCameraCallback.onClosed` (Task 6).
4. **No HEVC encoder / wrong receiver** — H.264 fallback on the phone, and a Full-only receiver showing audio and no picture in HX mode (Task 4 + Task 7 negative check).
5. **Late receiver / mid-GOP join** — a receiver that connects three seconds after start must show a picture within ~2 s (1 s IDR), not a frozen black frame (Task 4 behaviour, Task 7 check).

---

### Task 1: libomt sends coded frames

**Files:**
- Modify: `libomt/cpp/OMTProtocol.h:63-70` (codec constants)
- Modify: `libomt/libomt.h:71-84` (public `OMTCodec` enum)
- Modify: `libomt/cpp/OMTSender.cpp` (`send()`, `validateCodedFrame()`, `buildVideoHeaders()`)
- Modify: `libomt/cpp/OMTSender.h` (declare `validateCodedFrame`)
- Create: `qa/coded-send.cpp`
- Modify: `qa/build-host.sh` (scenario + two mutants)

**Interfaces:**
- Consumes: `omt::Sender::send(const MediaFrame&)`, `encodeFailures()`, `lastError()`, `qa/omt-wire.h` (`Stream`, `Reader`, `Tally`).
- Produces: `Codec::HEVC`, `Codec::H264`; `OMTCodec_HEVC`, `OMTCodec_H264`; `int Sender::send(const MediaFrame&)` returns payload length when delivered, `1` with no subscribers, `0` when refused.

- [ ] **Step 1: Write the failing scenario** in `qa/coded-send.cpp` (port 45348), using `qa/omt-wire.h`:

```cpp
// HX1: a coded HEVC frame reaches a subscribed socket with VideoExtHeader.codec == 'HEVC'
// HX2: its payload is byte-identical to what was posted
// HX3: header width/height follow the frame (send 64x64 then 128x72; second header is 128x72)
// HX4: two frames 3333333 ticks apart arrive with that timestamp delta
// HX5: the same with 'H264'
// HX6..8: odd width (65), dataLength 0, and VideoFlags::Alpha are refused, counted, and named
// HX9: a coded frame sent to a Sender with no subscribers returns 1, not 0
// HX10 (anti-vacuity): a BGRA 64x64 frame still arrives tagged 'VMX1'
```

- [ ] **Step 2: Run it and watch it fail**

Run: `wsl -d Ubuntu -- bash -lc 'cd /mnt/f/Coding/BreadMedia/opencamera-omthx && bash qa/build-host.sh'`
Expected: `coded-send: BUILD FAILED` (or the first HX case red) because `Codec::HEVC` does not exist and the sender rejects the codec.

- [ ] **Step 3: Add the constants**

`OMTProtocol.h` namespace `Codec`: `constexpr int32_t HEVC = 0x43564548; constexpr int32_t H264 = 0x34363248;`
`libomt.h` enum `OMTCodec`: `OMTCodec_HEVC = 0x43564548, OMTCodec_H264 = 0x34363248,`
Pin both against the wire next to the existing colour-space `static_assert` in `OMTSender.cpp`:
`static_assert(Codec::HEVC == OMTCodec_HEVC && Codec::H264 == OMTCodec_H264, "Hx codec tags must match the public C enum");`

- [ ] **Step 4: Branch `send()` on coded frames**

In `Sender::send`, `const bool coded = frame.codec == Codec::HEVC || frame.codec == Codec::H264;`. When `coded`:
skip `createEncoder()`/`encodeFrame()`; set `encodedLen = frame.dataLength`, `havePayload = true`; fill the
`EncoderIdentity` producer from the frame (`valid = true`, width, height, colorSpace) instead of from the VMX
encoder. Pass the codec into `buildVideoHeaders(...)` as a new `int codec` parameter that sets `ext.codec`.

- [ ] **Step 5: Add `validateCodedFrame`** (same refusal style as `validateFrame`: `why`, counted, named)

Refuse: null data, `width <= 0 || height <= 0`, odd width, `dataLength <= 0`, `frameRateN <= 0`,
`frameRateD <= 0`, non-finite/non-positive aspect, colour space outside 0/601/709, and any of
`VideoFlags::Alpha | PreMultiplied | HighBitDepth | Interlaced` or an undefined flag bit. `stride` is ignored.

- [ ] **Step 6: Run the scenario and the whole harness**

Run: same command as Step 2.
Expected: `coded-send: pass as expected` and every existing scenario still passes.

- [ ] **Step 7: Add the mutants** in `qa/build-host.sh` (the harness's `apply_mutant` + `fresh_copy` pattern, both must come back red)

- `cs-mutant-codec`: replace `ext.codec = codec;` with `ext.codec = Codec::VMX1;` → HX1 fails.
- `cs-mutant-geometry`: in the coded producer identity, replace the width/height assignment with `1920`/`1080` → HX3 fails.

- [ ] **Step 8: Commit**

```bash
git add libomt/cpp/OMTProtocol.h libomt/libomt.h libomt/cpp/OMTSender.cpp libomt/cpp/OMTSender.h qa/coded-send.cpp qa/build-host.sh
git commit -m "feat(omt): send already-coded Hx frames through the sender"
```

---

### Task 2: JNI `nativeSendCoded`

**Files:**
- Modify: `app/src/main/cpp/OMTBridge.cpp` (new entry point next to `nativeSendFramePlanes`)
- Modify: `app/src/main/java/net/sourceforge/opencamera/OMTSender.java` (native declaration + tag constants)
- Create: `qa/coded-send-jni.cpp`
- Modify: `qa/build-host.sh` (`build_jni` scenario + mutant)

**Interfaces:**
- Consumes: Task 1's coded sender.
- Produces: `OMTSender.sendCoded(ByteBuffer data, int offset, int length, int codec, int width, int height, int fpsN, int fpsD, long ptsNs)`; `OMTSender.CODEC_HEVC` / `OMTSender.CODEC_H264`; JNI symbol `Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded`.

- [ ] **Step 1: Write the failing host test** `qa/coded-send-jni.cpp`, modelled on `qa/jni-boundary.cpp` (real `OMTBridge.cpp` against `qa/host-jni/jni.h`, real `omt::Sender` behind an `omt_send` shim that records the `MediaFrame`), port 45349:

```cpp
// JC1: the recorded MediaFrame has Codec == OMTCodec_HEVC, DataLength == length,
//      Data == buffer + offset, Width/Height/FrameRateN/FrameRateD from the arguments,
//      Timestamp == ptsNs / 100, AspectRatio == width/(float)height, Flags == 0,
//      ColorSpace == 709 for 1080p and 601 for 480p
// JC2: offset + length > GetDirectBufferCapacity() is refused: no omt_send call, a log line naming it
// JC3: length == 0 and fpsD == 0 are refused
// JC4: a codec value that is neither HEVC nor H264 is refused
```

- [ ] **Step 2: Run it and watch it fail**

Run: `wsl -d Ubuntu -- bash -lc 'cd /mnt/f/Coding/BreadMedia/opencamera-omthx && bash qa/build-host.sh'`
Expected: build failure — the symbol does not exist.

- [ ] **Step 3: Implement the JNI function**

```cpp
extern "C" JNIEXPORT jboolean JNICALL
Java_net_sourceforge_opencamera_OMTSender_nativeSendCoded(
    JNIEnv* env, jobject, jobject buffer, jint offset, jint length, jint codec,
    jint width, jint height, jint frameRateN, jint frameRateD, jlong ptsNs);
```

Under `g_senderMutex`: refuse when `g_sender == nullptr`, when `GetDirectBufferAddress` is null, when
`offset < 0 || length <= 0`, when `offset + length > GetDirectBufferCapacity(buffer)`, when the codec is neither
`OMTCodec_HEVC` nor `OMTCodec_H264`, and when `frameRateD <= 0` — each with a named `LOGE`. Otherwise fill
`OMTMediaFrame{Type=Video, Codec=codec, Data=base+offset, DataLength=length, Width, Height, Stride=width,
FrameRateN, FrameRateD, AspectRatio=width/(float)height, Flags=0, ColorSpace=height >= 720 ? BT709 : BT601,
Timestamp=ptsNs/100}` and call `omt_send`.

- [ ] **Step 4: Run it green**

Run: same command. Expected: `coded-send-jni: pass as expected`, all previous scenarios green.

- [ ] **Step 5: Add the mutant** `jc-mutant-microseconds`: replace `ptsNs / 100` with `ptsNs` → JC1 fails.

- [ ] **Step 6: Install the Android toolchain, then compile the APK**

```powershell
# JDK 25 is already installed and JAVA_HOME points at it.
$sdk = "$env:LOCALAPPDATA\Android\Sdk"
# 1. command-line tools: the current "commandlinetools-win-*_latest.zip" from
#    https://developer.android.com/studio#command-line-tools-only
#    Unzip so that "$sdk\cmdline-tools\latest\bin\sdkmanager.bat" exists.
# 2. packages (if a version is not offered, `sdkmanager --list` and take the current 36.x build-tools)
& "$sdk\cmdline-tools\latest\bin\sdkmanager.bat" --sdk_root=$sdk "platform-tools" "platforms;android-36" "build-tools;36.0.0" "ndk;27.0.12077973" "cmake;3.22.1"
& "$sdk\cmdline-tools\latest\bin\sdkmanager.bat" --sdk_root=$sdk --licenses
[Environment]::SetEnvironmentVariable('ANDROID_HOME', $sdk, 'User')
[Environment]::SetEnvironmentVariable('ANDROID_SDK_ROOT', $sdk, 'User')
```

Then, in a new shell:

Run: `./gradlew --no-daemon assembleDebug`
Expected: `BUILD SUCCESSFUL`; `app/build/outputs/apk/debug/*.apk` exists. If Gradle rejects JDK 25, install Temurin 21 and set `JAVA_HOME` for the build (record which, in the commit message).

- [ ] **Step 7: Commit**

```bash
git add app/src/main/cpp/OMTBridge.cpp app/src/main/java/net/sourceforge/opencamera/OMTSender.java qa/coded-send-jni.cpp qa/build-host.sh
git commit -m "feat(omt): a JNI entry point for hardware-coded access units"
```

---

### Task 3: Geometry clamp and bitrate, as pure JVM code

**Files:**
- Create: `app/src/main/java/net/sourceforge/opencamera/OMTHXFormat.java`
- Test: `app/src/test/java/net/sourceforge/opencamera/OMTHXFormatTest.java`

**Interfaces:**
- Consumes: nothing (pure functions).
- Produces: `int[] OMTHXFormat.choose(int reqW, int reqH, int reqFps, int[][] cameraSizes, Limits limits)` returning `{width, height, fps}` or `null`; `int OMTHXFormat.bitrate(int width, int height, int fps, int quality, boolean hevc)`; nested `interface Limits { int maxFpsAt(int width, int height); }` (0 = unsupported).

- [ ] **Step 1: Write the failing tests** (JUnit 4, `assertEquals`)

```java
// ch1: 3840x2160@120 supported and in cameraSizes -> {3840,2160,120}
// ch2: 4K supports only 60 -> {3840,2160,60}
// ch3: 1080p supports 240, request 120 -> 120 (never above the request)
// ch4: codec tops out at 1080p while the request is 4K60 -> largest 1080p size at 60
// ch5: nothing supports 60, the largest size does 30 -> that size at 30
// ch6: nothing supported -> null
// br1: 1920x1080x30 Low -> 3_732_480; Medium -> 6_220_800; High -> 9_331_200
// br2: 3840x2160x120 High -> 149_299_200; H.264 High -> 238_878_720
// br3: 7680x4320x60 High H.264 -> clamped to 250_000_000; 320x240x15 Low -> clamped to 4_000_000
// br4: quality 0 (Auto) uses the Medium bpp
```

- [ ] **Step 2: Run and watch them fail**

Run: `./gradlew --no-daemon :app:testDebugUnitTest --tests "net.sourceforge.opencamera.OMTHXFormatTest"`
Expected: compilation failure — `OMTHXFormat` does not exist.

- [ ] **Step 3: Implement `choose`** in this exact order (the design's clamp):

1. If `limits.maxFpsAt(reqW, reqH) >= reqFps` and `{reqW,reqH}` is in `cameraSizes` → `{reqW, reqH, reqFps}`.
2. Otherwise the largest camera size (by pixels, not larger than the request) whose `maxFpsAt >= reqFps` → that size at `reqFps`.
3. Otherwise the largest camera size with `maxFpsAt > 0` → that size at `min(maxFpsAt, reqFps)`.
4. Otherwise `null`.

- [ ] **Step 4: Implement `bitrate`** with the Global Constraints table; round with `Math.round`, clamp last.

- [ ] **Step 5: Run green**

Run: same as Step 2. Expected: `BUILD SUCCESSFUL`, 10 tests passed.

- [ ] **Step 6: Commit**

```bash
git add app/src/main/java/net/sourceforge/opencamera/OMTHXFormat.java app/src/test/java/net/sourceforge/opencamera/OMTHXFormatTest.java
git commit -m "feat(omt): pick the best device geometry and an Hx bitrate"
```

---

### Task 4: `OMTHXEncoder` — the MediaCodec wrapper

**Files:**
- Create: `app/src/main/java/net/sourceforge/opencamera/OMTHXEncoder.java`

**Interfaces:**
- Consumes: `OMTHXFormat`, `OMTSender.CODEC_HEVC`/`CODEC_H264`, `OMTSender.sendCoded(...)`.
- Produces: `OMTHXEncoder(String mime, int width, int height, int fps, int bitrate)`, `Surface getInputSurface()`, `void start(Sink sink)`, `void stop()`, nested `interface Sink { void onAccessUnit(ByteBuffer data, int offset, int length, int codecTag, int width, int height, int fpsN, int fpsD, long ptsNs, boolean keyframe); }`.

- [ ] **Step 1: Implement the wrapper** (no unit test is possible: MediaCodec has no JVM implementation; the pure decisions live in Task 3)

- `MediaCodec.createEncoderByType(mime)` with `COLOR_FormatSurface`, `KEY_BITRATE_MODE = BITRATE_MODE_CBR`, `KEY_BITRATE`, `KEY_FRAME_RATE = fps`, `KEY_I_FRAME_INTERVAL = 1`, `KEY_PREPEND_HEADER_TO_SYNC_FRAMES = 1`, `KEY_OPERATING_RATE = fps`, `KEY_PRIORITY = 0`, profile Main.
- `getInputSurface()` returns `codec.createInputSurface()`; `start()` also calls `codec.start()`.
- A daemon drain thread: `dequeueOutputBuffer(info, 10_000)`; `BUFFER_FLAG_CODEC_CONFIG` buffers are copied into a
  `csd` array and **not** sent; every other buffer is sent as `csd + buffer[offset..offset+size]` when it is a key
  frame and `csd` is non-empty, else as the buffer alone, with `ptsNs = info.presentationTimeUs * 1000` and
  `keyframe = (info.flags & BUFFER_FLAG_KEY_FRAME) != 0`; `releaseOutputBuffer(index, false)`.
- `stop()` sets a volatile flag, joins the thread, `codec.stop()`/`codec.release()`, and is idempotent.
- Refuse to construct when the device has no such encoder (`MediaCodecList` check in the caller).

- [ ] **Step 2: Compile**

Run: `./gradlew --no-daemon assembleDebug`
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 3: Commit**

```bash
git add app/src/main/java/net/sourceforge/opencamera/OMTHXEncoder.java
git commit -m "feat(omt): a MediaCodec Hx encoder and its drain loop"
```

---

### Task 5: Mode setting, codec setting, and the wiring

**Files:**
- Modify: `app/src/main/java/net/sourceforge/opencamera/PreferenceKeys.java:415-418`
- Modify: `app/src/main/res/xml/preferences_sub_video.xml:171-202`
- Modify: `app/src/main/res/values/arrays.xml:1046-1059` (new arrays)
- Modify: `app/src/main/java/net/sourceforge/opencamera/OMTStreamingManager.java` (`startStreaming`, `getStreamingSurface`, cleanup)
- Modify: `app/src/main/java/net/sourceforge/opencamera/MainActivity.java:2495-2516, 2553-2570, 7252-7325`

**Interfaces:**
- Consumes: Tasks 2–4.
- Produces: `OMTStreamingManager.startStreaming(int width, int height, int fps, int quality, String name, String mode, String codec)`; `OMTStreamingManager.getChosenGeometry()` returning `int[]{w,h,fps}` (the values it was handed).

- [ ] **Step 1: Add the preferences**

`PreferenceKeys`: `OmtStreamingModeKey = "preference_omt_streaming_mode"`, `OmtHxCodecKey = "preference_omt_streaming_hx_codec"`.
XML, inside the existing OMT category, following the file's literal-title style:

```xml
<ListPreference android:key="preference_omt_streaming_mode" android:title="Streaming Mode"
    android:entries="@array/preference_omt_mode_entries" android:entryValues="@array/preference_omt_mode_values"
    android:defaultValue="full" android:dependency="preference_omt_streaming_enabled" />
<ListPreference android:key="preference_omt_streaming_hx_codec" android:title="HX Codec"
    android:entries="@array/preference_omt_hx_codec_entries" android:entryValues="@array/preference_omt_hx_codec_values"
    android:defaultValue="hevc" android:dependency="preference_omt_streaming_mode" />
```

Arrays: mode entries `Full (VMX)`, `OMT HX (H.264/H.265)` with values `full`, `hx`; codec entries `H.265 (HEVC)`, `H.264` with values `hevc`, `h264`.

- [ ] **Step 2: Touch `OMTStreamingManager` only where the encoder lives**

`startStreaming(...)` keeps the chosen geometry it is handed. In HX mode it creates the
`OMTHXEncoder(mime, width, height, fps, OMTHXFormat.bitrate(width, height, fps, quality, hevc))`, starts it
with a sink that calls `omtSender.sendCoded(...)`, and returns `getInputSurface()` from
`getStreamingSurface()`. `stopStreaming`/`cleanup` stop the encoder when it exists. Full mode is untouched.

- [ ] **Step 3: Choose the geometry and codec in `MainActivity`**

Where quality/name are read for `startStreaming` (around line 2495): read the two preferences; if mode is `hx`,
build `int[][] cameraSizes` from `preview.getVideoQualityHander().getSupportedVideoSizes()` (a `List<CameraController.Size>`),
pick the MIME (`hevc` → `"video/hevc"`; fall back to `"video/avc"` and log when `MediaCodecList` has no HEVC
encoder), and pass the codec's `VideoCapabilities` as the `Limits` lambda:
`(w, h) -> caps.isSizeSupported(w, h) ? (int) Math.floor(caps.getSupportedFrameRatesFor(w, h).getUpper()) : 0`.
Then call `OMTHXFormat.choose(...)` with the profile's width/height/fps. If it returns `null`, toast and do not
start (Full stays available).
Pass the chosen `{w,h,fps}` plus mode and codec into `startStreaming`, and show `"HX HEVC 3840x2160@120"` (or
`"Full VMX"`) in `omt_status_text`.

- [ ] **Step 4: Build and run the JVM tests**

Run: `./gradlew --no-daemon assembleDebug :app:testDebugUnitTest --tests "net.sourceforge.opencamera.OMTHXFormatTest" --tests "net.sourceforge.opencamera.OMTStreamNameTest"`
Expected: `BUILD SUCCESSFUL`, all tests pass.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/java/net/sourceforge/opencamera/PreferenceKeys.java app/src/main/res/xml/preferences_sub_video.xml app/src/main/res/values/arrays.xml app/src/main/java/net/sourceforge/opencamera/OMTStreamingManager.java app/src/main/java/net/sourceforge/opencamera/MainActivity.java
git commit -m "feat(omt): choose Full or HX, and stream HX from the encoder surface"
```

---

### Task 6: Release the surface only after the camera lets go

**Files:**
- Modify: `app/src/main/java/net/sourceforge/opencamera/preview/Preview.java:2237-2263`
- Modify: `app/src/main/java/net/sourceforge/opencamera/MainActivity.java:2522-2534`

**Interfaces:**
- Consumes: the existing `closeCamera(boolean, CloseCameraCallback)`.
- Produces: `Preview.setOmtStreamingSurface(Surface surface, Runnable afterDetach)`; the old single-argument method delegates with `null`.

- [ ] **Step 1: Implement the ordered detach**

```java
public void setOmtStreamingSurface(android.view.Surface surface, Runnable afterDetach) {
    this.omt_streaming_surface = surface;
    closeCamera(true, () -> { if (afterDetach != null) afterDetach.run(); openCamera(); });
}
```

`stopOmtStreaming()` becomes `preview.setOmtStreamingSurface(null, omtStreamingManager::stopStreaming);` and no
longer calls `stopStreaming()` directly. `OMTHXEncoder.stop()` must tolerate being called after the surface was
released (it only stops the codec and joins the thread).

- [ ] **Step 2: Build**

Run: `./gradlew --no-daemon assembleDebug`
Expected: `BUILD SUCCESSFUL`.

- [ ] **Step 3: Commit**

```bash
git add app/src/main/java/net/sourceforge/opencamera/preview/Preview.java app/src/main/java/net/sourceforge/opencamera/MainActivity.java
git commit -m "fix(omt): stop the encoder after the camera has released its surface"
```

---

### Task 7: Documentation and the device run

**Files:**
- Modify: `README.md` (a "Streaming modes" section: Full vs HX, receiver requirement, bitrate table)
- Modify: `docs/known-issues.md` (HX v1 limits: no audio/alpha/preview stream, no keyframe-request handling, no live re-clamp)

**Interfaces:** none.

- [ ] **Step 1: Write the README section** naming: Mode = OMT HX needs an HX-capable receiver (`omtplugin` in OBS, `omthx-recv`); Full receivers show no picture in HX mode by design; choose Quality for the bitrate (table from Global Constraints); geometry follows the video profile up to 4K120 and is clamped to the device.

- [ ] **Step 2: Install on the phone and run the checklist against a PC** (this is the only proof the feature works; record each result in the commit message)

```
1. adb install -r app/build/outputs/apk/debug/app-debug.apk
2. Mode = OMT HX, Codec = H.265, Quality = Medium; start streaming.
   PC: omthx-recv --source "<phone> (<name>)" -> HEVC, picture, ~bitrate from the table.
3. OBS omtplugin OMT Source -> picture within 2 s (late join / mid-GOP).
4. Rotate the phone to portrait, then landscape: PC picture follows the sensor as Full mode does.
5. Attempt 4K120 (or the highest the phone offers); if the camera refuses the encoder surface in a
   constrained session, the status text shows the fallback rate and the stream keeps running.
6. Stop, immediately start again, twice: no crash, no black frame left behind.
7. Mode = Full: the VMX stream plays as before; a Full-only viewer still shows it.
8. HX mode with a Full-only viewer (plain OMT viewer): audio/no picture expected, documented.
```

- [ ] **Step 3: Commit**

```bash
git add README.md docs/known-issues.md
git commit -m "docs: OMT HX streaming modes and their limits"
```

---

## Self-review

- Spec §2 (wire facts) → Task 1 constants and header handling.
- Spec §4.1 → Task 1. §4.2 → Task 2. §4.3 → Tasks 3+4. §4.4 → Tasks 5+6. §4.5 → Task 3.
- Spec §6.1 → Tasks 1, 2. §6.2 → Task 2 Step 6. §6.3/§6.4 → Task 7.
- Wire values appear once in Global Constraints and are referenced, not repeated.
- Names are consistent: `sendCoded` (Java) / `nativeSendCoded` (JNI) / `Codec::HEVC` (C++); `OMTHXFormat.choose`/`bitrate` used by Task 4 and Task 5 only.