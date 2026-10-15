#!/usr/bin/env bash
# Host-side QA for the vendored OMT C++ — no device, no JVM, no emulator.
#
# Every test here is run twice: against the current tree (must pass) and against a copy with the
# safety gate deliberately disabled (must fail). A test that is green in both cases is not
# evidence of anything — it just means nobody wrote the assertion.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
# ── Harness isolation ────────────────────────────────────────────────────────
# Scenario binaries bind fixed ports (45331-45335) and every mutant lives under one scratch tree,
# so two qa/build-host.sh runs in parallel shred each other's verdicts. A shared /tmp/omt-qa lets
# run A compile while run B rm -rf's and re-copies the same mutant directory underneath it: the
# observed wreckage was a wall of NULs inside <built-in> #define __cpp_aligned_new, an
# OMTChannel.cpp referencing CloseReason from the OTHER checkout's tree, an OMTDiscovery.cpp that
# "simply was not there", and "[FAIL] sender.start() on port 45334" from the other run's live
# scenarios. None of it was the product's fault. Two rules:
#   1. the scratch dir is unique per run (SCRATCH= still overrides, e.g. for repeat-run.sh);
#   2. the suite holds an exclusive lock — the fixed ports are a machine-wide resource that no
#      unique tmpdir can un-share. A second run queues behind the first instead of corrupting it.
SCRATCH_ROOT=${SCRATCH_ROOT:-/tmp/omt-qa}
mkdir -p "$SCRATCH_ROOT" || { echo "QA HARNESS ERROR: cannot create $SCRATCH_ROOT"; exit 2; }
if [ -z "${SCRATCH:-}" ]; then
  SCRATCH=$(mktemp -d "$SCRATCH_ROOT/run.XXXXXXXX") \
    || { echo "QA HARNESS ERROR: mktemp -d under $SCRATCH_ROOT failed"; exit 2; }
fi
mkdir -p "$SCRATCH" || { echo "QA HARNESS ERROR: cannot create scratch dir $SCRATCH"; exit 2; }
if command -v flock >/dev/null 2>&1; then
  exec 9>"$SCRATCH_ROOT/.build-host.lock" \
    || { echo "QA HARNESS ERROR: cannot open the run lock under $SCRATCH_ROOT"; exit 2; }
  flock -w "${QA_LOCK_TIMEOUT:-3600}" 9 \
    || { echo "QA HARNESS ERROR: another qa/build-host.sh holds the run lock after ${QA_LOCK_TIMEOUT:-3600}s — the scenarios bind fixed ports, so runs must not overlap"; exit 2; }
fi
echo "qa scratch: $SCRATCH"
# Prefer the clang that ships with the NDK: vmxcodec.h uses __declspec(align(64)), which only
# clang accepts (with -fdeclspec), and using the same compiler family as the APK build means the
# host QA cannot drift into a build the device would never have produced.
if [ -z "${CXX:-}" ]; then
  for c in "${ANDROID_HOME:-$HOME/android-sdk}"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ \
           "$(command -v clang++ 2>/dev/null)"; do
    [ -x "$c" ] && CXX="$c" && break
  done
fi
CXX=${CXX:-g++}
# OMTChannel.h uses std::string without including <string>: the NDK's libc++ provides it
# transitively, host libstdc++ does not. Fixed in the tree; -include keeps older copies buildable
# so an A/B compares behaviour rather than failing on a missing include.
# -fms-extensions -fdeclspec: vmxcodec.h writes __declspec(align(64)); the APK build passes the
# same flags (app/src/main/cpp/CMakeLists.txt), so the host build must too or it cannot compile.
# -fms-extensions: vmxcodec.h writes __declspec(align(64)). The APK build adds -fdeclspec too,
# but that is a clang-only flag (the NDK is clang); g++ rejects it, so pick the flags per compiler.
DECLEXT="-fms-extensions"
case "$($CXX --version 2>/dev/null | head -1)" in *clang*) DECLEXT="-fms-extensions -fdeclspec";; esac
# ASan only: the NDK clang ships no host libubsan for this target (linking it fails on
# __ubsan_vptr_type_cache). ASan is what catches the lifetime bugs these tests are about.
FLAGS="-std=c++17 -O1 -g -march=haswell -fsanitize=address -pthread -include string -include cstring $DECLEXT"
INC="-I$ROOT/libomt/cpp -I$ROOT/libvmx/src -I$ROOT/libomt"
# Symbolize the ASan reports. An unsymbolized "READ of size 16 at 0x..." tells the next reader
# nothing about which plane of which frame was walked off the end, and the whole point of the
# out-of-bounds scenarios is that the report is the evidence. The NDK ships llvm-symbolizer.
if [ -z "${ASAN_SYMBOLIZER_PATH:-}" ]; then
  for s in "${ANDROID_HOME:-$HOME/android-sdk}"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-symbolizer; do
    [ -x "$s" ] && ASAN_SYMBOLIZER_PATH="$s" && break
  done
  export ASAN_SYMBOLIZER_PATH
fi

rc=0
FAILED=()             # scenarios that came back red, with the reason
HARNESS_ERR=0         # the harness could not even express a scenario — NOT a product verdict

# A harness failure (missing pinned base, extraction that produced the wrong tree, a mutant anchor
# that no longer matches) is not a product failure and must never be dressed up as one.
harness_die() {
  echo
  echo "QA HARNESS ERROR: $*"
  echo "  (harness failure — no verdict was reached; this is not a product regression)"
  exit 2
}
mark_harness_err() { echo "  QA HARNESS ERROR: $*"; HARNESS_ERR=1; rc=1; }

require_rev() { # <rev> <what it captures> — the pinned pre-fix bases are immutable commits; if
                # this clone cannot see one, the harness is broken and must say so, loudly
  git cat-file -e "$1^{commit}" 2>/dev/null \
    || harness_die "base revision $1 ($2) is not reachable in this clone — the QA bases are pinned commits, not branch names; fetch the full history (git fetch --unshallow; git fetch origin; git fetch https://github.com/yd-zd/opencamera-omt '+refs/heads/*:refs/remotes/fork/*') and retry"
}

extract_rev_file() { # <rev> <path> <dest> — `git show ... > f` ALONE leaves an EMPTY f behind when
                     # the rev or path is bad, and the scenario then dies in the linker looking
                     # like a product defect; abort before that can happen
  require_rev "$1" "source of $2"
  git show "$1:$2" > "$3.part" 2>"$SCRATCH/git-show.err" \
    || harness_die "git show $1:$2 failed: $(tr '\n' ' ' < "$SCRATCH/git-show.err")"
  [ -s "$3.part" ] || harness_die "git show $1:$2 produced an empty file — refusing to build a scenario out of nothing"
  mv "$3.part" "$3"
}

# A pre-fix copy must be VERIFIABLY pre-fix: a pin that silently points after the fix turns a
# must-fail scenario into a must-pass lie (that is exactly how `git show master:` rotted — PR #13
# merged, master moved under the harness, and ss-prefix started passing the gate it exists to
# prove). Grep the copy both ways: the old shape present, the fix absent.
must_grep()     { grep -q "$2" "$1" || harness_die "pre-fix check: $1 lacks /$2/ — the extracted copy is not the pre-fix tree this scenario claims to run"; }
must_not_grep() { if grep -q "$2" "$1"; then harness_die "pre-fix check: $1 still contains /$2/ — the gate was never removed; scoring this copy as a caught mutant would be a lie"; fi; }

apply_mutant() { # <scenario> <file> — python patch text on stdin. An anchor that no longer
                 # matches means the gate was NEVER disabled: the copy is still the fixed code, and
                 # scoring it as "expected fail" would blame the test for the harness's drift.
  local name=$1 f=$2
  if python3 - "$f"; then return 0; fi
  mark_harness_err "$name: mutant could not be applied — the anchor is gone, so the gate was never disabled; refusing to score an unpatched copy"
  FAILED+=("$name(anchor-missing)")
  return 1
}

# build_and_run <binary-name> <test.cpp> <omt-source-dir> <expect pass|fail> [vmx-source-dir]
#   <omt-source-dir> is a copy of libomt/cpp (the tree, or a mutant's).
#   [vmx-source-dir] is a copy of libvmx/src when the scenario mutates the codec itself, or the
#   literal STUB to link qa/stub_vmx.cpp instead of the codec — used where the test needs the codec
#   to fail in a way the real one cannot be driven into from this side of the gate.
build_and_run() {
  local name=$1 test=$2 srcdir=$3 expect=$4 vmxdir=${5:-$ROOT/libvmx/src}
  local vmxsrcs
  if [ "$vmxdir" = "STUB" ]; then
    vmxsrcs="$ROOT/qa/stub_vmx.cpp"
  else
    vmxsrcs="$vmxdir/vmxcodec.cpp $vmxdir/vmxcodec_x86.cpp $vmxdir/vmxcodec_avx2.cpp"
  fi
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS $INC "$test" "$srcdir/OMTChannel.cpp" "$srcdir/OMTAsyncPool.cpp" \
       "$srcdir/OMTSender.cpp" "$srcdir/OMTDiscovery.cpp" $vmxsrcs \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -10
    FAILED+=("$name(build-failed)"); rc=1; return
  fi
  score_run "$name" "$expect"
}

fresh_copy() { # <dir> — an isolated tree a mutant may deface. Copies the WHOLE directory, not a
               # *.cpp/*.h glob: a future .hpp would otherwise be silently left behind and the
               # mutant would fail to compile for the wrong reason. Errors go to stderr because
               # callers capture stdout — an empty return plus the apply_mutant/extract guards
               # downstream is what turns a cp failure into a harness error, not a mystery.
  local d="$SCRATCH/$1"; rm -rf "$d"
  mkdir -p "$d" || { echo "QA HARNESS ERROR: cannot create mutant dir $d" >&2; return 1; }
  cp -R "$ROOT/libomt/cpp/." "$d"/ || { echo "QA HARNESS ERROR: cannot copy libomt/cpp into $d" >&2; return 1; }
  echo "$d"
}

fresh_copy_vmx() { # <dir> — an isolated copy of the CODEC, for mutants that edit vmxcodec.cpp
  local d="$SCRATCH/$1"; rm -rf "$d"
  mkdir -p "$d" || { echo "QA HARNESS ERROR: cannot create mutant dir $d" >&2; return 1; }
  cp -R "$ROOT/libvmx/src/." "$d"/ || { echo "QA HARNESS ERROR: cannot copy libvmx/src into $d" >&2; return 1; }
  echo "$d"
}

# score_run <binary-name> <expect pass|fail> — run the freshly built scenario and check it produced
# the verdict we predicted for it.
score_run() {
  timeout 90 "$SCRATCH/$1" >"$SCRATCH/$1.run.log" 2>&1
  # The status must be captured in the SAME statement that reads $?: two earlier versions of this
  # line (`local got=pass; [ $? -ne 0 ]` and `local got; if [ $? ...`) each scored every run as a
  # pass, because the intervening `local` is itself a command and resets $? to 0. A harness that
  # cannot go red is not evidence.
  local status=$?
  local got=fail
  [ "$status" -eq 0 ] && got=pass
  if [ "$got" = "$2" ]; then
    echo "  $1: $got as expected"
  else
    echo "  $1: $got but expected $2  <-- PROBLEM"
    sed 's/^/      /' "$SCRATCH/$1.run.log" | tail -18
    FAILED+=("$1(verdict-$got)"); rc=1
  fi
}

# build_and_run_vmx <name> <test.cpp> <omt-src-dir> <vmx-src-dir> <expect pass|fail>
# Same as build_and_run, but the codec comes from a copy too — for scenarios whose whole point is a
# mutated vmxcodec.cpp. (Headers still resolve to the tree; the mutants below only ever change .cpp.)
build_and_run_vmx() {
  local name=$1 test=$2 srcdir=$3 vmxdir=$4 expect=$5
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS $INC "$test" "$srcdir/OMTChannel.cpp" "$srcdir/OMTAsyncPool.cpp" \
       "$srcdir/OMTSender.cpp" "$srcdir/OMTDiscovery.cpp" \
       "$vmxdir/vmxcodec.cpp" "$vmxdir/vmxcodec_x86.cpp" "$vmxdir/vmxcodec_avx2.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -10
    FAILED+=("$name(build-failed)"); rc=1; return
  fi
  score_run "$name" "$expect"
}

echo "== slow-subscriber: a stalled Wi-Fi viewer must not take the stream down =="
build_and_run ss-fixed "$ROOT/qa/slow-subscriber.cpp" "$ROOT/libomt/cpp" pass
# The pre-fix state of the slow-subscriber gate, pinned to the IMMUTABLE commit immediately before
# the fix: df66b01f = 45b8b48e^, the commit before "a stalled subscriber can no longer hold the
# stream hostage" (PR #13). This used to be `git show master:` — accurate only until PR #13 MERGED:
# master moved under the harness, the extraction silently became a post-fix copy, and the
# must-fail scenario started passing the very gate it exists to prove. master is not a pin.
#
# Only OMTChannel.cpp comes from the base; every header and every other .cpp stays from the tree.
# (1) The test asserts against post-fix names — ch->staleEvicted(), Constants::NETWORK_SEND_TIMEOUT_MS
# — that no pre-fix header declares, and a pre-fix OMTSender.cpp cannot compile against today's
# OMTSender.h at all (addChannel/createEncoder changed signature since). (2) All TUs must agree on
# Channel's layout: the socket-write section (added by fix/omt-socket-write-serialization) documents
# what header mixing did to the counters (-4702111234474983746 — "not evidence of anything"). The
# eviction and write paths under test live
# entirely in OMTChannel.cpp; OMTAsyncPool.cpp/.h have not moved since df66b01f.
BASE_SS=${QA_BASE:-df66b01f}
OLD="$SCRATCH/pre-fix"; rm -rf "$OLD"; mkdir -p "$OLD"
cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$OLD"/
extract_rev_file "$BASE_SS" "libomt/cpp/OMTChannel.cpp" "$OLD/OMTChannel.cpp"
must_grep     "$OLD/OMTChannel.cpp" 'sendAllBlocking'
must_not_grep "$OLD/OMTChannel.cpp" 'sendWithDeadline'
build_and_run ss-prefix "$ROOT/qa/slow-subscriber.cpp" "$OLD" fail

echo
echo "== colorspace-gate: refuse what the encoder cannot honour =="
build_and_run cs-fixed "$ROOT/qa/colorspace-gate.cpp" "$ROOT/libomt/cpp" pass

M1=$(fresh_copy mutant-gate)
if apply_mutant cs-mutant-gate "$M1/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (frame.colorSpace != ColorSpace::Undefined &&"
assert s.count(old) == 1, "gate not found to disable"
open(p, "w").write(s.replace(old, "    if (false && frame.colorSpace != ColorSpace::Undefined &&", 1))
PY
then build_and_run cs-mutant-gate "$ROOT/qa/colorspace-gate.cpp" "$M1" fail
fi

M2=$(fresh_copy mutant-count)
if apply_mutant cs-mutant-count "$M2/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        encodeFailures_++;"
assert s.count(old) == 1, "counter not found to disable"
open(p, "w").write(s.replace(old, "        ; // MUTANT: the failure goes uncounted", 1))
PY
then build_and_run cs-mutant-count "$ROOT/qa/colorspace-gate.cpp" "$M2" fail
fi

echo
echo "== frame-validation: refuse a frame the buffer cannot honour, before the codec reads it =="
build_and_run fv-fixed "$ROOT/qa/frame-validation.cpp" "$ROOT/libomt/cpp" pass

# M5: the gate removed wholesale. B1 must die with an ASan heap-buffer-overflow READ in
# VMX_NV12ToPlanar — that report, not the assertion, is the proof the crash was real.
M5=$(fresh_copy mutant-validate-call)
if apply_mutant fv-mutant-validate-call "$M5/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (!validateFrame(frame, why)) {"
assert s.count(old) == 1, "validateFrame call not found to disable"
open(p, "w").write(s.replace(old, "    if (false && !validateFrame(frame, why)) {", 1))
PY
then build_and_run fv-mutant-validate-call "$ROOT/qa/frame-validation.cpp" "$M5" fail
fi

# M6: the gate present, but charging NV12 only for the luma plane — the exact arithmetic error that
# let the chroma rows walk off the ImageReader allocation. Same ASan death, narrower cause.
M6=$(fresh_copy mutant-nv12-need)
if apply_mutant fv-mutant-nv12-need "$M6/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        need = h * s + (h / 2ull) * s;"
assert s.count(old) == 1, "NV12 need clause not found"
open(p, "w").write(s.replace(old, "        need = h * s;  // MUTANT: the UV plane is not charged for", 1))
PY
then build_and_run fv-mutant-nv12-need "$ROOT/qa/frame-validation.cpp" "$M6" fail
fi

# M7: P0 #16 removed — a zero/negative frame-rate numerator would be stamped into the wire header.
M7=$(fresh_copy mutant-frameraten)
if apply_mutant fv-mutant-frameraten "$M7/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (frame.frameRateN <= 0)"
assert s.count(old) == 1, "frameRateN clause not found"
open(p, "w").write(s.replace(old, "    if (false && frame.frameRateN <= 0)", 1))
PY
then build_and_run fv-mutant-frameraten "$ROOT/qa/frame-validation.cpp" "$M7" fail
fi

# M8: the encode-buffer sizing put back the way it was: `int maxSize = width * height * 4`, which
# wraps, plus the clamp that quietly turns the wrapped negative into the 1 MB floor.
M8=$(fresh_copy mutant-narrow-cap)
if apply_mutant fv-mutant-narrow-cap "$M8/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    if (pixels >= maxPixels) return kMaxEncodeBufferBytes;
    uint64_t bytes = pixels * 4ull;
    if (bytes < kMinEncodeBufferBytes) bytes = kMinEncodeBufferBytes;
    return static_cast<size_t>(bytes);"""
assert s.count(old) == 1, "encodeBufferCapacity body not found"
new = """    int maxSize = width * height * 4;              // MUTANT: the old signed narrowing
    if (maxSize < 1024 * 1024) maxSize = 1024 * 1024;
    return static_cast<size_t>(maxSize);"""
open(p, "w").write(s.replace(old, new, 1))
PY
then build_and_run fv-mutant-narrow-cap "$ROOT/qa/frame-validation.cpp" "$M8" fail
fi

echo
echo "== encoder-create-failure: what the sender says when the codec says no =="
# Links qa/stub_vmx.cpp rather than the codec: VMX_Create cannot be driven to NULL through the real
# one any more now that validateFrame() enforces the same dimension limits the codec checks.
build_and_run ec-fixed "$ROOT/qa/encoder-create-failure.cpp" "$ROOT/libomt/cpp" pass STUB

M9=$(fresh_copy mutant-create-null)
if apply_mutant ec-mutant-create-null "$M9/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (!vmxInstance_) {\n        why = \"VMX_Create refused \""
assert s.count(old) == 1, "createEncoder NULL check not found"
open(p, "w").write(s.replace("    if (!vmxInstance_) {", "    if (false) {  // MUTANT: NULL is cached like a success", 1))
PY
then build_and_run ec-mutant-create-null "$ROOT/qa/encoder-create-failure.cpp" "$M9" fail STUB
fi

echo
echo "== drop-policy: when the ring is full, the newest frame must be the one that survives =="
build_and_run dp-fixed "$ROOT/qa/drop-policy.cpp" "$ROOT/libomt/cpp" pass

M3=$(fresh_copy mutant-keep-oldest)
if apply_mutant dp-mutant-keep-oldest "$M3/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (!pendingQueue_.empty()) { stale = pendingQueue_.front(); pendingQueue_.pop(); }"
assert s.count(old) == 1, "evict-oldest not found"
# the original policy: keep what is queued, drop the frame in hand
open(p, "w").write(s.replace(old, "            if (false) { stale = pendingQueue_.front(); pendingQueue_.pop(); }", 1))
PY
then build_and_run dp-mutant-keep-oldest "$ROOT/qa/drop-policy.cpp" "$M3" fail
fi

M4=$(fresh_copy mutant-blind-drops)
if apply_mutant dp-mutant-blind "$M4/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    const int64_t delta = total - drops_baseline_.exchange(total);"
assert s.count(old) == 1, "drop window not found"
# the state before this commit: the C entry point returned a hardcoded 0
open(p, "w").write(s.replace(old, "    const int64_t delta = 0; (void)total;", 1))
PY
then build_and_run dp-mutant-blind "$ROOT/qa/drop-policy.cpp" "$M4" fail
fi

echo
echo "== vmx-saveto-contract: failure is 0, never a positive error code that reads as a frame =="
build_and_run vs-fixed "$ROOT/qa/vmx-saveto-contract.cpp" "$ROOT/libomt/cpp" pass

# MV1: the failure contract reverted. VMX_SaveTo answers a null instance / null dst / zero length
# with VMX_ERR_INVALID_INSTANCE (5) and VMX_ERR_INVALID_PARAMETERS (6) again, and Sender::send() —
# which tests `encodedLen <= 0` — puts a 5-byte payload behind a real FrameHeader on the wire.
MV1=$(fresh_copy_vmx mutant-saveto-errcodes)
if apply_mutant vs-mutant-saveto-errcodes "$MV1/vmxcodec.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "\tif (!instance) return 0;\n\tif (!dst) return 0;\n\tif (maxLen <= 0) return 0;\n"
assert s.count(old) == 1, "VMX_SaveTo failure contract not found"
new = ("\tif (!instance) return VMX_ERR_INVALID_INSTANCE;\n"
       "\tif (!dst) return VMX_ERR_INVALID_PARAMETERS;\n"
       "\tif (!maxLen) return VMX_ERR_INVALID_PARAMETERS;\n")
open(p, "w").write(s.replace(old, new, 1))
PY
then build_and_run_vmx vs-mutant-saveto-errcodes "$ROOT/qa/vmx-saveto-contract.cpp" "$ROOT/libomt/cpp" "$MV1" fail
fi

# MV2: CHECKBUFF_SAVE back to the out-of-range pointer comparison. The negative-length probes in
# part C then pass the bounds check and hand memcpy (size_t)(-8) and (size_t)(-4): ASan, not an
# assertion. Both loops are probed because the macro is written out twice in VMX_SaveTo.
MV2=$(fresh_copy_vmx mutant-checkbuff)
if apply_mutant vs-mutant-checkbuff "$MV2/vmxcodec.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "\tif (_need < 4 || _need > _remaining) return 0; \\\n"
assert s.count(old) == 1, "CHECKBUFF_SAVE remaining-bytes check not found"
open(p, "w").write(s.replace(old, "\tif ((b + checkNumBytes) > maxB) return 0; \\\n", 1))
PY
then build_and_run_vmx vs-mutant-checkbuff "$ROOT/qa/vmx-saveto-contract.cpp" "$ROOT/libomt/cpp" "$MV2" fail
fi

# MV4: the sender's own length gate disabled at the predicate. The codec bounds itself, so this is
# the belt being tested on its own — which is only possible because the predicate is pure.
MV4=$(fresh_copy mutant-saveto-predicate)
if apply_mutant vs-mutant-predicate "$MV4/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    return encodedLen > 0 && static_cast<size_t>(encodedLen) <= capacity;"
assert s.count(old) == 1, "encodedLenAcceptable predicate not found"
open(p, "w").write(s.replace(old, "    return true;  // MUTANT: any length the codec returns is publishable", 1))
PY
then build_and_run vs-mutant-predicate "$ROOT/qa/vmx-saveto-contract.cpp" "$MV4" fail
fi

echo
echo "== aligned-stride: the codec may only touch the buffer it actually owns =="
# The codec-only harness: the scenario links the codec straight from a copy of libvmx/src, and the
# -I order puts that copy's HEADERS ahead of the tree's, so a mutant that edits
# vmxcodec_common.h — where the aligned-stride helpers live — is really what gets compiled.
build_and_run_codec() { # <name> <test.cpp> <vmx-src-dir> <expect pass|fail>
  local name=$1 test=$2 vmxdir=$3 expect=$4
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS -I"$vmxdir" $INC "$test" \
       "$vmxdir/vmxcodec.cpp" "$vmxdir/vmxcodec_x86.cpp" "$vmxdir/vmxcodec_avx2.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -10
    FAILED+=("$name(build-failed)"); rc=1; return
  fi
  score_run "$name" "$expect"
}

build_and_run_codec as-fixed "$ROOT/qa/aligned-stride.cpp" "$ROOT/libvmx/src" pass

# The fix removed wholesale: the three files it touches, taken from the commit this branch sits on.
# The predicate revert cannot be done in-place in the header alone — VMX_FreeAlignedStrideBuffer
# changed signature, so its callers have to revert with it or the mutant just fails to build.
# Pinned to d2ab8ca8 — the commit the aligned-stride fix (cc4a57a9, PR #20) sat on, i.e. the old
# tip of fix/omt-input-validation-off-the-desktop. A branch NAME was deletable and absent from
# fresh clones; this sha is an ancestor of master, so it is always reachable. QA_BASE overrides for
# A/B experiments — it must name a commit, not a moving ref.
BASE_A=${QA_BASE:-d2ab8ca8}
PFV="$SCRATCH/vmx-prefix-aligned"; rm -rf "$PFV"; mkdir -p "$PFV"
cp "$ROOT"/libvmx/src/*.cpp "$ROOT"/libvmx/src/*.h "$PFV"/
for f in vmxcodec_common.h vmxcodec_x86.cpp vmxcodec_arm.cpp; do
  extract_rev_file "$BASE_A" "libvmx/src/$f" "$PFV/$f"
done
must_grep "$PFV/vmxcodec_common.h" 'alignedStride != srcStride'
build_and_run_codec as-prefix "$ROOT/qa/aligned-stride.cpp" "$PFV" fail

# MA1: ownership predicate back to stride inequality in the copy only — the encoder reads the
# uninitialised malloc for a stride-padded HAL frame, and the bitstream depends on heap history.
MA1=$(fresh_copy_vmx mutant-copy-predicate)
if apply_mutant as-mutant-copy-predicate "$MA1/vmxcodec_common.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "\tif (aligned != srcBuffer)"
assert s.count(old) == 1, "CopyTo ownership predicate not found"
open(p, "w").write(s.replace(old, "\tif (alignedStride != srcStride)  // MUTANT: the old predicate", 1))
PY
then build_and_run_codec as-mutant-copy-predicate "$ROOT/qa/aligned-stride.cpp" "$MA1" fail
fi

# MA2: ownership fixed but the row tail left uninitialised — the vector loads read indeterminate
# bytes and the last block of every row is non-deterministic.
MA2=$(fresh_copy_vmx mutant-no-tail-memset)
if apply_mutant as-mutant-no-tail-memset "$MA2/vmxcodec_common.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "\t\t\tif (alignedStride > rowBytes)\n\t\t\t\tmemset(pDst + rowBytes, 0, (size_t)(alignedStride - rowBytes));\n"
assert s.count(old) == 1, "tail memset not found"
open(p, "w").write(s.replace(old, "\t\t\t// MUTANT: the tail stays exactly as malloc left it\n", 1))
PY
then build_and_run_codec as-mutant-no-tail-memset "$ROOT/qa/aligned-stride.cpp" "$MA2" fail
fi

# MA3: free() unconditionally — the other direction. A width that is already aligned borrows the
# caller's buffer; freeing it is an ASan death ("not malloc()-ed"), which is why the ownership
# predicate has to be identity and not "always".
MA3=$(fresh_copy_vmx mutant-free-always)
if apply_mutant as-mutant-free-always "$MA3/vmxcodec_common.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "\tif (aligned != original)\n\t{\n\t\tfree(aligned);\n\t}"
assert s.count(old) == 1, "Free ownership predicate not found"
open(p, "w").write(s.replace(old, "\tif (true)  // MUTANT: ownership ignored\n\t{\n\t\tfree(aligned);\n\t}", 1))
PY
then build_and_run_codec as-mutant-free-always "$ROOT/qa/aligned-stride.cpp" "$MA3" fail
fi
# ==================================================================================================
# stop-deadlock: Sender::stop() must not destroy channels while it holds channelsMutex_.
# ~Channel() joins the receiver thread; that thread is inside listener_->onQualityChanged() whenever
# a subscriber has just sent <OMTSettings Quality="..."/>, and Sender::onQualityChanged() calls
# getBestSuggestedQuality(), which takes the SAME mutex. The old `channels_.clear()` under the lock
# therefore deadlocked against its own callback: stop() never returned, and MainActivity::onDestroy
# -> stopAll() (PR #15) puts that on the UI thread. removeChannel()/cleanupDisconnected() already
# unlink under the lock and destroy outside it — stop() was the path that did not.
echo "== stop-deadlock: teardown must not hold channelsMutex_ across ~Channel()'s join =="
build_and_run sd-fixed "$ROOT/qa/stop-deadlock.cpp" "$ROOT/libomt/cpp" pass

# The pre-fix run: OMTSender.cpp back at the revision this branch sits on, everything else from the
# tree (and the headers from the tree, so all TUs agree on Sender's layout — rule 15).
PFS="$SCRATCH/pre-fix-stop"; rm -rf "$PFS"; mkdir -p "$PFS"
cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$PFS"/
# Pinned to d9814679 — the commit this branch sits on (the product commit's parent, master's tip
# at audit time). The old default named fix/omt-announcement-truthfulness: a deletable branch
# name, absent from fresh clones — and two PRs too old besides: its OMTSender.cpp could not even
# compile against today's OMTSender.h (createEncoder changed signature in #18), which is why
# this scenario arrived BUILD FAILED rather than red. The base's file is now exactly "this
# branch's product commit, removed"; the headers stay the tree's so all TUs agree on Sender's
# layout (rule 15), and the branch's only header change is the private→protected visibility of
# the listener overrides the test subclass needs — definitions match either way.
BASE_SD=${QA_BASE:-d9814679}
extract_rev_file "$BASE_SD" "libomt/cpp/OMTSender.cpp" "$PFS/OMTSender.cpp"
must_grep     "$PFS/OMTSender.cpp" 'channels_.clear();'
must_not_grep "$PFS/OMTSender.cpp" 'doomed.swap(channels_)'

build_and_run sd-prefix "$ROOT/qa/stop-deadlock.cpp" "$PFS" fail

# SD1: the swap is kept and the lock is still released — but the destruction happens INSIDE the
# scope. Proves the fix is the release, not the temporary vector: a reviewer who "improves" this by
# moving doomed.clear() back into the block recreates the hang, and this mutant catches it.
SD1=$(fresh_copy mutant-destroy-in-lock)
if apply_mutant sd-mutant-destroy-in-lock "$SD1/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("        doomed.swap(channels_);\n    }\n    doomed.clear();")
assert s.count(old) == 1, "deferred destruction not found"
new = ("        doomed.swap(channels_);\n        doomed.clear();  // MUTANT: joined the receiver "
       "while still holding channelsMutex_\n    }")
open(p, "w").write(s.replace(old, new, 1))
PY
then build_and_run sd-mutant-destroy-in-lock "$ROOT/qa/stop-deadlock.cpp" "$SD1" fail
fi

# SD2: copy the vector instead of swapping it. The channels stay referenced, so ~Channel() never
# runs and stop() returns having torn nothing down — the other way a teardown fix goes wrong.
SD2=$(fresh_copy mutant-copy-not-swap)
if apply_mutant sd-mutant-copy-not-swap "$SD2/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        doomed.swap(channels_);"
assert s.count(old) == 1, "swap not found"
open(p, "w").write(s.replace(old, "        doomed = channels_;  // MUTANT: unlink never happens", 1))
PY
then build_and_run sd-mutant-copy-not-swap "$ROOT/qa/stop-deadlock.cpp" "$SD2" fail
fi

# Codec-only builder for this scenario: the test drives ThreadTask straight out of the header, and
# the -I order puts the copy's headers ahead of the tree's so a mutant that edits thread_tasks.h is
# really what gets compiled. Named for this section: the sibling branches add their own, so each
# section stays independently mergeable.
build_and_run_threadtask() { # <name> <test.cpp> <vmx-src-dir> <expect pass|fail>
  local name=$1 test=$2 vmxdir=$3 expect=$4
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS -I"$vmxdir" $INC "$test" \
       "$vmxdir/vmxcodec.cpp" "$vmxdir/vmxcodec_x86.cpp" "$vmxdir/vmxcodec_avx2.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -10
    FAILED+=("$name(build-failed)"); rc=1; return
  fi
  score_run "$name" "$expect"
}

echo "== thread-task-join: Join() waits for the work, Destroy() waits for the thread =="
build_and_run_threadtask tt-fixed "$ROOT/qa/thread-task-join.cpp" "$ROOT/libvmx/src" pass

# The fix removed wholesale: thread_tasks.h from the commit this branch sits on. That copy already
# carries the `delete[] tasks->tasks` line from df66b01f, so what goes red here is the Join half —
# the bare condvar wait and the `running` read outside the mutex — and nothing else.
PFT="$SCRATCH/vmx-prefix-join"; rm -rf "$PFT"; mkdir -p "$PFT"
# Pinned to d2ab8ca8 — the old tip of fix/omt-input-validation-off-the-desktop, an ancestor of
# master so always reachable (a branch NAME is deletable and absent from fresh clones).
# thread_tasks.h has not moved since; QA_BASE overrides must name a commit.
BASE_TT=${QA_BASE:-d2ab8ca8}
cp "$ROOT"/libvmx/src/*.cpp "$ROOT"/libvmx/src/*.h "$PFT"/
extract_rev_file "$BASE_TT" "libvmx/src/thread_tasks.h" "$PFT/thread_tasks.h"
must_grep "$PFT/thread_tasks.h" $'\t\t\tcomplete.wait(lock);' 
build_and_run_threadtask tt-prefix "$ROOT/qa/thread-task-join.cpp" "$PFT" fail

# MJ1: Join back to the bare wait, TaskLoop left fixed. Red on J1 alone — the predicate, not the
# thread bookkeeping, is what stops Join reporting "done" with tasks still queued.
MJ1=$(fresh_copy_vmx mutant-bare-wait)
if apply_mutant tt-mutant-bare-wait "$MJ1/thread_tasks.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "\t\tcomplete.wait(lock, [this] { return queue.empty(); });"
assert s.count(old) == 1, "Join predicate not found"
new = ("\t\tif (queue.size() > 0)\n"
       "\t\t\tcomplete.wait(lock);  // MUTANT: a spurious wake now means 'finished'")
open(p, "w").write(s.replace(old, new, 1))
PY
then build_and_run_threadtask tt-mutant-bare-wait "$ROOT/qa/thread-task-join.cpp" "$MJ1" fail
fi

# MJ2: TaskLoop back to reading `running` outside the mutex, Join left fixed. Red on J3 alone:
# Destroy can clear running and notify in the window before the worker reaches cv.wait, the worker
# sleeps through the only wakeup it will ever get, and thread.join() never returns.
MJ2=$(fresh_copy_vmx mutant-running-outside-lock)
if apply_mutant tt-mutant-running-outside-lock "$MJ2/thread_tasks.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
i = s.index("\tvoid TaskLoop()")
j = s.index("\tvoid Initialize()")
old = s[i:j]
assert "cv.wait(lock, [this]" in old, "TaskLoop predicate not found"
new = ("\tvoid TaskLoop()\n\t{\n\t\twhile (running)  // MUTANT: read outside mtx — lost wakeup\n\t\t{\n"
       "\t\t\tfor (volatile int spin = 0; spin < 300000; ++spin) {}  // MUTANT: widen the unlocked\n"
       "\t\t\t// window so the lost wakeup is structural, not a coin flip (see qa/repeat-run.sh)\n"
       "\t\t\tstd::unique_lock<std::mutex> lock(mtx);\n"
       "\t\t\tif (queue.size() == 0)\n\t\t\t{\n\t\t\t\tcomplete.notify_all();\n\t\t\t\tcv.wait(lock);\n\t\t\t}\n"
       "\t\t\tif (!running) break;\n\t\t\tstd::function<void()> func = NULL;\n"
       "\t\t\tif (queue.size() > 0)\n\t\t\t{\n\t\t\t\tfunc = queue.front();\n\t\t\t\tqueue.pop();\n\t\t\t}\n"
       "\t\t\tif (func)\n\t\t\t{\n\t\t\t\tfunc();\n\t\t\t}\n\t\t}\n\t}\n\n")
open(p, "w").write(s[:i] + new + s[j:])
PY
then build_and_run_threadtask tt-mutant-running-outside-lock "$ROOT/qa/thread-task-join.cpp" "$MJ2" fail
fi

echo

echo
# ---------------------------------------------------------------------------
# planar-chroma: the capture path must honour BOTH chroma layouts of YUV_420_888.
#
# qa/planar-chroma.cpp compiles app/src/main/cpp/omtnv12.h — the SAME header the APK builds — against
# synthetic plane geometries, so the layout decision that decides whether a phone can stream at all
# is testable without a phone. Two things make the section worth running:
#   * pc-prefix builds the identical scenarios against a verbatim transcription of the pre-fix
#     capture path (the Java plane copy + OMTBridge.cpp:121's dataLength arithmetic). Case B is the
#     emulator's own geometry — 1920x1080, y rowStride 1920, chroma rowStride 960, chroma
#     pixelStride 1 — and it MUST be red there. That is the bug, measured, not described.
#   * the mutants sabotage one property of the packer each. In particular mp3 (the chroma row walked
#     with the luma stride) dies to ASan, because the fake planes are allocated to exactly
#     Image.Plane.getBuffer().limit() and nothing more.
# Android host QA is ASan-only here (no libubsan for this target), as everywhere in this file.
# ---------------------------------------------------------------------------
# build_planar <name> <expect pass|fail> [extra compiler flags...]
# Extra -I dirs go FIRST: a mutant's copy of omtnv12.h has to win the include race against the
# tree's, and the first -I on the command line does.
build_planar() {
  local name=$1 expect=$2; shift 2
  local extra="$*"
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS $extra $INC -I"$ROOT/app/src/main/cpp" "$ROOT/qa/planar-chroma.cpp" \
       "$ROOT/libomt/cpp/OMTChannel.cpp" "$ROOT/libomt/cpp/OMTAsyncPool.cpp" \
       "$ROOT/libomt/cpp/OMTSender.cpp" "$ROOT/libomt/cpp/OMTDiscovery.cpp" \
       "$ROOT/libvmx/src/vmxcodec.cpp" "$ROOT/libvmx/src/vmxcodec_x86.cpp" \
       "$ROOT/libvmx/src/vmxcodec_avx2.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -15
    rc=1; return
  fi
  score_run "$name" "$expect"
}

fresh_copy_bridge() { # <dir> — an isolated copy of the JNI bridge's headers, for packer mutants
  local d="$SCRATCH/$1"; rm -rf "$d"; mkdir -p "$d"
  cp "$ROOT"/app/src/main/cpp/*.h "$d"/
  echo "$d"
}

mutate_bridge() { # <dir> <old> <new> <what> — one property sabotaged, or the suite is not honest
  local d=$1
  python3 - "$d/omtnv12.h" "$2" "$3" "$4" <<'PY'
import sys
p, old, new, what = sys.argv[1:5]
s = open(p).read()
assert s.count(old) == 1, "cannot find the clause to mutate (%s): %r" % (what, old[:70])
open(p, "w").write(s.replace(old, new, 1))
PY
  if [ $? -ne 0 ]; then echo "  $4: MUTATION FAILED (the clause is gone — the suite would pass by forgetting)"; rc=1; fi
}

echo "== planar-chroma: YUV_420_888 is two layouts, and the sender must serve both =="
build_planar pc-fixed pass

# The pre-fix build: same scenarios, same assertions, the capture path as it stood before this
# branch. Red on case B (the emulator's planar I420) is the point of the whole exercise.
build_planar pc-prefix fail -DOMT_QA_PLANAR_PREFIX

# MP1: pixelStride ignored — the packer decides "interleaved" without asking the HAL. The planar
# frames then fail the NV12 row-width check and never reach the encoder at all.
MP1=$(fresh_copy_bridge mutant-pixelstride)
mutate_bridge "$MP1" \
  '    const bool planar = (u.pixelStride == 1 && v.pixelStride == 1);
    const bool interleaved = (u.pixelStride == 2 && v.pixelStride == 2);' \
  '    const bool planar = false;   // MUTANT: the HAL was asked nothing
    const bool interleaved = true;' \
  "ignore pixelStride"
build_planar pc-mutant-pixelstride fail -I"$MP1"

# MP2: the interleave forgotten — a planar Cb plane wearing Cr's clothes. Content, not size: only a
# byte-by-byte assertion sees it, which is why the harness compares against the source planes.
MP2=$(fresh_copy_bridge mutant-no-interleave)
mutate_bridge "$MP2" \
  '                row[2 * c]     = srcU[c];
                row[2 * c + 1] = srcV[c];' \
  '                row[2 * c]     = srcU[c];
                row[2 * c + 1] = srcU[c];  // MUTANT: the Cr plane is never read' \
  "forget to interleave"
build_planar pc-mutant-no-interleave fail -I"$MP2"

# MP3: THE ORIGINAL DEFECT, one line wide — the chroma row walked with the luma stride. The size
# pre-checks still pass (they use the plane's own rowStride), so this dies to ASan reading past the
# end of a Cb plane allocated to exactly its reported limit. That report is the proof.
MP3=$(fresh_copy_bridge mutant-chroma-luma-stride)
mutate_bridge "$MP3" \
  '            const uint8_t* srcU = u.data + r * static_cast<int64_t>(u.rowStride);
            const uint8_t* srcV = v.data + r * static_cast<int64_t>(v.rowStride);' \
  '            const uint8_t* srcU = u.data + r * stride;   // MUTANT: the luma stride, as pre-fix
            const uint8_t* srcV = v.data + r * stride;' \
  "chroma walked with the luma stride"
build_planar pc-mutant-chroma-luma-stride fail -I"$MP3"

# MP4: the frame charged with the SOURCE uv rowStride instead of the stride the encoder will use —
# OMTBridge.cpp:121's arithmetic, smuggled back into the packer. The gate refuses it, which is the
# whole reason the defect was diagnosable at all.
MP4=$(fresh_copy_bridge mutant-old-arithmetic)
mutate_bridge "$MP4" \
  '    const int64_t dataLength = h * stride + chromaRows * stride;' \
  '    const int64_t dataLength = h * stride + chromaRows * (planar ? (int64_t)u.rowStride : stride);  // MUTANT: pre-fix arithmetic' \
  "charge with the source uvStride"
build_planar pc-mutant-old-arithmetic fail -I"$MP4"

# MP5: the UV plane parked at width*height instead of stride*height. Invisible on an unpadded frame,
# fatal on a padded one — encodeFrame() computes uv = data + stride*height and would encode the tail
# of the luma plane as chroma.
MP5=$(fresh_copy_bridge mutant-uv-offset)
mutate_bridge "$MP5" \
  '    uint8_t* uv = dst + h * stride;  // where encodeFrame() will point VMX — do not move it' \
  '    uint8_t* uv = dst + h * w;       // MUTANT: the tight NV12 offset, wrong for a padded stride' \
  "UV plane at the wrong offset"
build_planar pc-mutant-uv-offset fail -I"$MP5"

# Channel scenarios, with the HEADERS taken from the same copy as the sources. build_and_run resolves
# includes against the tree, which is fine while a mutant changes only .cpp; here the pre-fix copy
# changes Channel's member layout (sendSocketMutex_ is a member), and a test built against the tree's
# header while linking the copy's .cpp reads members at the wrong offsets — the counters came back as
# -4702111234474983746 the first time, which is not evidence of anything.
build_and_run_channel() { # <name> <test.cpp> <omt-src-dir> <expect pass|fail>
  local name=$1 test=$2 srcdir=$3 expect=$4
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS -I"$srcdir" $INC "$test" "$srcdir/OMTChannel.cpp" "$srcdir/OMTAsyncPool.cpp" \
       "$srcdir/OMTSender.cpp" "$srcdir/OMTDiscovery.cpp" \
       "$ROOT/libvmx/src/vmxcodec.cpp" "$ROOT/libvmx/src/vmxcodec_x86.cpp" "$ROOT/libvmx/src/vmxcodec_avx2.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -10
    FAILED+=("$name(build-failed)"); rc=1; return
  fi
  score_run "$name" "$expect"
}

echo "== socket-write-order: one writer per channel, one deadline per write =="
build_and_run_channel sw-fixed "$ROOT/qa/socket-write-order.cpp" "$ROOT/libomt/cpp" pass

# The fix removed wholesale: OMTChannel.cpp/.h from the commit this branch sits on — i.e. the tree
# that already has sendWithDeadline (45b8b48e) and nothing else. OMTConstants.h is deliberately
# left alone: the scenario asserts against the new budgets by name, and reverting the header that
# merely declares them would fail the BUILD rather than the gate.
# Pinned to d2ab8ca8 — the commit this branch sat on (old tip of
# fix/omt-input-validation-off-the-desktop, an ancestor of master so always reachable): the
# tree that already has sendWithDeadline (45b8b48e) and nothing else. A branch NAME is
# deletable and absent from fresh clones; QA_BASE overrides must name a commit, not a moving ref.
BASE_SW=${QA_BASE:-d2ab8ca8}
PFS="$SCRATCH/pre-fix-write"; rm -rf "$PFS"; mkdir -p "$PFS"
cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$PFS"/
for f in OMTChannel.cpp OMTChannel.h; do
  extract_rev_file "$BASE_SW" "libomt/cpp/$f" "$PFS/$f"
done
must_not_grep "$PFS/OMTChannel.cpp" 'sendSocketMutex_'
must_grep     "$PFS/OMTChannel.cpp" 'sendWithDeadline'
build_and_run_channel sw-prefix "$ROOT/qa/socket-write-order.cpp" "$PFS" fail

# MS1: the video writer stops taking the write lock. The metadata writer still tries, but a lock
# nobody else holds is not serialisation — this is the mis-framing defect in one line.
MS1=$(fresh_copy mutant-video-unlocked)
if apply_mutant sw-mutant-video-unlocked "$MS1/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    std::lock_guard<std::mutex> writeLock(sendSocketMutex_);\n    const int fd = socketFd_;"
assert s.count(old) == 1, "sendAllBlocking write lock not found"
open(p, "w").write(s.replace(old, "    // MUTANT: the async video writer writes without the lock\n    const int fd = socketFd_;", 1))
PY
then build_and_run_channel sw-mutant-video-unlocked "$ROOT/qa/socket-write-order.cpp" "$MS1" fail
fi

# MS2: the metadata writer stops taking the lock (and stops bounding its acquisition). Same defect,
# other direction: a handshake or a tally broadcast during streaming is enough to tear the stream.
MS2=$(fresh_copy mutant-metadata-unlocked)
if apply_mutant sw-mutant-metadata-unlocked "$MS2/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    while (!sendSocketMutex_.try_lock()) {"
assert s.count(old) == 1, "sendMetadataSync bounded acquisition not found"
i = s.index(old); j = s.index("    std::lock_guard<std::mutex> writeLock(sendSocketMutex_, std::adopt_lock);", i)
assert j > i
open(p, "w").write(s[:i] + "    // MUTANT: metadata is written with no lock and no budget\n" + s[j:]
                   .replace("std::lock_guard<std::mutex> writeLock(sendSocketMutex_, std::adopt_lock);",
                            "// MUTANT: nothing was acquired to adopt", 1))
PY
then build_and_run_channel sw-mutant-metadata-unlocked "$ROOT/qa/socket-write-order.cpp" "$MS2" fail
fi

# MS3: the deadline is back to being per-poll rather than per-write. The trickle peer keeps poll()
# succeeding forever, so S2 — the eviction — is what dies.
MS3=$(fresh_copy mutant-per-poll-deadline)
if apply_mutant sw-mutant-per-poll-deadline "$MS3/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        if (now >= deadline) {"
assert s.count(old) == 1, "whole-write deadline not found"
open(p, "w").write(s.replace(old, "        if (false && now >= deadline) {  // MUTANT: only poll() is bounded", 1))
PY
then build_and_run_channel sw-mutant-per-poll-deadline "$ROOT/qa/socket-write-order.cpp" "$MS3" fail
fi

# MS4: the write loop stops looking at running_/connected_. S2 stays green — the overall deadline is
# still there — and S3 is what dies: the frame keeps being served to a peer that is no longer going
# to take it, and the sender thread is only released when the transfer finishes. Proves the alive
# check is load-bearing and is NOT covered by ~Channel()'s shutdown(SHUT_RDWR), which is why S3 marks
# the channel dead with the peer's FIN instead of by tearing the channel down.
MS4=$(fresh_copy mutant-no-alive-check)
if apply_mutant sw-mutant-no-alive-check "$MS4/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        if (!running_ || !connected_) {"
assert s.count(old) == 1, "alive check not found"
open(p, "w").write(s.replace(old, "        if (false) {  // MUTANT: teardown cannot interrupt the write", 1))
PY
then build_and_run_channel sw-mutant-no-alive-check "$ROOT/qa/socket-write-order.cpp" "$MS4" fail
fi

echo

# build_and_run_tree <name> <test.cpp> <omt-src-dir> <expect pass|fail> [vmx-source-dir|STUB]
#   Like build_and_run, but <omt-src-dir> goes FIRST on the include path so the scenario compiles
#   against that copy's HEADERS as well as its sources. Required for every pre-fix run: the tree's
#   OMTSender.h now declares members (EncoderIdentity, buildVideoHeaders, the identity-capturing
#   encodeFrame) that the old .cpp never defines, so mixing the two is a compile error — and a
#   compile error is not a red run, it is an absent one.
build_and_run_tree() {
  local name=$1 test=$2 srcdir=$3 expect=$4 vmxdir=${5:-$ROOT/libvmx/src}
  local vmxsrcs
  if [ "$vmxdir" = "STUB" ]; then
    vmxsrcs="$ROOT/qa/stub_vmx.cpp"
  else
    vmxsrcs="$vmxdir/vmxcodec.cpp $vmxdir/vmxcodec_x86.cpp $vmxdir/vmxcodec_avx2.cpp"
  fi
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS -I"$srcdir" $INC "$test" "$srcdir/OMTChannel.cpp" "$srcdir/OMTAsyncPool.cpp" \
       "$srcdir/OMTSender.cpp" "$srcdir/OMTDiscovery.cpp" $vmxsrcs \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -10
    rc=1; return
  fi
  score_run "$name" "$expect"
}

# extract_libomt <dir> <git-ref> — a whole libomt/cpp (sources AND headers) as it stood at <git-ref>,
# for a pre-fix run. The caller must verify the extraction actually looks pre-fix.
extract_libomt() {
  local d="$SCRATCH/$1" ref=$2
  rm -rf "$d"; mkdir -p "$d"
  for f in OMTChannel.cpp OMTAsyncPool.cpp OMTSender.cpp OMTDiscovery.cpp \
           OMTChannel.h OMTSender.h OMTDiscovery.h OMTConstants.h OMTProtocol.h OMTAsyncPool.h; do
    git show "$ref:libomt/cpp/$f" > "$d/$f" 2>/dev/null || { echo "  cannot extract $f from $ref"; rc=1; }
  done
  echo "$d"
}

echo
echo "== encoder-reconfiguration: the header may only describe the encoder that made the payload =="
# The defect: createEncoder() reused a VMX instance on width+height alone while the colour space it
# was handed decided the codec's RGB<->YUV table and the frame's colour space went into the header.
# The scenario links the REAL codec and compares each payload against independently created reference
# encoders, so "which matrix made these bytes" is measured, not asserted from the sender's own cache.
grep -q 'currentColorSpace_ == colorSpace' "$ROOT/libomt/cpp/OMTSender.cpp" \
  || { echo "  the tree does not contain the encoder-key fix — there is nothing to score"; rc=1; }
build_and_run er-fixed "$ROOT/qa/encoder-reconfiguration.cpp" "$ROOT/libomt/cpp" pass

# PINNED, not origin/master: a moving ref here silently turns the must-fail control into a no-op the
# day the fix lands upstream (exactly the defect class fixed for RPBASE/HPBASE today). d9814679 is the
# commit this branch was written against and is genuinely pre-fix. Override with QA_ER_BASE=<rev>.
BASE_ER=${QA_ER_BASE:-d9814679}
require_rev "$BASE_ER" "pre-fix source of encoder-reconfiguration"
ER_OLD=$(extract_libomt pre-fix-encoder "$BASE_ER")
grep -q 'currentColorSpace_ == colorSpace' "$ER_OLD/OMTSender.cpp" \
  && { echo "  the pre-fix copy ALREADY has the key fix — extraction from $BASE_ER is wrong"; rc=1; }
build_and_run_tree er-prefix "$ROOT/qa/encoder-reconfiguration.cpp" "$ER_OLD" fail

# ME1: the key clause alone, back out. This is the same regression as er-prefix, isolated to one
# condition, so a future refactor that keeps the cache but drops the comparison cannot pass unnoticed.
ME1=$(fresh_copy mutant-encoder-key)
python3 - "$ME1/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        currentColorSpace_ == colorSpace) {"
assert s.count(old) == 1, "encoder-reuse colour-space key clause not found"
open(p, "w").write(s.replace(old, "        true) {  // MUTANT: the reuse key is back to width+height alone", 1))
PY
build_and_run er-mutant-key "$ROOT/qa/encoder-reconfiguration.cpp" "$ME1" fail

echo
echo "== wire-declaration: the declaration is built from the encoder, never from the caller's struct =="
build_and_run wd-fixed "$ROOT/qa/wire-declaration.cpp" "$ROOT/libomt/cpp" pass STUB
build_and_run wd-mutant-key "$ROOT/qa/wire-declaration.cpp" "$ME1" fail STUB

# MW1 WAS ATTEMPTED AND WITHDRAWN (not scored, on purpose). Mutating the published value
# `ext.colorSpace = encoder.colorSpace` back to `frame.colorSpace` cannot be turned red by any test
# while MW2's tripwire is present: buildVideoHeaders() refuses every frame where the two differ, so
# on every input the function accepts they are equal and the two spellings are the same program. It
# is an equivalent mutant, and scoring an equivalent mutant as "must fail" would either fail the
# suite forever or push someone to invent an assertion that cannot distinguish anything. What the
# pair means: publishing from the encoder is the belt, the tripwire is the brace, and the falsifiable
# property is the conjunction — MW2 (tripwire removed) is its control, and er-mutant-key / er-prefix
# are the controls for the key that keeps them equal on the live path. If the tripwire is ever
# deleted, restore MW1 here and it will go red on its own.
# MW2: the mismatch tripwire removed — a contradictory (frame, encoder) pair is then published as a
# frame instead of refused. This is the scored control for the whole declaration-honesty clause.
MW2=$(fresh_copy mutant-tripwire)
python3 - "$MW2/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (encoder.colorSpace != frame.colorSpace)"
assert s.count(old) == 1, "the encoder/frame colour-space tripwire is not where it was left"
open(p, "w").write(s.replace(old, "    if (false && encoder.colorSpace != frame.colorSpace)", 1))
PY
build_and_run wd-mutant-tripwire "$ROOT/qa/wire-declaration.cpp" "$MW2" fail STUB

# MW3: the belt on the value that reaches the wire (0/601/709), independent of validateFrame.
MW3=$(fresh_copy mutant-declare-belt)
python3 - "$MW3/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (encoder.colorSpace != ColorSpace::Undefined &&\n        encoder.colorSpace != ColorSpace::BT601 &&\n        encoder.colorSpace != ColorSpace::BT709)\n        return reject(\"the encoder reports colour space \""
assert s.count(old) == 1, "the header builder's colour-space belt not found"
open(p, "w").write(s.replace(old, "    if (false)\n        return reject(\"the encoder reports colour space \"", 1))
PY
build_and_run wd-mutant-belt "$ROOT/qa/wire-declaration.cpp" "$MW3" fail STUB

# MW4: the size clause. A header may not carry a size the encoder does not have.
MW4=$(fresh_copy mutant-declare-size)
python3 - "$MW4/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (encoder.width != frame.width || encoder.height != frame.height)"
assert s.count(old) == 1, "the encoder/frame size clause not found"
open(p, "w").write(s.replace(old, "    if (false && (encoder.width != frame.width || encoder.height != frame.height))", 1))
PY
build_and_run wd-mutant-size "$ROOT/qa/wire-declaration.cpp" "$MW4" fail STUB

# MW5: the "no encoder, no declaration" clause.
MW5=$(fresh_copy mutant-declare-noencoder)
python3 - "$MW5/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (!encoder.valid)\n        return reject(\"no encoder was captured"
assert s.count(old) == 1, "the encoder.valid clause not found"
open(p, "w").write(s.replace(old, "    if (false)\n        return reject(\"no encoder was captured", 1))
PY
build_and_run wd-mutant-noencoder "$ROOT/qa/wire-declaration.cpp" "$MW5" fail STUB

# MW6: the length clause — a payload that does not exist cannot be declared.
MW6=$(fresh_copy mutant-declare-nolength)
python3 - "$MW6/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (encodedLen <= 0)\n        return reject("
assert s.count(old) == 1, "the encodedLen clause not found"
open(p, "w").write(s.replace(old, "    if (false)\n        return reject(", 1))
PY
build_and_run wd-mutant-nolength "$ROOT/qa/wire-declaration.cpp" "$MW6" fail STUB

# MW7: the identity capture itself. If encodeFrame() stopped reporting which instance wrote the
# payload, every frame would be refused — this is what proves the declaration path is really on the
# wire rather than a decoration the sender could skip.
MW7=$(fresh_copy mutant-no-identity)
python3 - "$MW7/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        used->valid = (currentWidth_ == frame.width && currentHeight_ == frame.height &&\n                       currentColorSpace_ == frame.colorSpace);"
assert s.count(old) == 1, "the identity capture not found"
open(p, "w").write(s.replace(old, "        used->valid = false;  // MUTANT: the producer never identifies itself", 1))
PY
build_and_run wd-mutant-noident "$ROOT/qa/wire-declaration.cpp" "$MW7" fail STUB
build_and_run er-mutant-noident "$ROOT/qa/encoder-reconfiguration.cpp" "$MW7" fail

echo
echo "== declaration-gate: no claim on the wire that the payload does not carry =="
build_and_run dg-fixed "$ROOT/qa/declaration-gate.cpp" "$ROOT/libomt/cpp" pass STUB

DG_OLD=$(extract_libomt pre-fix-decl "$BASE_ER")
grep -q 'HighBitDepth claimed' "$DG_OLD/OMTSender.cpp" \
  && { echo "  the pre-fix copy ALREADY has the flag gates — extraction from $BASE_ER is wrong"; rc=1; }
build_and_run_tree dg-prefix "$ROOT/qa/declaration-gate.cpp" "$DG_OLD" fail STUB

# One mutant per new clause. Each is expected to let exactly its own case through.
dg_mutant() { # <slug> <anchor> <replacement>
  local name=$1 old=$2 new=$3
  local M; M=$(fresh_copy "mutant-$1")
  python3 - "$M/OMTSender.cpp" "$old" "$new" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert s.count(old) == 1, "anchor not found (or not unique): " + old[:70]
open(p, "w").write(s.replace(old, new, 1))
PY
  build_and_run "dg-mutant-$1" "$ROOT/qa/declaration-gate.cpp" "$M" fail STUB
}

dg_mutant highbitdepth \
  '    if (frame.flags & VideoFlags::HighBitDepth)' \
  '    if (false && (frame.flags & VideoFlags::HighBitDepth))'
dg_mutant alpha-plane \
  '    if ((frame.flags & VideoFlags::Alpha) && frame.codec != Codec::BGRA)' \
  '    if (false && (frame.flags & VideoFlags::Alpha) && frame.codec != Codec::BGRA)'
dg_mutant premultiplied \
  '    if ((frame.flags & VideoFlags::PreMultiplied) && !(frame.flags & VideoFlags::Alpha))' \
  '    if (false && (frame.flags & VideoFlags::PreMultiplied) && !(frame.flags & VideoFlags::Alpha))'
dg_mutant unknown-bits \
  '    if (frame.flags & ~knownFlags) {' \
  '    if (false && (frame.flags & ~knownFlags)) {'
dg_mutant interlace-claim \
  '    if ((frame.flags & VideoFlags::Interlaced) && !interlacedHeightSupported(frame.height))' \
  '    if (false && (frame.flags & VideoFlags::Interlaced) && !interlacedHeightSupported(frame.height))'
dg_mutant aspect-ratio \
  '    if (!std::isfinite(frame.aspectRatio) || frame.aspectRatio <= 0.0f)' \
  '    if (false && (!std::isfinite(frame.aspectRatio) || frame.aspectRatio <= 0.0f))'
# The interlace heights, widened to "anything": the header would then advertise a field order the
# codec silently declined to produce.
dg_mutant interlace-heights \
  '    return height == 480 || height == 576 || height == 1080;' \
  '    return true;  // MUTANT: the codec never consulted about the field order'
# The flag NUMBERING, not a gate: HighBitDepth back to 4 (which is PreMultiplied on the wire), with
# the compile-time pin taken out so the mutant builds and the runtime assertion is what catches it.
MNUM=$(fresh_copy mutant-flag-numbering)
python3 - "$MNUM/OMTProtocol.h" "$MNUM/OMTSender.cpp" <<'PY'
import sys
proto, sender = sys.argv[1], sys.argv[2]
s = open(proto).read()
old = "    constexpr int32_t HighBitDepth = 16;"
assert s.count(old) == 1, "HighBitDepth constant not found"
open(proto, "w").write(s.replace(old, "    constexpr int32_t HighBitDepth = 4;  // MUTANT: the old wrong number", 1))
s = open(sender).read()
old = "static_assert(VideoFlags::Interlaced == 1 && VideoFlags::Alpha == 2 &&"
assert s.count(old) == 1, "the flag static_assert pin not found"
i = s.index(old); j = s.index(');\n', i) + 3
open(sender, "w").write(s[:i] + "  // MUTANT: the compile-time pin removed so the wrong number builds\n" + s[j:])
PY
build_and_run_tree dg-mutant-flag-numbering "$ROOT/qa/declaration-gate.cpp" "$MNUM" fail STUB

echo


echo
if [ "$HARNESS_ERR" -ne 0 ]; then
  echo "QA FAILED — the harness itself could not express every scenario (see QA HARNESS ERROR lines above)"
  [ ${#FAILED[@]} -gt 0 ] && echo "  failing scenarios: ${FAILED[*]}"
  exit 2
fi
if [ $rc -ne 0 ]; then
  echo "QA FAILED"
  [ ${#FAILED[@]} -gt 0 ] && echo "  red scenarios: ${FAILED[*]}"
  exit 1
fi
echo "QA PASSED (every gate has a scenario where disabling it turns the suite red)"
