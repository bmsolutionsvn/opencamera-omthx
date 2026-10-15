# Encoder colour-space identity and wire-declaration honesty — evidence record

Date: 2026-10-15. Branch: `fix/omt-encoder-colorspace-key` (worktree
`/home/reality/.hermes/cache/wt-color-honesty`, base `origin/master` = `d9814679`).
Scope: the first of the four parent-verified Astra claims
(`qa/ANDROID-FORMAT-PLAN-ASTRA-20261015.md`, "encoder reuse ignores colour-space changes") plus the
declaration-honesty family around it. Files touched: `libomt/cpp/` and `qa/` only.

## 1. The defect

`Sender::createEncoder()` reused a live VMX instance on a key of width + height alone
(`libomt/cpp/OMTSender.cpp:315-317` on the base commit):

```cpp
if (vmxInstance_ && currentWidth_ == width && currentHeight_ == height) {
    return true;  // Already have correct encoder.
}
```

while the colour space it was handed went into the encoder (`:324`,
`VMX_Create(size, profile, cspace)`) and the *current frame's* colour space went into the header
(`:633`, `extHeader.colorSpace = frame.colorSpace`). VMX selects its RGB↔YUV table from the
create-time value (`libvmx/src/vmxcodec.cpp:1448/1466/1654/1673`:
`if (instance->ColorSpace == VMX_COLORSPACE_BT601) colorTable = RGB_YUV_601;`), so at one size a
601→709 transition kept encoding with the 601 matrix and advertised 709 to every subscriber. Same
family as the desktop's lying Alpha flag: declaration and payload disagreeing, and a same-codec
loopback structurally unable to notice because both ends share the mistake.

### Measured on host, before the fix

`qa/encoder-reconfiguration.cpp` against pristine `origin/master` (`er-prefix`, exit 1). 64×64
saturated RGB bars, BGRA, dimensions fixed, one connected video subscriber, the real codec linked
(not the stub). Payloads are compared against reference encoders created by calling libvmx directly
in the same process, so the sender cannot conspire with its own witness:

| assertion | expected | got (unmodified tree) |
|---|---|---|
| SA5 header of the frame sent after 601→709 | declares 709 | **declares 709** (the lie is the premise, not the finding) |
| SA6 payload of that frame | the 709 reference: 621 bytes | **717 bytes** = the continued-601 encoder's second frame, byte for byte |
| SA7 that payload ≠ continued-601 (717 B) | different | **identical** |
| SB4 reverse hop 709→601 | the 601 reference: 665 bytes | **657 bytes** = the continued-709 encoder's second frame |
| SC8/SC9 (601→709→601 hops 2,3) | 621 / 665 | **717 / 725** (previous matrix, continued) |
| SE1…SE5 flag claims, SF1…SF4 aspect ratios | refused, counted, named | **accepted**, uncounted, `lastError()` empty |

The fixture's discriminating power is asserted, not assumed: `R0b` fails the file if a 601 and a 709
encode of the fixture ever produce the same bytes (a grey fixture produces byte-identical payloads
under both matrices — measured — and would have made the whole test vacuous). `R0c` pins that equal
history produces equal bytes, which is what makes byte equality a witness at all: VMX has per-frame
rate control, so the Nth payload depends on its history, and every comparison in the file is
history-matched (continued-vs-continued, fresh-vs-fresh).

## 2. The fix

1. **Colour space enters the encoder identity** — `createEncoder()` now reuses only when
   `(width, height, colour space)` all match, and `currentColorSpace_` is published with the instance
   and cleared to `-1` (not 0, which is a legal colour space) by `destroyEncoder()`. Profile stays
   out of the key because every path that changes it (`setQuality`, `onQualityChanged`,
   `checkCongestion`) calls `destroyEncoder()`; `wd-fixed` WC3 pins that behaviour rather than
   trusting the comment.
2. **The declaration is built from the producer** — new pure `Sender::buildVideoHeaders(frame,
   encoderIdentity, encodedLen, …)` is the only place the two wire headers are assembled.
   `ext.colorSpace`, `ext.width`, `ext.height` come from the identity that `encodeFrame()` captured
   under the same `encoderMutex_` hold as the encode, never from the caller's struct. `send()` no
   longer stamps anything into the headers itself.
3. **Fail closed on a contradiction** — if the frame claims something the encoder behind the payload
   was not created as, the frame is refused, counted in `encodeFailures()` and named in
   `lastError()`, like every other refusal. The sender does not repair a claim; it declines to make
   one. A belt inside the builder also refuses a colour space outside 0/601/709 without assuming
   `validateFrame()` ran.
4. **The rest of the header's promises are gated at the boundary** (additive clauses appended after
   the existing colour-space gate in `validateFrame()`; nothing existing was weakened or reordered):
   undefined flag bits, `HighBitDepth` (every input this sender encodes is 8-bit), `Alpha` on a codec
   with no alpha plane (only the BGRA path fills VMX plane 3), `PreMultiplied` without `Alpha`,
   `Interlaced` at a height `VMX_ConfigureInterlaced` (`libvmx/src/vmxcodec.cpp:501-510`) will not
   interlace, and a non-finite or non-positive `aspectRatio`. These run before the no-subscribers
   shortcut, so a frame that would have lied on air is refused on the bench.
   `OMTProtocol.h`'s `VideoFlags` also carried a wrong number: `HighBitDepth = 4` — which is
   `PreMultiplied` on the wire — and no `PreMultiplied` at all. Corrected to the authority
   (`libomt.h:135-144`) and pinned at compile time, the same way the colour numbering is pinned. The
   constant was unread by production code before this change, which is why it was latent and why it
   had to be fixed in the same commit as the gate that now reads it.

## 3. After the fix

Same scenario, same machine, same command (`er-fixed`, exit 0): SA6 gets **621** (the 709 reference)
and SA7 confirms it is not the 717-byte continued-601 payload; SB4 gets **665**; SC7/SC8/SC9 get
665/621/665. `SD*` still matches the *continued* reference for frames whose descriptor did not
change, which is what stops the fix from being "recreate every frame"; `SG*`/`SH*` confirm the
legitimate flag combinations and the exact NV12 descriptor `OMTBridge.cpp` posts today are still
accepted.

## 4. Mutation matrix

`bash qa/build-host.sh` on this branch, `SCRATCH=/tmp/omt-qa-full2`, NDK clang++ with ASan (the
runner's standing configuration — nothing was added for this branch):

```
QA PASSED (every gate has a scenario where disabling it turns the suite red)
46 scored runs (24 pre-existing + 22 added here), exit 0
```

Baseline for comparison: the same command on the pristine base commit before any change here gave
`QA PASSED ... ` with **24** scored runs, exit 0 — so nothing in the pre-existing suite was red for
reasons predating this branch, and none of the 22 new runs inherits an existing failure.

Each row disables ONE new safety condition; the scenario named must then go red.

| # | New safety condition | Mutation | Scenario(s) | Result |
|---|---|---|---|---|
| 1 | colour space is part of the encoder-reuse key | `currentColorSpace_ == colorSpace` → `true` | `er-mutant-key`, `wd-mutant-key` | red / red |
| 2 | the whole key, as it stood | `libomt/cpp` extracted from `origin/master` | `er-prefix` | red (payload bytes, all SETUP+calibration green) |
| 3 | producer must identify itself before anything is declared | `used->valid = false` | `wd-mutant-noident`, `er-mutant-noident` | red / red |
| 4 | tripwire: frame claim ≠ encoder configuration is refused | `if (false && encoder.colorSpace != frame.colorSpace)` | `wd-mutant-tripwire` | red |
| 5 | belt: header builder refuses a matrix outside 0/601/709 | `if (false)` | `wd-mutant-belt` | red |
| 6 | width/height come from the encoder | `if (false && (w≠‖h≠))` | `wd-mutant-size` | red |
| 7 | no encoder alive ⇒ no declaration | `if (false)` on `!encoder.valid` | `wd-mutant-noencoder` | red |
| 8 | no payload ⇒ no declaration | `if (false)` on `encodedLen <= 0` | `wd-mutant-nolength` | red |
| 9 | `HighBitDepth` refused on 8-bit inputs | gate disabled | `dg-mutant-highbitdepth` | red |
| 10 | `Alpha` refused on a codec with no alpha plane | gate disabled | `dg-mutant-alpha-plane` | red |
| 11 | `PreMultiplied` refused without `Alpha` | gate disabled | `dg-mutant-premultiplied` | red |
| 12 | undefined flag bits refused | gate disabled | `dg-mutant-unknown-bits` | red |
| 13 | `Interlaced` refused where the codec will not interlace | gate disabled | `dg-mutant-interlace-claim` | red |
| 14 | the interlace height rule is the codec's rule | `interlacedHeightSupported` → `return true` | `dg-mutant-interlace-heights` | red |
| 15 | aspect ratio must be finite and positive | gate disabled | `dg-mutant-aspect-ratio` | red |
| 16 | flag numbering matches the authority | `HighBitDepth` 16→4, compile-time pin deleted so it builds | `dg-mutant-flag-numbering` | red |
| — | declaration published from the encoder rather than the frame | `ext.colorSpace = frame.colorSpace` | *(attempted, not scored)* | **not applied — equivalent mutant**, see below |

**Counts: 15 distinct conditions mutated across 18 scored mutant runs — 18 caught, 0 escaped,
1 not applied.**

The not-applied case is recorded rather than hidden: with condition 4 (the tripwire) present,
`buildVideoHeaders()` refuses every input where the frame's claim and the encoder's configuration
differ, so on every input it *accepts* the two are equal and the two spellings of the published
value are the same program. No assertion can distinguish them, and inventing one that "fails" only
when its own premise is removed is the kind of evidence this suite exists to refuse. The falsifiable
property is the conjunction, and its controls are rows 4 (tripwire removed → red) and 1/2 (key
removed → red). `qa/build-host.sh` carries the same note where the mutant used to live, so if the
tripwire is ever deleted the mutant becomes meaningful and should be restored.

Cross-checks that the mutants are not simply breaking the build: every run above reached its
assertions (`score_run` refuses to score an absent binary — `BUILD FAILED` is reported separately and
sets the exit code), and each red run names the assertion that moved, not a setup line.

## 5. Anti-vacuity

* What would let the payload test pass with the fix absent: a fixture whose 601 and 709 encodes are
  the same bytes (grey/saturated-UV content does exactly that). Answered by `R0b`, which fails the
  file when the two references ever agree, and by `R0c`, which fails it when the references stop
  being reproducible.
* What would let it pass with nothing being tested: `Sender::send()` returns **1** when no client is
  attached, so a `send() > 0` assertion scores a frame that reached nobody as delivered. The harness
  asserts `connectionCount() == 1`, that the subscribe control was accepted, and `send() > 1`
  (`omtqa::Stream::sendDelivered`), and prints the returned length on every delivery line.
* What would let the header assertions pass vacuously: a parser that desynchronises and returns
  nothing. `omt-wire.h` records a `setupFailure` string, and the payload comparisons compare real
  bytes against a reference whose length is printed on the assertion itself.
* What would let the seam tests pass vacuously: `buildVideoHeaders()` being dead code. `MW7` (the
  identity capture never marks itself valid) turns both the stub suite and the real-codec suite red,
  which is what proves the declaration path is on the wire.
* The one mutant that could not be made to fail is documented as an equivalent mutant rather than
  replaced with an assertion that cannot distinguish anything (§4).

## 6. Host-provable vs device-only

Host-proven by this branch: the encoder-reuse key and its consequence in the bitstream (x86_64, the
real codec, AVX2 path, ASan); the declaration derivation, its tripwire and each gate's refusal,
counting and wording; the flag numbering against the public C enum; the app's current NV12 descriptor
still being accepted.

Not claimed from the host, device-only:
* Whether the shipped app ever *produces* a same-size colour-space change. `OMTBridge.cpp:139`
  derives the space from the height, so same size ⇒ same height ⇒ same space: today the transition
  is unreachable through the Android capture path and reachable through the public C entry point
  (`libomt/cpp/libomt_android.cpp:163-179` copies `OMTMediaFrame` verbatim). It becomes reachable
  from the app the moment the colour-resolver task (#8) lands, which is why the sender is being
  closed first.
* What the picture looks like on a real panel or in OBS, and whether a receiver reads the corrected
  declaration the way we expect (needs the receiver-side interop run).
* arm64 behaviour: the payload byte-equality evidence is the x86/AVX2 codec translation unit; the
  arm64 TU (`vmxcodec_arm.cpp`) is not exercised by host QA.
* Anything about the JNI/Java packing layer, camera-session behaviour, timestamps, thermals or
  sustained frame rates. Those are other lanes; `app/src/main/cpp/` and the `OMT*` Java files were
  not touched here.

## 7. Same defect in the desktop vendored tree — REPORTED, NOT PATCHED

`/home/reality/nodos-yded-linux/plugins/yd.net.ompout/vendor/libomtcpp` (read-only from this branch;
no file was copied in either direction):

* `src/OMTSender.cpp:368-382` — `Sender::createEncoder()` still carries the dimension-only reuse key
  (`:371`: `if (vmxInstance_ && currentWidth_ == width && currentHeight_ == height) return;`) while
  `:379-380` passes `cspace` into `VMX_Create()`. It is **dead code**: declared at
  `src/OMTSender.h:279`, with no call site anywhere in that tree. The live path is
  `ensureEncoderLocked()` (`src/OMTSender.cpp:513-533`), whose key already includes
  `currentColorSpace_` (`:514-515`; member at `src/OMTSender.h:255`, "P0 #8: encoder cache key
  includes colorspace"). So the desktop node is not exposed to the lie today, but the overload is a
  landmine: the first caller wired back to `createEncoder()` reintroduces it.
* `src/OMTSender.cpp:383` — the same dead overload still computes `int maxSize = width * height * 4;`
  , the signed narrowing this tree replaced with 64-bit sizing (`libomt/cpp/OMTSender.cpp:287-304`).
* `src/OMTSender.cpp:635` — the live desktop `send()` publishes `extHeader.colorSpace =
  frame.colorSpace;` verbatim, with no derivation from the encoder identity and no tripwire. It is
  correct only because `ensureEncoderLocked()` keys on the same field; the property is not enforced
  structurally. This is exactly the state our MW1 mutant describes, and it is safe there only for as
  long as the key holds.
* Flag honesty: `src/OMTSender.cpp:634` publishes `frame.flags` verbatim with no Alpha /
  HighBitDepth / PreMultiplied / undefined-bit gate, and `:633` publishes `aspectRatio` with no
  finiteness check. Interlace is protected, but in a different place and a different way: the desktop
  refuses to *encode* at an uninterlaceable height inside `encodeFrameLocked`
  (`src/OMTSender.cpp:542-551`) and the legacy overload (`:405-412`), whereas this branch refuses the
  *frame* at `validateFrame()`, before any encoder is touched and before the no-subscribers shortcut.
  The Android tree had neither form of the interlace protection before this branch.
* `src/OMTProtocol.h:75-84` already has the corrected flag numbering (`HighBitDepth = 16`,
  `PreMultiplied = 4`, with a comment recording that HighBitDepth was wrongly 4). The Android copy had
  not received that correction; it is re-derived here from `libomt.h`, not copied.
