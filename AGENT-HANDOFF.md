# Agent handoff — opencamera-omt

Audited and improved 2026-10-15 by an autonomous coding agent on a Linux box
(32 cores, no Android device attached). This file is the entry point for whoever
picks this up next: it says what the code does, what was changed, how it was
verified, **what was not verified**, and what is left.

Branches: `https://github.com/yd-zd/opencamera-omt` (fork; `ysdede/opencamera-omt`
is the upstream and the reviewing account has no write access to it).
Top of the stack: **`fix/omt-announcement-truthfulness`** — it contains all 8
commits below, stacked in dependency order so each one builds on its own.

```
289ff28b  fix(omt): an announcement that never registered no longer claims to be discoverable
cea02494  fix(omt): a custom Stream Name is now one OBS can actually see
0f2fdabc  fix(omt): tear the stream down when the activity dies — the reboot half of issue #9
1e565fcd  perf(omt): drop the OLDEST queued frame, and make drops actually visible
b66ab347  fix(omt): the sender now refuses what it cannot encode, says why, and counts it
45b8b48e  fix(omt): a stalled subscriber can no longer hold the stream hostage
df66b01f  fix(vmx): DestroyTasks leaked the ThreadTask*[] array
367b2be7  fix(build): drop the checked-in libvmx.so prebuilt that collided with the CMake target
```

## How the streaming path works

```
Camera2 session ──▶ ImageReader(RGBA_8888)                     OMTStreamingManager.java
                        │  onImageAvailable (capture thread)
                        ▼
                  OMTSender (JNI)                              app/src/main/cpp/OMTBridge.cpp
                        │  static omt_send_t* g_sender   ← one per PROCESS, not per object
                        ▼
                  omt_send_* C API                             libomt/cpp/libomt_android.cpp
                        ▼
                  Sender ──▶ Channel per subscriber            libomt/cpp/OMTSender.cpp
                                └ AsyncPool: 4 slots × ~1 MB   libomt/cpp/OMTChannel.cpp
                        ▼
                  VMX intra-frame codec (NEON)                 libvmx/src/*
                        ▲
      DNS-SD  _omt._tcp : 6400   registered by NsdManager in Java
```

Things a new agent will otherwise get wrong:

- **The advertised instance name must contain both `(` and `)`.** Conforming
  receivers (`libomtnet`'s `OMTAddress.IsValid()`) discard records without a
  matched pair **silently** — no error, no log, the source just never appears.
  `OMTStreamName.java` normalises user input; the auto name is
  `"<device> (Camera)"`.
- **Colour-space numbering on the wire is 0 / 601 / 709** (`OMTColorSpace` in
  `libomt.h`, same values in `VMX_COLORSPACE`). It is *not* the
  `BT601=1/BT709=2/BT2020=3` set that `OMTProtocol.h` used to declare. VMX cannot
  encode BT.2020 at all, so anything else must fail closed at the sender boundary.
- **Port 6400 is fixed.** `NETWORK_PORT_END=6600` in `OMTConstants.h` is dead
  code — there is no scan or fallback, so a busy 6400 means no announcement at
  all. `SO_REUSEADDR` is already set; TIME_WAIT is not why a port gets stuck.
- **`g_sender` is process-global** while the Java `isInitialized` flag is
  per-instance, so an activity recreation while streaming produces two managers
  over one sender.
- **`stopAll()` is the only teardown** (unregister + 3 locks + native destroy).
  For the entire history of this repo it had zero callers; `stopStreaming()`
  deliberately keeps the announcement alive.

## Build & verify (all three, from a clean checkout)

```bash
export ANDROID_HOME=/home/reality/android-sdk          # ndk;27.0.12077973, cmake;3.22.1
./gradlew --no-daemon assembleDebug                    # fails if jniLibs/ ever regains a prebuilt
bash qa/build-host.sh                                  # host ASan/UBSan: fixed / pre-fix / mutants
./gradlew --no-daemon --rerun-tasks :app:testDebugUnitTest \
    --tests "net.sourceforge.opencamera.OMTStreamNameTest"
# then read app/build/test-results/testDebugUnitTest/TEST-*.xml — not the console
```

`qa/build-host.sh` compiles the vendored C++ on the host and runs each scenario
against the fixed build **and** a build with the fix reverted: the reverted run
must fail, or the test proves nothing. Add your own mutants when you add a gate.

## What was verified, and what was not

Verified with real sockets on the host (ASan/UBSan), including the reverted-fix
control: stalled-subscriber eviction (measured ~1008 ms; before the fix: never),
drop-oldest ring behaviour (newest frame survives; before: up to ~133 ms of stale
video replayed), colourspace refusal + failure counting, `frameRateD == 0`
refusal, the codec task-array leak (ASan clean).

**Not verified: anything in Java.** The lifecycle and discovery fixes
(`0f2fdabc`, `cea02494`, `289ff28b`) are compile-verified and unit-tested only
where a pure helper was involved. The device sequence that actually closes issue
#9 — start streaming → swipe the app away from recents → relaunch → is the source
listed in OBS without a reboot — has never been run. Do not report issue #9 as
fixed until someone runs it on a phone.

## Known-bad, deliberately not touched here

See `docs/known-issues.md` (same commit). Short version: no foreground service
behind the sender; changing the stream name or disabling OMT does not
re-register/unregister; `ImageReader` closed while the session still targets its
surface; camera error / Home leaves `isStreaming=true` with zero frames; NV12
assumed for `YUV_420_888` without checking `pixelStride`; encoding happens on the
capture thread under `g_senderMutex`; `SO_SNDBUF` forced off kernel auto-tuning;
`test_project` intent extra honoured from an exported activity; `MyDebug.LOG`
ships `true`.

## Relationship to the nodos-yded OMT code

The same `libomtcpp` / `libvmx` sources are vendored into the nodos
`yd.net.ompout` plugin (Linux sender + `OmpIn` receiver). **They have diverged** —
`OMTChannel.cpp` is 572 lines there vs 489 here, `OMTSender.cpp` 782 vs 570 — so
fixes do not propagate themselves:

- codec task-array leak: fixed on the desktop side first, ported here (`df66b01f`) ✓
- send deadline, TCP keepalive, colourspace gate, `frameRateD` guard: already
  present on the desktop side, re-implemented here
- **drop-oldest: exists only here.** The desktop `sendAsync` still does
  `acquire(); if (!buf) { framesDropped_++; return 0; }` and `AsyncPool::acquire()`
  never evicts, so the Linux sender still keeps the 4 oldest frames and throws the
  newest away. Port it with its own pre-fix control.
