**Acceptance is blocked by remaining format, colour, configuration, and verification gaps.** The planar fix now present in the working tree is useful, but it does not establish Android-ecosystem robustness.

This is a source review, not a completed acceptance run. Builds, mutation runs, emulator reproduction, physical-device behaviour, and receiver interoperability are **NOT VERIFIED** in this review. The environment is read-only; I changed no files.

The checkout changed during inspection. The findings below use the subsequently reread three-plane implementation, not the earlier two-buffer implementation. Base commit: `3d64d14a6ee11c683fe6510e370f7e329b370eac`, plus working-tree changes. Reviewed file hashes:

| File | SHA-256 |
|---|---|
| `app/src/main/cpp/OMTBridge.cpp` | `2fcfb33ffd3184fea45429b098cfb5bade1b6c689d74a41b8d9231193e2c840f` |
| `app/src/main/cpp/omtnv12.h` | `0a8f2321865367f42064b2bb8c237d2202b9cd3be0d77eeae6cb62dadb11af41` |
| `app/src/main/java/net/sourceforge/opencamera/OMTSender.java` | `994c68758c7c858fc4814b0da594e86b063a370e0fdf91fe603cdbd95b07bdf0` |
| `app/src/main/java/net/sourceforge/opencamera/OMTStreamingManager.java` | `3b60405588b458b0963256d98777984846f04cbe83fa6153b7d0933df59782bc` |

Java filenames below are relative to `app/src/main/java/net/sourceforge/opencamera/`; other paths are repository-relative. New test filenames are proposed deliverables.

**1. Adversarial review**

**Corrections to the starting assumptions**

- The current capture path passes all three planes to JNI, and JNI calls `packNv12()`. The old two-buffer copy is no longer the active implementation in the reviewed snapshot. Its runtime success remains **NOT VERIFIED**. Evidence: `OMTStreamingManager.java:389–436`; `app/src/main/cpp/OMTBridge.cpp:235–262`.
- Android exposes three Y/U/V plane buffers. Pixel stride 2 does **not** establish that the U buffer contains a complete, contiguous NV12 chroma plane. Android guarantees Y/U/V ordering, Y pixel stride 1, and matching U/V strides; the packer’s stronger interpretation is unsupported. Evidence: `app/src/main/cpp/omtnv12.h:130–158,180–184`; [Android YUV_420_888 contract](https://developer.android.com/reference/android/graphics/ImageFormat#YUV_420_888).
- The dossier’s “committed prebuilt `.so`” claim does not match this checkout. `git ls-files '*.so'` returned no files, and `app/src/main/jniLibs` does not exist. CMake already declares source-built VMX and OMT targets. Successful clean APK construction and packaged-library provenance are **NOT VERIFIED**. Evidence: `app/src/main/cpp/CMakeLists.txt:18–21,47–53`; contradictory dossier claim: `qa/ANDROID-FORMAT-ROBUSTNESS-DOSSIER.md:48–50`.
- The emulator failure and RMX3867 history are dossier-reported observations, not independently reproduced here. Actual RMX3867 plane geometry remains **NOT VERIFIED**. Evidence: `qa/ANDROID-FORMAT-ROBUSTNESS-DOSSIER.md:10–29,60–62`.

| Finding and severity | Evidence and adversarial consequence | Smallest measurement that proves or kills it |
|---|---|---|
| **High — pixel stride 2 still means “copy U as NV12.”** | `app/src/main/cpp/omtnv12.h:130–158,180–184` checks U/V pixel strides, then validates and copies only U. A VU-backed view, separate stride-2 U/V buffers, or a U view ending at its last U sample can produce wrong chroma or reject a valid frame. This is not solved by the planar branch. | Feed distinct U/V sample values through separate stride-2 buffers and both shared-memory orderings, with exactly bounded final samples. Compare every packed byte against an independent scalar oracle. |
| **High — JNI trusts caller-supplied readable lengths.** | `app/src/main/cpp/OMTBridge.cpp:225–243` checks direct addresses and destination capacity, but constructs source `Plane` objects from `ySize/uSize/vSize` without checking source capacities. `OMTSender.java:176–197` checks null/direct/position/destination properties, not those source lengths. An overstated length can defeat the packer’s bounds checks. | Call the public plane wrapper with a short direct buffer and a larger claimed size. Require rejection before the first read, with an ASan-instrumented native equivalent. |
| **High — crop and buffer-window semantics are not represented.** | `OMTStreamingManager.java:398–436` uses full image dimensions, rewinds buffers, and passes limits. It does not convey crop coordinates or preserve a meaningful nonzero input position. The native interface has no crop/window-offset arguments at `app/src/main/cpp/OMTBridge.cpp:209–216`. Cropped or offset input can become the wrong picture. | Use nonzero buffer positions and a crop with unique edge markers. Verify exact output pixels and dimensions; prove that an unsupported crop is explicitly rejected rather than silently ignored. |
| **High — image format is assumed after reader creation.** | Reader creation requests YUV at `OMTStreamingManager.java:223–227`; processing checks only that there are at least three planes at `:389–396`. The processor does not itself establish that this image is supported 8-bit YUV. A misrouted image should not be interpreted by plane count. | Inject JPEG/blob, PRIVATE, higher-bit-depth, and wrong-plane-count descriptors into the processing boundary. Require named rejection before allocation or plane access. Actual camera misrouting is **NOT VERIFIED**. |
| **High — colour space is inferred from height.** | `app/src/main/cpp/OMTBridge.cpp:68` labels every frame below 720 lines 601 and every other frame 709. Neither capture metadata nor a colour transform participates. A correctly packed image can therefore carry an incorrect colour claim. | Supply equally sized images with independently established 601 and 709 provenance. Check the selected encoder space and received header, not just successful decoding. |
| **High — HDR/range/transfer information disappears before validation.** | The bridge emits NV12, no flags, and height-derived 601/709 at `app/src/main/cpp/OMTBridge.cpp:59–74`. `Sender::validateFrame()` correctly rejects explicit 2020 at `libomt/cpp/OMTSender.cpp:552–556`, but cannot reject source information the bridge replaced. The NV12 codec path performs plane conversion, not a documented HDR-to-SDR transformation: `libvmx/src/vmxcodec.cpp:1405–1417`. | Inject known 2020/HLG/PQ/full-range descriptors. Require explicit rejection or a separately proven conversion before any 601/709 declaration. Actual device HDR output is **NOT VERIFIED**. |
| **High — encoder reuse ignores colour-space changes.** | `libomt/cpp/OMTSender.cpp:315–317` reuses an encoder based only on width/height. Creation consumes colour space at `:321–324`; headers use the current frame’s colour space at `:633`. A same-size switch can leave RGB conversion using the previous encoder configuration while advertising the new one. | Send same-sized BGRA frames through 601→709→601. Record creation parameters and compare against independently created reference encoders. Include NV12 header-policy tests separately. |
| **Medium — capture time is replaced by send-entry time.** | `app/src/main/cpp/OMTBridge.cpp:55–57` generates a steady-clock timestamp. Division by 100 matches the local protocol’s 100 ns units at `libomt/cpp/OMTProtocol.h:22`; this is not evidence of an exposure timestamp. Queueing delay can be hidden in apparent capture timing. | Inject known capture timestamps and variable processing delays. Wire deltas must follow the declared timestamp policy. Cross-clock/device synchronization is **NOT VERIFIED**. |
| **High — advertised fps can be stale or rounded.** | `MainActivity.java:2360` truncates capture rate to an integer. `OMTSender.java:73` supplies denominator 1. `OMTStreamingManager.java:215–219` skips announcement initialization when already announcing; the bridge retains initialized fps at `app/src/main/cpp/OMTBridge.cpp:138–139` and publishes it at `:65–66`. | Announce at 30, change settings, then stream at 25 or a fractional rate. Parse received headers and measure capture intervals independently. |
| **Medium — aspect ratio and orientation lack an explicit contract.** | `app/src/main/cpp/OMTBridge.cpp:61–68` uses raw dimensions and width/height aspect ratio. Packing copies rows without orientation transformation at `app/src/main/cpp/omtnv12.h:173–197`. The wire header has no explicit rotation field at `libomt/cpp/OMTProtocol.h:33–43`. Upright preview does not establish upright output. | Film an asymmetric labelled chart with front/back cameras at four rotations. Compare decoded orientation, mirroring, crop, dimensions, and aspect ratio. Actual devices are **NOT VERIFIED**. |
| **High — video-profile availability is treated as YUV-session availability.** | OMT takes dimensions from the video profile at `MainActivity.java:2356–2376`. Camera video-size enumeration uses `MediaRecorder.class` at `cameracontroller/CameraController2.java:3419–3439`; the OMT reader uses YUV. The session adds preview and other outputs before OMT at `:6844–6868`. Individually supported sizes do not prove this session combination succeeds. | Select UHD, log the YUV size/minimum-duration support and complete surface combination, then require session configuration and actual received frames. |
| **High — “streaming started” precedes camera-session success; high-speed mode omits OMT.** | Manager reports started at `OMTStreamingManager.java:230–237`; surface attachment follows at `MainActivity.java:2376`. OMT is excluded when `want_video_high_speed` at `cameracontroller/CameraController2.java:6733–6737,6864–6868`. Users can see an active state without a producing camera target. | Exercise high-speed and deliberately unsupported combinations. Require an explicit unsupported/configuration-failed state, not an indefinite active state with zero video. |
| **High — codec limits exceed transport admission limits.** | VMX admits up to 7680×4320 at `libvmx/src/vmxcodec_common.h:39–42`. Sender encode capacity reaches 132,710,400 bytes at `libomt/cpp/OMTSender.cpp:45–50,287–304`. Channels reject an entire record above 10,485,760 bytes at `libomt/cpp/OMTConstants.h:53` and `OMTChannel.cpp:220–225`. Large encoded frames can be discarded even after successful encoding. Whether real UHD/8K payloads cross that limit is **NOT VERIFIED**. | Generate high-detail/noisy frames at each quality; record encoded lengths. Separately test transport with exact boundary-sized records so compression cannot hide the limit. |
| **High — bounded queue count does not establish bounded total memory or producer cost.** | Each channel owns four buffers at `libomt/cpp/OMTChannel.cpp:64–67`; buffers grow at `OMTAsyncPool.h:28–31`; payloads are copied per subscriber at `OMTChannel.cpp:258–265`. Sender holds `channelsMutex_` during fan-out at `OMTSender.cpp:638–647`. Large frames and many subscribers increase allocation, copying, and retained memory. | Measure allocation count, retained bytes, and producer duration with one fast subscriber plus multiple stalled subscribers and large payloads. |
| **Medium — network protections exist, but end-to-end isolation still needs proof.** | Whole-write deadlines and connection checks exist at `libomt/cpp/OMTChannel.cpp:322–375`; oldest queued frames are evicted at `:228–245`; metadata snapshots release the sender lock at `OMTSender.cpp:663–669`. However, `cleanupDisconnected()` destroys removed channels on its caller after unlocking at `:207–217`, and `send()` invokes it at `:578`. Source-level protection is not a measured camera-thread latency guarantee. | Run fast, stalled, and trickling subscribers together; force disconnections during production. Measure fast-subscriber freshness and producer latency, including cleanup. |
| **High — lifecycle ownership can race frame processing.** | Listener processing occurs on a handler at `OMTStreamingManager.java:228,330–357`; synchronized stop closes the reader and clears the frame buffer at `:252–259,534–541`, while the listener is not synchronized with it. Surface removal starts asynchronous reopening at `preview/Preview.java:2237–2258`, before manager cleanup at `MainActivity.java:2389–2391`. A close/use race is plausible; an actual failure is **NOT VERIFIED**. | Pause a callback immediately after image acquisition, stop/reconfigure, then release it. Check image lifetime, buffer ownership, callback generation, and bounded teardown. |
| **High — permission/lifecycle coverage across API 21–36 is unproven.** | Min SDK is 21 and compile/target SDK are 35 at `app/build.gradle:4,9–10`. Permission handling branches at API 23 at `PermissionHandler.java:114–135`. `MainActivity.onPause()` pauses preview at `:1949`, while OMT full teardown is in destruction at `:1439–1440`. These code paths do not prove permission revocation, background/resume, or API 36 behaviour. | Run permission deny/grant/revoke, background/resume, process recreation, and rapid restart scenarios on the specified API matrix. Physical camera-service behaviour remains device-only. |
| **High — errors still do not establish a truthful streaming state.** | The new manager logs failed sends at `OMTStreamingManager.java:438–445`; the periodic UI callback observes network drops at `:508–519`. Sender validation/encoding failures use a different counter at `libomt/cpp/OMTSender.cpp:598–603`. Also, no connections returns success at `:580`, so a true JNI result does not prove received video. | Force pack failure, encoder failure, no viewers, connected-but-unsubscribed viewers, and queue loss. Require distinct observable outcomes and receiver-confirmed video for acceptance. |
| **High — x86_64 Android is not configured.** | `app/build.gradle:19` selects only arm64-v8a; CMake always selects the ARM translation unit and defines ARM64 at `app/src/main/cpp/CMakeLists.txt:18–20,31–38`. Host QA selects x86/AVX2 sources at `qa/build-host.sh:58` and globally uses Haswell at `:33`. That does not establish Android x86_64 compatibility or baseline-CPU safety. | Clean-build both Android ABIs, inspect ELF architectures and compile commands, then execute x86_64 without ARM translation and without requiring host-only ISA assumptions. |

Existing protections must remain intact: codec-specific bounds and 2020 rejection in `libomt/cpp/OMTSender.cpp:450–558`, 64-bit encode sizing at `:287–304`, and `VMX_SaveTo` result validation at `:401–418`.

The current `<OMTInfo>` generator contains product/manufacturer/version, not colour information. Do not invent an existing XML colour lie: the present colour declaration is in `VideoExtHeader`. Evidence: `libomt/cpp/OMTSender.cpp:625–633,743–749`; default XML at `OMTSender.h:69`.

**2. Ordered implementation task list**

All RED/GREEN outcomes below are requirements for future runs, **NOT VERIFIED results**. For already implemented protections, RED means a deliberately isolated regression/mutant run; it does not mean weakening production validation.

For every task: compile failure, missing test, unavailable device, missing subscription, or unrelated crash must not count as the expected RED. Keep existing numeric tolerances unchanged. Android host native QA remains **ASan-only**, matching `qa/build-host.sh:31–33`.

1. **Make the evidence runner distinguish a killed mutant from broken infrastructure.**

   **Change:** `qa/build-host.sh::score_run`, currently treating every nonzero exit as failure at `:87–101`; add scenario IDs, expected assertion/ASan signatures, source hashes, command lines, and explicit setup failures.

   **RED → GREEN:** A deliberately unexecuted scenario, occupied port, unrelated timeout, or wrong assertion must fail the harness’s own self-test. Afterward, only the intended behavioural failure counts as a killed mutant.

   **Anti-vacuity:** Accepting any nonzero status lets broken setup masquerade as proof. Require a reached-scenario marker and the expected failure class.

   **Proof:** Host.

2. **Freeze the capture-to-wire contract and fixture vocabulary.**

   **Change:** Add `qa/ANDROID-FORMAT-CAPABILITIES.md` and reusable fixture descriptors covering format, plane windows, crop, dimensions, strides, bit depth, colour provenance, timestamp domain, and session generation. Tie them to `OMTStreamingManager.java:389–436`, `OMTBridge.cpp:209–243`, and `OMTSender.h:46–58`.

   **RED → GREEN:** A contract test must reject an incomplete descriptor and detect any advertised capability without a corresponding executable case. Complete descriptors and supported cases then pass.

   **Anti-vacuity:** Documentation that is never checked can claim anything. Remove one required field or test mapping and require RED.

   **Proof:** Host; device observations populate fixtures later.

3. **Complete the existing packer instead of duplicating the planar fix.**

   **Change:** `app/src/main/cpp/omtnv12.h::packNv12`, especially `:130–158,180–195`. Read U and V samples independently using their own descriptors; write one canonical NV12 buffer. Handle planar, UV-backed, VU-backed, separate stride-2 buffers, padding, and safely representable positive strides. Validate malformed geometry explicitly.

   **RED → GREEN:** Add `qa/yuv-layout.cpp`: exact-byte fixtures for all those shapes, distinct U/V values, changing rows, and unequal Y/chroma strides. The current U-only interleaved branch must fail appropriate cases; the completed implementation must match the oracle.

   **Anti-vacuity:** Grey images, U=V, or fixtures built by the production packer conceal ordering errors. Mutate V sampling, swap U/V, and ignore padding separately; each must turn RED.

   **Proof:** Host ASan; emulator/device integration follows.

4. **Prove source windows and final-row bounds.**

   **Change:** `omtnv12.h::Plane/packNv12` at `:53–58,124–167`, plus JNI arguments. Use checked last-sample addressing: offset plus row displacement plus column displacement plus sample size. Never require inaccessible padding after the final valid sample.

   **RED → GREEN:** Guarded fixtures with exactly the last readable sample must pass; one-byte-short fixtures must reject before reading. Include source windows, padding, and large stride arithmetic.

   **Anti-vacuity:** Oversized vector allocations hide overreads. Use exact allocations/guard boundaries and canaries; remove a source-bound check and require the intended RED.

   **Proof:** Host ASan. Android documents that plane buffers need not include trailing stride padding after the last pixel: [Image.Plane](https://developer.android.com/reference/android/media/Image.Plane#getBuffer()).

5. **Validate JNI buffers independently of Java’s claims.**

   **Change:** `OMTBridge.cpp::nativeSendFramePlanes` at `:225–243`, deprecated `nativeSendFrame` at `:180–196`, and `OMTSender.java::sendFramePlanes` at `:167–197`. Check nonnegative lengths, real direct-buffer capacities, readable windows, destination size, and all narrowing conversions.

   **RED → GREEN:** Add JNI instrumentation cases with overstated lengths, null/non-direct buffers, slices, nonzero positions, and insufficient destination capacity. All invalid cases must fail before packing; valid sliced inputs must follow the documented window contract.

   **Anti-vacuity:** Testing only `packNv12()` bypasses the unsafe JNI translation. Remove the native capacity check while retaining Java checks and require an independent native-boundary test to fail.

   **Proof:** Host for extracted checks; emulator for actual JNI.

6. **Enforce image format and crop handling at capture entry.**

   **Change:** `OMTStreamingManager.java::sendFrame` at `:384–436`; extend the pack descriptor for crop. Accept supported 8-bit YUV explicitly. Implement chroma-aligned crops; reject unsupported odd-origin/odd-size crops until their resampling policy is implemented.

   **RED → GREEN:** Inject non-YUV formats and crops with distinguishable borders. Supported crops produce exact expected pixels and dimensions; unsupported formats/crops return specific errors.

   **Anti-vacuity:** Full-frame-only tests pass when crop handling is absent. Mutate crop coordinates to zero and remove the format check separately.

   **Proof:** Host for geometry; emulator for Image integration; physical crop behaviour device-only.

7. **Keep layout normalization to one production operation.**

   **Change:** Retain the three-plane route at `OMTStreamingManager.java:431–436` and `OMTBridge.cpp:241–262`; eliminate any future second Java repack. Add production-path counters or test hooks for packing and buffer reuse.

   **RED → GREEN:** A test must fail when packing is bypassed, performed twice, or replaced with the deprecated NV12-claiming entry point. A sequence of distinct frames must show one normalization per admitted frame and destination reuse at stable geometry.

   **Anti-vacuity:** A helper test can pass while the app calls another function. Invoke the actual Java→JNI path and mutate its call site.

   **Proof:** Host helper tests plus emulator integration. Codec-internal planar conversion at `libvmx/src/vmxcodec.cpp:1410–1417` is a separate operation, not another Camera2 layout normalization.

8. **Introduce one colour-resolution authority.**

   **Change:** Add a tested colour resolver used by `OMTStreamingManager::sendFrame` and `OMTBridge.cpp::postFrame`; replace the height rule at `OMTBridge.cpp:68`. Carry source standard/matrix, transfer, range, bit depth, and provenance through to an immutable output descriptor.

   **RED → GREEN:** Same-resolution fixtures with known 601, known 709, unknown, contradictory metadata, and 2020 must produce the defined decisions. A height-only implementation must fail.

   **Anti-vacuity:** Using only SD=601 and HD=709 fixtures reproduces the existing heuristic. Include HD601 and SD709 cases.

   **Proof:** Host decision logic; emulator API integration; device provenance device-only. `Image.getDataSpace()` is available from API 33, so older API paths require an explicit evidence-backed policy: [Image API](https://developer.android.com/reference/android/media/Image#getDataSpace()).

9. **Define and implement range/HDR policy without relabelling pixels.**

   **Change:** Colour resolver, normalization stage, and capability document. Keep 2020 rejected at `libomt/cpp/OMTSender.cpp:552–556`. Reject unknown or incompatible transfer/range unless a separately specified conversion produces supported SDR output. Do not treat `Undefined=0` as “no interpretation”: VMX resolves it by height at `libvmx/src/vmxcodec.cpp:257–264`.

   **RED → GREEN:** Known HDR/2020 input must emit no falsely labelled 601/709 video. If full-range normalization is supported, exact pre-encode black/white/chroma vectors must match independent expected values.

   **Anti-vacuity:** Changing only the header passes superficial colour tests. Inspect transformed samples and received declarations; mutate the conversion to identity and require RED.

   **Proof:** Host policy/conversion; physical HDR production and visual colour fidelity device-only.

10. **Include colour space in encoder identity.**

    **Change:** `libomt/cpp/OMTSender.cpp::createEncoder/destroyEncoder` at `:312–351` and cached state in `OMTSender.h`. Recreate when any encoder-affecting descriptor changes.

    **RED → GREEN:** Add `qa/encoder-reconfiguration.cpp`: fixed-size BGRA, 601→709→601, plus profile changes. Compare each transition with an independently initialized reference instance and inspect creation parameters.

    **Anti-vacuity:** Resizing each frame already forces recreation; NV12-only tests may conceal RGB conversion differences. Keep dimensions fixed and include colour-sensitive RGB fixtures. Restore the dimension-only cache and require RED.

    **Proof:** Host; both ABI executions later.

11. **Derive all wire declarations from the output descriptor.**

    **Change:** `OMTBridge.cpp::postFrame` at `:59–74`; `libomt/cpp/OMTSender.cpp::send` at `:625–633`; strengthen flag/aspect validation in `validateFrame`. Retain existing validation. Reject unsupported Alpha, HighBitDepth, or interlace claims for payloads that cannot carry them. Any future format metadata must consume this same descriptor.

    **RED → GREEN:** Inject contradictory codec/flags/colour/dimensions and nonfinite/nonpositive aspect ratios. Parse emitted headers and verify supported descriptors match payload production.

    **Anti-vacuity:** A normal Android frame always has flags zero, so it cannot test contradictory flags. Mutate one header field after validation and require the receiver-side assertion to fail.

    **Proof:** Host; external receiver check later.

12. **Preserve capture timing with a documented timebase conversion.**

    **Change:** Pass image timestamps through `OMTStreamingManager::sendFrame`, Java JNI declarations, and `OMTBridge.cpp::postFrame`, replacing `:55–57`. Define session origins and discontinuity handling; correlate capture results only within compatible timestamp domains.

    **RED → GREEN:** Inject known nanosecond capture times with variable packing delays. Check exact conversion to 100 ns ticks, ordering, restart policy, and discontinuity reporting.

    **Anti-vacuity:** Testing monotonicity alone lets send-time timestamps pass. Deliberately vary processing delays; restoring `steady_clock::now()` must turn RED.

    **Proof:** Host/emulator logic; sensor-timebase behaviour device-only. Android documents source-dependent nanosecond timebases: [Image.getTimestamp](https://developer.android.com/reference/android/media/Image#getTimestamp()).

13. **Make configuration changes atomic across announcement and streaming.**

    **Change:** `MainActivity.java:2356–2376,7100–7104`; `OMTStreamingManager::startAnnouncing/startStreaming` at `:116–125,206–228`; bridge configuration at `OMTBridge.cpp:135–140`. Introduce session generations and rational fps. Reconfigure or restart explicitly when settings change.

    **RED → GREEN:** Announce at one configuration, change fps/size/quality, then stream; repeat during reconnect. Headers and encoder settings must match the active generation, including fractional-rate fixtures.

    **Anti-vacuity:** Always recreating the manager in tests avoids the stale-announcement path. Preserve the manager and connected subscriber; disable the generation update and require RED.

    **Proof:** Host state tests plus emulator.

14. **Negotiate OMT capture capabilities separately from recording profiles.**

    **Change:** `MainActivity::startOmtStreaming`; `cameracontroller/CameraController2.java` capability enumeration near `:3419–3439`; add OMT-specific YUV sizes, durations, fps ranges, and full-session compatibility policy.

    **RED → GREEN:** Fake capability maps where UHD recording exists but UHD YUV or the requested surface combination does not. Select a documented supported fallback or report unsupported. Also test a valid UHD combination that must remain selectable.

    **Anti-vacuity:** A blanket 1080p cap can pass negative tests. Require positive 3840×2160 and, when advertised, 4096×2160 cases. Remove the YUV/session intersection and require RED.

    **Proof:** Host decision logic; real combinations device-only.

15. **Tie streaming state to session readiness and frame production.**

    **Change:** `OMTStreamingManager.java:230–237`; `MainActivity.java:2372–2376`; `CameraController2` session callbacks and OMT target insertion at `:6733–6741,6864–6868`. Model announcing, configuring, capturing, subscribed, delivering, and failed states. Reject high-speed configurations that exclude OMT.

    **RED → GREEN:** Force configuration failure, delayed first image, unsupported high-speed mode, and successful ordinary capture. Only successful camera production reaches the capturing state; delivery additionally requires video progress.

    **Anti-vacuity:** Checking only the toggle or callback invocation repeats the defect. Suppress OMT target attachment and require RED.

    **Proof:** Host state logic and emulator; vendor session behaviour device-only.

16. **Define orientation, mirroring, and aspect-ratio policy.**

    **Change:** Capture descriptor, packing transform, and `OMTBridge.cpp::postFrame` at `:61–68`; integrate camera/display orientation from the camera controller. Choose either explicitly documented sensor-native output or a proven upright transform. Recompute dimensions/aspect after transforms.

    **RED → GREEN:** Exact asymmetric fixtures for 0/90/180/270 degrees and front-camera mirroring; include crops and portrait dimensions against VMX’s separate width/height limits.

    **Anti-vacuity:** Symmetric charts and width/height checks alone hide rotation and mirroring mistakes. Disable the transform or swap dimensions without rotating pixels and require RED.

    **Proof:** Host transform; physical orientation and front-camera conventions device-only.

17. **Expose separate capture, pack, encode, queue, and delivery outcomes.**

    **Change:** `OMTStreamingManager.java:438–445,508–519`; JNI statistics; `libomt/cpp/OMTSender.cpp:598–603,224–238`. Preserve counters across subscriber removal and distinguish no subscribers from refused frames and queued frames.

    **RED → GREEN:** Inject each failure stage and disconnect a channel after drops. Verify distinct counters/reasons, retained totals, and UI state.

    **Anti-vacuity:** Testing only total network drops misses pack/encode failures; polling only while the channel exists misses lost history. Disable each counter update separately.

    **Proof:** Host and emulator.

18. **Serialize image, surface, buffer, and sender lifetime.**

    **Change:** `OMTStreamingManager::onImageAvailable/stopStreaming/cleanupStreaming/stopBackgroundThread` at `:330–357,252–259,458–469,534–554`; surface replacement at `preview/Preview.java:2237–2258`.

    **RED → GREEN:** Deterministically hold an acquired image while stopping, switching camera, or replacing the session. Require no read after close, exactly-once closure, no stale-generation send, and bounded teardown.

    **Anti-vacuity:** Sleep-based tests may never intersect the critical interval. Use barriers at acquisition, packing, and submission; remove the ownership/generation barrier and require RED.

    **Proof:** Host ownership tests plus emulator; OEM scheduling/device behaviour device-only.

19. **Make permission and lifecycle transitions recoverable.**

    **Change:** `MainActivity::onPause/onResume/onDestroy`, `PermissionHandler::requestCameraPermission/onRequestPermissionsResult`, and manager state handling. Anchors: `MainActivity.java:1439–1440,1912–1949,8041–8046`; `PermissionHandler.java:114–135`.

    **RED → GREEN:** Deny then grant, revoke while active, background/resume, screen off/on, camera unavailable, Activity recreation, and process restart. Require accurate state and successful restart without duplicate readers, senders, or announcements.

    **Anti-vacuity:** Pregranting permissions and never leaving the Activity proves little. Remove transition cleanup/recovery and require the relevant scenario to fail.

    **Proof:** Emulator framework coverage; physical camera-service and power-management coverage device-only.

20. **Complete checked sizing from capture allocation through wire framing.**

    **Change:** `OMTSender.java::nv12BufferSize` at `:144–149`; `OMTBridge.cpp::postFrame` at `:71–74`; `omtnv12.h::nv12Size`; `Sender::encodeBufferCapacity`, `calculateOptimalBuffer`, and frame-header length construction at `libomt/cpp/OMTSender.cpp:268–304,623`; channel length addition at `OMTChannel.cpp:220`.

    **RED → GREEN:** Test negative/zero values, odd NV12 geometry, codec-limit boundaries, `32768×16385`, extreme positive integers, header-addition overflow, and allocation failure. Reject impossible geometry before large allocation; retain checked int narrowing.

    **Anti-vacuity:** `validateFrame` can hide arithmetic defects in helper tests. Exercise sizing functions directly as well as the public path; restore narrow multiplication in isolated mutants and require exact-value failures.

    **Proof:** Host ASan plus arithmetic assertions; JNI narrowing in emulator. ASan alone is not an integer-overflow proof.

21. **Prove codec edge geometry and output-capacity contracts.**

    **Change:** Extend `qa/frame-validation.cpp`, `qa/vmx-saveto-contract.cpp`, and VMX tests around `VMX_NV12ToPlanar` in `libvmx/src/vmxcodec_x86.cpp:2184` and `vmxcodec_arm.cpp:2178`. Change codec internals only for demonstrated failures.

    **RED → GREEN:** Exercise minimum dimensions, even widths around SIMD boundaries, partial final slices, 1080-line height, padded input, UHD, and maximum dimensions. Undersized `VMX_SaveTo` output must fail without overrun; valid input must produce a parseable payload.

    **Anti-vacuity:** Large padding conceals SIMD overreads; a stub cannot prove codec execution. Require exact input bounds, the real codec, and targeted SaveTo/bounds mutants.

    **Proof:** Host x86 ASan; arm64 runtime required separately. Existing protections may already be GREEN; the required RED is their isolated mutation control.

22. **Reconcile transport limits with supported encoded payloads.**

    **Change:** `libomt/cpp/OMTConstants.h:53`, `Channel::sendAsync` at `OMTChannel.cpp:220–225`, sender capability admission, and corresponding receiver-limit documentation. Establish a checked maximum including headers and explicit oversized-payload behaviour.

    **RED → GREEN:** Test records immediately below, at, and above the supported maximum, plus real noisy UHD encodes across quality levels. Supported payloads must arrive completely; oversized payloads must produce a specific outcome without a torn stream.

    **Anti-vacuity:** Compressible charts may never reach the boundary. Use synthetic transport payloads independently of codec output. Restore the incompatible cap or omit header bytes and require RED.

    **Proof:** Host transport; receiver compatibility separately. Raising a capability limit requires memory/receiver evidence, not tolerance adjustment.

23. **Bound subscriber memory and move expensive fan-out work off global locks.**

    **Change:** `Sender::send` at `libomt/cpp/OMTSender.cpp:638–647`, `Channel::sendAsync` at `OMTChannel.cpp:228–270`, and `AsyncBuffer::resize` at `OMTAsyncPool.h:28–31`. Define per-channel/global byte budgets and subscriber admission; consider shared immutable encoded payload ownership.

    **RED → GREEN:** Large-frame, many-subscriber tests must stay within declared retained-byte budgets and keep the fast subscriber progressing. Inject allocation failures and verify recovery.

    **Anti-vacuity:** One subscriber or small payloads never stress the budget. Disable admission/byte accounting and require RED; destroying all subscribers is not an acceptable way to satisfy fast-subscriber progress.

    **Proof:** Host; device memory/performance measurements later.

24. **Extend back-pressure tests to complete UHD records and mixed subscribers.**

    **Change:** `qa/slow-subscriber.cpp`, `qa/drop-policy.cpp`, `qa/socket-write-order.cpp`; preserve production deadlines, serialization, and oldest-queued eviction. Relevant production code: `OMTChannel.cpp:228–245,322–394`.

    **RED → GREEN:** Run fast, stalled, and trickling peers together, with metadata during partial video writes, reconnection, and cleanup during production. Require complete parseable records, newest-frame freshness, bounded producer delay, and slow-peer eviction.

    **Anti-vacuity:** Assert actual subscription, bytes transmitted, socket pressure, and queue occupancy. Small payloads can fit entirely in kernel buffers. The parser must not resynchronize past corruption; `qa/drop-policy.cpp:55–62` currently scans forward on invalid headers.

    **Proof:** Host. Existing protections require targeted isolated mutants; no deadline or tolerance tuning.

25. **Implement measured encode-budget admission and explicit fallback.**

    **Change:** Timing instrumentation around packing/encoding in `OMTStreamingManager::sendFrame`, `OMTBridge`, and `Sender::encodeFrame`; replace sole reliance on network-drop-driven adaptation at `libomt/cpp/OMTSender.cpp:752–775`.

    **RED → GREEN:** Inject deterministic encode delays beyond 25/30/60 fps budgets—40 ms, approximately 33.33 ms, and approximately 16.67 ms. Require bounded buffering and a visible policy: reduce quality, renegotiate fps/size, or report unsustainable configuration. Update declarations when the selected configuration changes.

    **Anti-vacuity:** No subscriber skips encoding at `OMTSender.cpp:580`; average timing hides tail stalls. Require real encode entry and controlled delays. Disable adaptation and require RED.

    **Proof:** Host policy; sustained SoC capability, thermals, and battery effects device-only.

26. **Preserve source builds and make ABI selection explicit.**

    **Change:** `app/src/main/cpp/CMakeLists.txt:18–38,47–78` and `app/build.gradle:17–20,63–70`. Build VMX, OMT, and JNI from the selected source revision; choose ARM sources/definitions only for arm64-v8a and x86 sources only for x86_64. Remove stale prebuilt commentary. Do not invent a deletion for absent `.so` files.

    **RED → GREEN:** A clean build requesting both ABIs must fail before support is added and pass afterward. Inspect produced ELF machine types and dependencies.

    **Anti-vacuity:** Incremental artifacts or a task that builds only arm64 can appear successful. Use empty output/cache locations and assert both ABI outputs exist.

    **Proof:** Build-host; runtime loading requires Android execution.

27. **Make the x86 path safe for the declared CPU baseline.**

    **Change:** Per-source compiler flags in CMake and VMX ISA dispatch. Anchors: `libvmx/src/vmxcodec.h:35–40`, `vmxcodec.cpp:231–253`, and host-only global Haswell flags at `qa/build-host.sh:33`.

    **RED → GREEN:** Build/run baseline x86_64 plus supported accelerated configurations. Verify unavailable AVX2/BMI2 paths are not executed and each enabled implementation passes the common codec corpus.

    **Anti-vacuity:** A Haswell-capable host masks accidental global ISA requirements. Run with constrained CPU features or equivalent controlled runners; force the dispatch mistake and require RED.

    **Proof:** Host CPU configurations and native x86_64 Android emulator. Physical x86 Android coverage is **NOT VERIFIED**.

28. **Gate both ABIs on identical source provenance.**

    **Change:** Add CI/build scripts, a generated build identity, and artifact verification around `app/build.gradle` and the native CMake targets. Record source-tree hashes, dependency revisions, NDK/CMake versions, and compile manifests for both ABIs.

    **RED → GREEN:** Deliberately package a stale library, omit one ABI, change one ABI’s source revision, or inject an imported prebuilt target. The provenance gate must fail; clean dual-ABI outputs from the same source manifest must pass.

    **Anti-vacuity:** ELF architecture or a version string alone does not prove source identity. Hash the actual inputs and correlate packaged binaries with produced artifacts. ABI binaries are not expected to have identical hashes.

    **Proof:** Host CI plus runtime build-identity check. Use the pinned NDK’s runtime libraries rather than repository-committed runtime binaries.

29. **Add an emulator acceptance path that requires video.**

    **Change:** Add `qa/android-emulator-e2e` and OMT instrumentation tests using the actual manager/JNI path at `OMTStreamingManager.java:431–436`; use an independent wire parser for `OMTProtocol.h:19–43`.

    **RED → GREEN:** Clean-install native x86_64, enable streaming, subscribe, and require a sequence of complete decoded frames with changing identifiers. Exercise injected layout fixtures, restart, permission transitions, and configuration changes.

    **Anti-vacuity:** Greeting, XML, discoverability, positive `send()` status, and an active toggle are insufficient. Disable camera-to-JNI submission or video subscription and require RED.

    **Proof:** Emulator: application/framework/JNI/transport integration. It does not prove a physical vendor HAL, physical-camera colour, thermals, or radio behaviour.

30. **Run and enforce the physical-device evidence matrix.**

    **Change:** Add `qa/device-matrix.md`, a machine-readable result schema, and instrumentation exporting observations from capture, encoder, and subscriber stages. Extend the diagnostics near `OMTBridge.cpp:252–259` to record actual descriptors rather than infer layout solely from pixel stride.

    **RED → GREEN:** The report gate must reject unsupported acceptance claims, absent required results, or results without received video. Required physical rows become GREEN only with attached passing runs; unsupported features must be explicitly recorded.

    **Anti-vacuity:** An announcing-only RMX3867 run, emulator result relabelled as physical, or skipped UHD case must fail the evidence validator. Mutation tests validate that validator; they do not replace physical measurements.

    **Proof:** Physical-device behaviour is device-only; report validation is host-provable. Minimum matrix appears below.

31. **Verify interoperability independently of the sender’s own assumptions.**

    **Change:** Add `qa/receiver-interop` fixtures and scripts covering header parsing, colour/range charts, flags, reconnect, and format changes. Anchor the wire contract to `OMTProtocol.h:19–43,73–96` and payload production to `OMTSender.cpp:364–405`.

    **RED → GREEN:** A deliberately mismatched colour/flag/dimension declaration and a truncated payload must fail; genuine supported streams must display and decode through a separately built receiver, including the intended desktop/OBS path.

    **Anti-vacuity:** Sender/receiver loopback sharing the same fallback can agree incorrectly. Use independent pixel expectations and explicit header inspection. Do not tune numerical gates.

    **Proof:** Host for synthetic streams and receivers; physical capture fidelity device-only. Actual desktop/OBS acceptance is **NOT VERIFIED**.

32. **Publish a precise UHD/8K support statement and final acceptance gate.**

    **Change:** `qa/ANDROID-FORMAT-CAPABILITIES.md`, device matrix, release checklist, and CI aggregation. Cite codec limits at `libvmx/src/vmxcodec_common.h:39–42`, UI filtering at `CameraController2.java:3432–3433`, transport admission, memory budgets, and measured configurations.

    **RED → GREEN:** Acceptance must fail if “UHD supported” lacks a successful negotiated-session and received-video run, if “8K supported” means only encoder dimension admission, or if a required mutation control survives. It passes only for the explicitly evidenced scope.

    **Anti-vacuity:** Automatically converting skipped tests into supported features defeats the gate. Remove one required evidence artifact or substitute a stale source hash and require RED.

    **Proof:** Host aggregation; performance/capture claims inherit device-only requirements.

**Required format/colour capability matrix**

This is the target policy to implement and test, not a claim that it already exists.

| Input/provenance | Required action | Output declaration |
|---|---|---|
| Supported 8-bit YUV_420_888; planar, UV-backed, VU-backed, padded or independently strided planes | Sample each component correctly; normalize once to owned NV12 | NV12 input to VMX; VMX1 on wire; actual output geometry |
| Proven 601 SDR with compatible range | Preserve samples or perform explicitly tested range normalization | 601 |
| Proven 709 SDR with compatible range | Preserve samples or perform explicitly tested range normalization | 709 |
| Unknown/contradictory colour provenance | Apply a documented, evidence-backed policy or reject; never infer truth solely from height | No fabricated 601/709 claim |
| 2020, HLG, PQ, unsupported wide-gamut/transfer combination | Fail closed; optional future conversion needs its own independent acceptance work | Never forward 2020 into the current sender |
| Full-range SDR | Explicitly normalize if supported, otherwise reject | Declaration matching resulting samples |
| JPEG/blob, PRIVATE, RAW, P010 or another unsupported input format | Reject before the YUV packer | No video payload |
| Unsupported alpha/high-bit-depth/interlace flags | Reject or use a separately implemented compatible encoding path | Flags matching actual payload |

Current implementation anchors: format admission at `OMTStreamingManager.java:223–227`; NV12 packing at `omtnv12.h:91–204`; accepted sender colour values at `OMTSender.cpp:552–556`; flags at `OMTProtocol.h:73–78`; VMX’s undefined-space fallback at `vmxcodec.cpp:257–264`. The full-range transform and physical-camera colour interpretation are **NOT VERIFIED**.

**UHD/8K arithmetic and acceptance scope**

For tightly packed NV12, the proposed sizing tests must pin these exact values:

| Geometry | Raw NV12 bytes | Current sender encode-buffer sizing |
|---|---:|---:|
| 3840×2160 UHD | 12,441,600 | 33,177,600 |
| 4096×2160 | 13,271,040 | 35,389,440 |
| 7680×4320 8K | 49,766,400 | 132,710,400 |

These are arithmetic consequences of NV12 sizing at `omtnv12.h:209–212` and sender sizing at `OMTSender.cpp:287–304`; they exclude padded input, ImageReader allocations, codec working memory, and subscriber buffers. The 8K encode ceiling is **126.5625 MiB**, below `INT_MAX`, as constrained at `OMTSender.cpp:45–50`.

The existing 10 MiB channel check applies to the encoded record, not the raw NV12 input: `OMTChannel.cpp:220–225`. Raw UHD exceeding 10 MiB therefore does not by itself prove rejection.

Document 8K as **codec-dimension admission only** until capture selection, memory, transport, receiver, and runtime evidence exist. The current normal video-size enumeration filters dimensions above 4096×2160 at `CameraController2.java:3432–3433`. Sustained UHD/8K at any specific fps is **NOT VERIFIED**.

**Device matrix without a lab**

Record source/APK hashes, actual ABI, API level, camera ID, physical/logical camera identity where available, chosen surface combination, dimensions/crop, all plane windows/strides, colour provenance, capture timestamps, pack/encode duration, payload sizes, drop reasons, subscriber state, and received frame identifiers.

| Lane | Required cases | What it can establish | What it cannot establish |
|---|---|---|---|
| Host ASan | All synthetic layouts/windows; arithmetic; colour decisions; encoder transitions; wire framing; mixed subscribers | Deterministic implementation correctness for exercised inputs | Real Android HAL behaviour or phone performance |
| Native x86_64 emulator | Representative API 21/23/29/33/34/35/36 images where available; JNI; permission/lifecycle; injected planes; decoded wire frames | Framework/app integration on those images | Physical vendor HAL, sensor colour, thermal sustainability, real Wi-Fi |
| Physical RMX3867 | Back/front; SD/HD/UHD if offered; all quality levels; 25/30/60 only where supported; restart and permission transitions | The owned device’s observed layouts and supported configurations | Other vendors or untested camera IDs |
| Borrowed older phone | API 21–28 where obtainable; limited camera capabilities; permission/lifecycle cases | Older physical camera-service and HAL coverage | New HDR/profile behaviour |
| Borrowed different-vendor phone | Different SoC/vendor; front/back; padded/semiplanar observations; Wi-Fi stress | Additional physical variance | Universal ecosystem support |
| HDR-capable physical phone | Supported SDR/HDR configurations; colour metadata; explicit rejection of unsupported output | Actual HDR rejection/configuration behaviour | HDR conversion quality unless separately implemented and tested |
| UHD performance run | Warm-up, sustained streaming, thermal change, fast plus slow subscribers, fallback | Measured sustainable configurations and recovery | An unconditional 4K60 claim |
| 8K-capable device, optional | Only after host/build/transport prerequisites; report unsupported explicitly | Device-specific 8K evidence | General 8K support |

An emulator uses an emulated camera implementation; it cannot establish the behaviour of a physical vendor HAL. The dossier’s translated arm64 emulator setup is also not evidence that a native x86_64 APK works: `qa/ANDROID-FORMAT-ROBUSTNESS-DOSSIER.md:10–12`.

Unavailable matrix cells remain **NOT VERIFIED**, with acceptance claims scoped accordingly. Existing physical evidence for RMX3867 is insufficient according to the dossier itself: `:60–62`.

**3. Sequencing and parallel safety**

The task numbers are implementation order within their dependency chains, not a requirement to serialize every independent lane.

| Lane | Order/dependencies | Parallel-safety rule |
|---|---|---|
| Evidence foundation | 1 → 2 | Start first. Establish scenario names, descriptors, provenance, and failure classification before accepting any GREEN result. |
| Packing/JNI | 2 → 3 → 4 → 5 → 6 → 7 | One owner for `omtnv12.h`, Java/JNI signatures, and integration. Extend the in-progress planar work; do not independently recreate it. |
| Colour/payload honesty | 2 → 8 → 9 → 10 → 11 | Resolver and fixtures can develop alongside packing. Integrate JNI descriptor changes with the packing owner. Serialize sender-cache/header changes. |
| Timing/configuration | 2 → 12 → 13 → 14 → 15 → 16 | State/negotiation tests can run independently. One owner must integrate manager, Activity, and camera-session changes. |
| Observability/lifecycle | 13/15 → 17 → 18 → 19 | Tests can be authored early. Production edits overlap the manager lane and require ordered integration. |
| Size/codec/transport | 2 → 20 → 21 → 22 → 23 → 24 | Arithmetic and codec fixtures can start early. Serialize changes to sender/channel ownership and capacity contracts. |
| Performance policy | 13/14/17/20/22/24 → 25 | Measure only after delivery and accounting are trustworthy. Define policy before collecting acceptance results. |
| Build/provenance | 26 → 27 → 28 | Safe alongside most Java work. Coordinate CMake/source-list changes with native owners; do not edit ISA code concurrently with codec fixes. |
| Integration acceptance | 5–19 and 28 → 29; then 30/31 → 32 | Physical measurements may start earlier as exploration, but final acceptance must use the integrated source/artifact identity. |

Additional rules:

- **Do not copy files between Android and desktop vendored trees.** Use reviewed semantic patches or a deliberately versioned shared-source dependency, with each tree’s tests run independently. The no-copy constraint is explicit at `qa/ANDROID-FORMAT-ROBUSTNESS-DOSSIER.md:103–106`.
- Keep one owner for production `Sender::validateFrame` changes. Preserve its existing restrictions; any new validation is additive. Mutation builds live in isolated test workspaces.
- Give parallel QA jobs distinct scratch directories and ports. The current runner defaults to shared `/tmp/omt-qa` at `qa/build-host.sh:10`; tests use fixed ports, for example `qa/colorspace-gate.cpp:79`, `qa/drop-policy.cpp:70`, and `qa/frame-validation.cpp:201`.
- Build mutants with matching source and headers. The runner already documents the mixed-header/member-layout hazard at `qa/build-host.sh:302–305`.
- Preserve existing numeric gates. An expected RED must demonstrate the intended defect; no increased tolerance, weakened deadline, missing receiver, or skipped physical run may manufacture acceptance.

VERDICT: NOT READY

---

## Parent verification of the four highest-value claims (2026-10-15, read from the working tree)

| Astra claim | Parent-verified? | What was actually seen |
|---|---|---|
| Encoder reuse ignores colour space | **YES** | `libomt/cpp/OMTSender.cpp:315-317` — reuse guard is `vmxInstance_ && currentWidth_ == width && currentHeight_ == height`; `colorSpace` is absent from the key while `createEncoder` passes `cspace` into `VMX_Create`. A same-size 601→709 switch therefore encodes with the previous matrix and the header advertises the current one. |
| Transport admission limit below codec limits | **YES** | `libomt/cpp/OMTConstants.h:53 — constexpr int VIDEO_MAX_SIZE = 10485760; // 10MB max frame` vs `VMX_MAX_WIDTH 7680 / VMX_MAX_HEIGHT 4320`. The UHD ceiling is the transport admission, not the encoder. |
| Colour space inferred from height | **YES** | `app/src/main/cpp/OMTBridge.cpp:68 — frame.ColorSpace = (height >= 720) ? OMTColorSpace_BT709 : OMTColorSpace_BT601;` — no capture metadata, no transform. |
| JNI trusts caller-supplied plane lengths | **YES** | `app/src/main/cpp/OMTBridge.cpp:225-236` — `dst` capacity is checked via `GetDirectBufferCapacity(dstBuffer)`, and all four addresses are null-checked, but `ySize/uSize/vSize` go straight into `omtpack::Plane{...}` with no `GetDirectBufferCapacity()` comparison against their own source buffers. |

Also noted: the review ran while `fix/omt-bridge-planar-yuv` was mid-flight in this checkout (it recorded file hashes and said so), so its `omtnv12.h` findings describe an in-progress packer, not a merged one.
