# Dossier: Android camera-format & colour-space robustness for the opencamera-omt OMT sender

Repo: /home/reality/omt-src/opencamera-omt (Android OMT sender; fork of Open Camera).
Counterpart: /home/reality/nodos-yded-linux/plugins/yd.net.ompout/vendor/libomtcpp (desktop vendored copy).
We own the source of libvmx/libomt and their ports — cross-platform buildability is in our control.
Date: 2026-10-15.

## Measured today on a real running environment (not speculation)

Environment: official Android emulator (emulator 37.2.12, AVD `omt`, android-35 google_apis x86_64,
KVM), back camera = host v4l2loopback `/dev/video10` fed by ffmpeg from a real MP4, app installed
(arm64 APK under `libndk_translation.so`), sender reached from the host via local `adb forward`.

Result after tapping the app's OMT toggle (`io.github.ysdede.opencameraomt:id/omt_streaming`):
the wire carries the greeting and metadata but **0 video frames**, because EVERY frame is refused by
our own gate:

    E omt-sender: Sender::send: frame rejected: buffer too small for NV12 1920x1080 stride=1920:
      the encoder reads 3110400 bytes, dataLength is 2592000 (a Camera2 device whose uv rowStride is
      smaller than its y rowStride lands here: the chroma plane is walked with the luma stride, so
      the read would run past the ImageReader allocation — refuse the frame, do not read it)

Arithmetic that localises it: `app/src/main/cpp/OMTBridge.cpp:121`
    int dataLength = (height * yStride) + ((height / 2) * uvStride);
With yStride=1920, h=1080, the observed 2,592,000 implies **uvStride = 960** — the row length of a
PLANAR chroma plane (I420: separate Cb/Cr, pixelStride 1), not of interleaved NV12 (chroma row =
yStride bytes). Working hypothesis: this HAL reports YUV_420_888 with chroma pixelStride==1 while the
bridge assumes interleaved NV12 unconditionally. (A subagent is confirming pixelStride and fixing it
on branch `fix/omt-bridge-planar-yuv`; do not duplicate that fix — your job is the wider picture.)

## Where the limits actually are (read these before proposing anything)

- `libvmx/src/vmxcodec_common.h:39-42` — VMX_MIN_WIDTH/HEIGHT 16, **VMX_MAX_WIDTH 7680, VMX_MAX_HEIGHT 4320**.
  So UHD/4K (3840x2160) and even 8K are inside the encoder's own limits; resolution is not the encoder's problem.
- `libomt/cpp/OMTSender.cpp:45-50` — `kMinEncodeBufferBytes` = 1 MiB, `kMaxEncodeBufferBytes` (read the
  value yourself) with a `static_assert` that it stays <= INT_MAX because VMX_SaveTo takes int capacity.
- `libomt/cpp/OMTSender.cpp:287` `Sender::encodeBufferCapacity()` — 64-bit sizing + clamps; the Android copy
  previously had a signed-overflow bug (`int maxSize = width*height*4`) that wrapped at 32768x16385.
- `libomt/cpp/OMTSender.cpp:450` `Sender::validateFrame()` — the gate that saved us from a SIGSEGV; its
  message is the reason this was diagnosable. Do not propose weakening it.
- `libomt/cpp/OMTSender.cpp:422-429` — `static_assert` pinning the wire numbering to 0/601/709 (a second,
  contradictory set BT601=1/BT709=2 existed in OMTProtocol.h and was removed).
- `libomt/cpp/OMTSender.cpp:545-560` — BT.2020 is refused at the sender boundary (fail closed), because
  `MediaFrame::colorSpace` is a plain int and nothing type-level stops a caller passing 2020.
- `app/src/main/java/net/sourceforge/opencamera/OMTStreamingManager.java:112-136` —
  `startAnnouncing(width, height, fps, quality)`, quality 1=Low/50=Medium/100=High. Width/height come from
  the app's video settings; the sender is handed whatever the UI produced.
- Build: `app/build.gradle` compileSdk 35, minSdk 21, targetSdk 35, **abiFilters 'arm64-v8a' only**;
  AGP 8.7.3 / Gradle 8.9.
- CORRECTION (parent-verified after this dossier was first written): there are **no committed prebuilt
  `.so` files** — `app/src/main/jniLibs/` does not exist; commit `367b2be7 fix(build): drop the
  checked-in libvmx.so prebuilt that collided with the CMake target` already removed them, so the
  native libs ARE built from source. Do not plan a task to "remove the prebuilt dependency" — it is done.
- What actually blocks a second ABI is `app/src/main/cpp/CMakeLists.txt:17-20`: the `vmx` target is
  hard-wired to `${LIBVMX_DIR}/src/vmxcodec.cpp` + `${LIBVMX_DIR}/src/vmxcodec_arm.cpp` compiled with
  `-DARM64`. An x86_64 build needs the x86 sources instead (`vmxcodec_x86.cpp`, `vmxcodec_avx2.cpp` —
  both exist in `libvmx/src/` and are exactly what the desktop/nodos host QA compiles and runs on x86),
  plus the `-DARM64` define and any NEON/sse2neon assumptions made conditional on `ANDROID_ABI`.
  So the cross-platform task is a CMake/ABI task, not a source-availability task.

## Known ecosystem variance to design against (this is the core of your task)

Android Camera2 does NOT give you a guaranteed pixel layout. Concretely:
- `ImageFormat.YUV_420_888` may arrive interleaved-NV12 (chroma pixelStride 2, one UV plane) or
  planar-I420-ish (pixelStride 1, separate Cb/Cr buffers) depending on HAL, sensor, resolution and even
  surface type. Some HALs report rowStride > width (padding), some give equal strides, some differ
  between the Y and UV planes.
- The app currently assumes one of those shapes. That assumption is what just failed on the emulator.
- Physical devices vary further: the realme 12+ 5G (RMX3867) we own has NEVER been measured for
  chroma pixelStride — the earlier session could not get frames out of it at all (the app was in its
  by-design "announcing without streaming" state), so "it works on phones" is UNPROVEN, not proven.
- Colour space: camera pipeline may be 601 or 709 depending on size/quality (and HDR profiles bring
  2020/HLG on some devices). VMX encodes 0/601/709 only; 2020 must fail closed — and the *announcement*
  (`<OMTInfo>`/VideoExtHeader) must not claim a space the payload does not carry. We already had a
  "lying Alpha flag" incident on the desktop side (Flags=2 Alpha on a BGRX payload → OBS dropped every
  frame), so declaration/payload honesty is a known failure family for us.
- UHD: 3840x2160 NV12 = 12,441,600 bytes/frame; 8K = ~49.8 MB/frame. Consider encode buffer sizing, the
  int-capacity clamp, per-frame encode time vs the 25/30/60 fps target on mid-range SoCs, kernel socket
  buffers (desktop box measured tcp_wmem[2]=4 MB — a big I-frame can exceed it and a slow subscriber
  then stalls the sender; we have already fixed two slow-subscriber bugs of this family), and whether
  the app can even be configured to UHD today.

## What we need from you

Read the actual files (cite `file:line` for every claim; if you cannot verify something, say
`NOT VERIFIED` rather than guessing). Then deliver:

1. **Adversarial review** of the capture→encode→wire path for format/colour-space honesty and device
   variance: what else besides chroma layout is assumed-but-unverified (stride relationships, buffer
   ordering, timestamp units, fps declaration vs actual, aspect ratio, HDR profiles, sensor orientation
   rotation, preview-vs-video size mismatch, ImageReader format choice, JPEG/blob paths, permission and
   lifecycle differences across API 21..36). For each: severity, how it would manifest to a user, and
   the smallest measurement that would prove or kill it.
   End with a verdict line, exactly one of: `VERDICT: READY FOR ACCEPTANCE` or `VERDICT: NOT READY`.
2. **A long, ordered implementation task list** (this is the main deliverable) to make the sender
   robust across the Android ecosystem, including at minimum:
   - full YUV_420_888 shape handling (interleaved and planar, any rowStride/pixelStride combination),
     with the conversion done once and proven by byte-level tests;
   - an explicit format/colour-space capability matrix and a single honest place where the wire's
     declared colour space is derived from what the payload actually contains;
   - UHD/4K (and documented 8K) support: buffer sizing, int clamps, encode-budget vs fps, subscriber
     back-pressure, and what happens on a device that offers UHD but cannot sustain it;
   - a device-matrix test plan we can run without a lab: what the emulator can and cannot prove
     (it has no real HAL), and which claims require a physical device — say plainly which is which;
   - cross-platform buildability: building libvmx/libomt from source for arm64-v8a AND x86_64 (the x86
     encoder path already compiles and runs in the desktop/nodos host QA), removing the committed
     prebuilt .so dependency, and what CI gate proves the two ABIs are actually the same source.
   Each task: what to change (file/function), the test that must go RED before the change and GREEN
   after, the anti-vacuity condition (what would make the test pass even with the fix absent — we have
   been burned by exactly this), and whether it is host-provable or device-only.
3. **Sequencing**: which of these unblock the others, and which are safe to do in parallel.

Constraints we hold you to: no tolerance-tuning to make a failing numeric gate pass; a green suite is
not evidence unless there is a run where disabling the gate turns it red; do not propose deleting or
weakening `validateFrame`; do not propose file-copying between the Android and desktop vendored trees
(they have diverged and file copies silently revert protections); Android host QA here is ASan-only.
