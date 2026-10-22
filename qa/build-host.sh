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

extract_rev_file() { # <rev> <path> <dest> [scenario] — `git show ... > f` ALONE leaves an EMPTY f
                     # behind when the rev or path is bad, and the scenario then dies in the linker
                     # looking like a product defect; abort before that can happen. The optional
                     # scenario name rides into every message: an extraction that comes up empty is
                     # the harness failing to express ONE control, and whoever reads the rc=2 must
                     # not have to reverse-engineer which one out of a scratch path.
  local why="source of $2"; [ -n "${4:-}" ] && why="$why for the $4 control"
  local at=""; [ -n "${4:-}" ] && at=" ($4)"
  require_rev "$1" "$why"
  git show "$1:$2" > "$3.part" 2>"$SCRATCH/git-show.err" \
    || harness_die "git show $1:$2 failed$at: $(tr '\n' ' ' < "$SCRATCH/git-show.err")"
  [ -s "$3.part" ] || harness_die "git show $1:$2 produced an empty file$at — refusing to build a scenario out of nothing"
  mv "$3.part" "$3"
}

# A pre-fix copy must be VERIFIABLY pre-fix: a pin that silently points after the fix turns a
# must-fail scenario into a must-pass lie (that is exactly how `git show master:` rotted — PR #13
# merged, master moved under the harness, and ss-prefix started passing the gate it exists to
# prove). Grep the copy both ways: the old shape present, the fix absent. The optional third
# argument names the scenario in the harness error, same reason as extract_rev_file's fourth.
must_grep()     { grep -q "$2" "$1" || harness_die "pre-fix check${3:+ ($3)}: $1 lacks /$2/ — the extracted copy is not the pre-fix tree this scenario claims to run"; }
must_not_grep() { if grep -q "$2" "$1"; then harness_die "pre-fix check${3:+ ($3)}: $1 still contains /$2/ — the gate was never removed; scoring this copy as a caught mutant would be a lie"; fi; }

apply_mutant() { # <scenario> <file> — python patch text on stdin. An anchor that no longer
                 # matches means the gate was NEVER disabled: the copy is still the fixed code, and
                 # scoring it as "expected fail" would blame the test for the harness's drift.
  local name=$1 f=$2
  if python3 - "$f"; then return 0; fi
  mark_harness_err "$name: mutant could not be applied — the anchor is gone, so the gate was never disabled; refusing to score an unpatched copy"
  FAILED+=("$name(anchor-missing)")
  return 1
}

# apply_test_seam <scenario> <file> — restore the one thing an extracted pre-fix copy must not lose:
# the visibility seam a scenario's subclass needs in order to reach the PRODUCTION implementation of
# a callback it is trying to collide with. It is not a gate and reverting it does not revert a fix;
# it is the test's ability to observe. Same stdin-patch discipline as apply_mutant, but it aborts
# (rc=2) rather than continuing: without the seam the scenario cannot compile, and a control that
# dies at the compiler is exactly the outcome this harness must never report as a verdict. The anchor
# lives in a file pinned to an immutable commit, so it matches forever or it never did.
apply_test_seam() {
  local name=$1 f=$2
  if python3 - "$f"; then return 0; fi
  harness_die "$name: the test seam could not be restored in $f — the anchor is gone, so the extracted copy is not the shape this control was written against"
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
build_and_run_sender() { # <name> <test.cpp> <omt-src-dir> <expect pass|fail>
  # Headers from the SAME copy as the sources — the rule build_and_run_channel encodes for Channel,
  # applied to Sender. This pre-fix copy replaces OMTSender.h as well as OMTSender.cpp (a class and
  # its out-of-line definitions are one program, see the pin note below), so the test and every TU
  # must compile the class from the copy's header: linking the copy's .cpp against the tree's header
  # reads channels_/encoderMutex_ at the wrong offsets, which is the hazard that once returned
  # staleEvicted() == -4702111234474983746 — "not evidence of anything" (rule 15). The codec itself
  # is not under test here and comes from the tree. Named for this section, like
  # build_and_run_threadtask, so the sibling QA branches stay independently mergeable.
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

echo "== stop-deadlock: teardown must not hold channelsMutex_ across ~Channel()'s join =="
build_and_run sd-fixed "$ROOT/qa/stop-deadlock.cpp" "$ROOT/libomt/cpp" pass

# The pre-fix run: the stop-deadlock fix removed — and removed as a PROGRAM, not as one file.
#
# A .cpp does not compile on its own: it is one half of a class whose other half is its own header,
# so pinning OMTSender.cpp to a pre-fix revision while the headers stay the tree's asks the old
# source to define the class the way TODAY declares it. That is exactly what rotted here. The
# previous pin was d9814679 (set by 8af9fb33: master's tip at audit time, so pre-fix for the
# deadlock), and when PR #29 (026d9d34) turned the private
#     int encodeFrame(const MediaFrame& frame);
# into
#     int encodeFrame(const MediaFrame& frame, EncoderIdentity* used = nullptr);
# the extracted file stopped matching its own class:
#     error: out-of-line definition of 'encodeFrame' does not match any declaration in 'omt::Sender'
# sd-prefix has been red as (build-failed) ever since. A must-fail control that dies at the compiler
# is not a caught mutant — it is the harness failing to express the control, reported as though it
# were a verdict, and the suite goes red for a reason that is not this gate.
#
# So both halves of the class come from ONE immutable revision, the way the aligned-stride section
# already extracts vmxcodec_common.h together with the .cpp files that include it (BASE_A): a .cpp
# and the header it was written against, pinned to the same commit, cannot drift apart again.
#
# That revision is 026d9d34 — not d74031e3^, and not "master's tip at audit time". d74031e3 (PR #24,
# merged as 6905bae1) and 026d9d34 (PR #29, merged as dfc09582) are the only two commits that have
# touched these two files since, they sit on sibling branches (d74031e3 is NOT an ancestor of
# 026d9d34: the colour-space-key branch never saw the deadlock fix), so this pair is the tree with
# EXACTLY the commit under test removed:
#     diff(026d9d34..master, libomt/cpp/OMTSender.cpp) == the diff of d74031e3  (swap, then destroy)
#     diff(026d9d34..master, libomt/cpp/OMTSender.h)   == the diff of d74031e3  (the seam, below)
# Pinning d9814679 or d74031e3^ instead would ALSO remove PR #29's encoder-identity work from the
# same translation unit — a second gate silently disabled in a scenario that is not about it, the
# hazard the receiver-validation section names outright ("the scenario could have gone red for
# reasons that are not this gate").
BASE_SD=${QA_BASE:-026d9d34}
PFS="$SCRATCH/pre-fix-stop"; rm -rf "$PFS"; mkdir -p "$PFS"
cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$PFS"/
extract_rev_file "$BASE_SD" "libomt/cpp/OMTSender.cpp" "$PFS/OMTSender.cpp" sd-prefix
extract_rev_file "$BASE_SD" "libomt/cpp/OMTSender.h"   "$PFS/OMTSender.h"   sd-prefix
must_grep     "$PFS/OMTSender.cpp" 'channels_.clear();'      sd-prefix
must_not_grep "$PFS/OMTSender.cpp" 'doomed.swap(channels_)'  sd-prefix
# Only the commit under test is reverted: PR #29's declaration builder is still in the .cpp and the
# encoder identity it derives the wire from is still in the header. If a re-pin ever points the pair
# at a revision that quietly drops #29 as well, these two lines say so — instead of the scenario
# going red for a gate nobody is testing here.
must_grep     "$PFS/OMTSender.cpp" 'buildVideoHeaders'       sd-prefix
must_grep     "$PFS/OMTSender.h"   'EncoderIdentity'         sd-prefix
# The one thing the pre-fix copy must NOT remove is the test's own observation point. d74031e3
# widened the three IChannelListener overrides from private to protected because
# qa/stop-deadlock.cpp's subclass chains to the PRODUCTION Sender::onQualityChanged() after parking
# the receiver thread on a gate it controls; with the overrides private that qualified call is a
# compile error ("'onQualityChanged' is a private member of 'omt::Sender'"), and a subclass that
# re-implements the callback instead of calling it measures nothing — the deadlock would be tested
# against a copy of the callback, not the callback. Reverting the fix must not revert the ability to
# observe it, so the seam is restored on the extracted copy. Its anchor is text pinned to an
# immutable commit: it matches forever or it never did, and apply_test_seam makes the second case a
# harness error rather than the build failure this section just escaped.
apply_test_seam sd-prefix "$PFS/OMTSender.h" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("    // IChannelListener implementation\n"
       "    void onDisconnected(Channel* channel) override;\n"
       "    void onTallyChanged(Channel* channel) override;\n"
       "    void onQualityChanged(Channel* channel) override;\n")
assert s.count(old) == 1, "listener override block not found to widen"
new = ("    // IChannelListener implementation — protected, not private: these are the override\n"
       "    // points of a public interface, and qa/stop-deadlock.cpp chains to the production\n"
       "    // callback. Restored by the harness; nothing else in this header was touched.\n"
       "protected:\n"
       "    void onDisconnected(Channel* channel) override;\n"
       "    void onTallyChanged(Channel* channel) override;\n"
       "    void onQualityChanged(Channel* channel) override;\n\n"
       "private:\n")
open(p, "w").write(s.replace(old, new, 1))
PY
must_grep "$PFS/OMTSender.h" '^protected:' sd-prefix

build_and_run_sender sd-prefix "$ROOT/qa/stop-deadlock.cpp" "$PFS" fail

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

# ---------------------------------------------------------------------------
# jni-boundary: the same discipline one level up, at the JNI entry point.
#
# qa/jni-boundary.cpp compiles app/src/main/cpp/OMTBridge.cpp — the production translation unit, not a
# transcription — against qa/host-jni/jni.h, and links the real sender and the real codec behind it.
# The JNIEnv it drives is the test's, so the test decides what each direct buffer's ADDRESS and
# CAPACITY are, and every plane is malloc'd to exactly the capacity the bridge is told about. That is
# what turns "the bridge trusted a caller-supplied length" into an ASan heap-buffer-overflow instead
# of a code review finding: jb-mutant-no-admission deletes the vetting and the same binary dies.
# ---------------------------------------------------------------------------
# build_jni <name> <expect pass|fail> <OMTBridge.cpp to compile> [extra compiler flags...]
# The directory holding that OMTBridge.cpp goes FIRST on the include path, so a mutant's copy of
# omtnv12.h really is the header the bridge compiles against.
build_jni() {
  local name=$1 expect=$2 bridge=$3; shift 3
  local extra="$*"
  [ -f "$bridge" ] || harness_die "build_jni $name: no such bridge source: $bridge"
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS $extra $INC -I"$(dirname "$bridge")" -I"$ROOT/qa/host-jni" -I"$ROOT/app/src/main/cpp" \
       "$ROOT/qa/jni-boundary.cpp" "$bridge" \
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

fresh_copy_jni() { # <dir> — the bridge AND the header it includes, so either can be the mutant
  local d="$SCRATCH/$1"; rm -rf "$d"; mkdir -p "$d"
  cp "$ROOT"/app/src/main/cpp/OMTBridge.cpp "$ROOT"/app/src/main/cpp/omtnv12.h "$d"/ \
    || { echo "QA HARNESS ERROR: cannot copy the JNI bridge into $d" >&2; return 1; }
  echo "$d"
}

mutate_jni() { # <dir> <file> <old> <new> <what> — one property of the boundary sabotaged
  local d=$1 f=$2
  python3 - "$d/$f" "$3" "$4" "$5" <<'PY'
import sys
p, old, new, what = sys.argv[1:5]
s = open(p).read()
assert s.count(old) == 1, "cannot find the clause to mutate (%s): %r" % (what, old[:70])
open(p, "w").write(s.replace(old, new, 1))
PY
  if [ $? -ne 0 ]; then echo "  $5: MUTATION FAILED (the clause is gone — the boundary would pass by forgetting)"; rc=1; fi
}

echo "== planar-chroma: YUV_420_888 is two layouts, and the sender must serve both =="
build_planar pc-fixed pass

# The pre-fix build: same scenarios, same assertions, the capture path as it stood before this
# branch. Red on case B (the emulator's planar I420) is the point of the whole exercise.
build_planar pc-prefix fail -DOMT_QA_PLANAR_PREFIX

# MP1: pixelStride ignored — the packer samples every component two bytes apart because that is what
# an NV12 device happens to do. A planar Cb plane read this way skips half its samples and then walks
# off the end of a plane allocated to its own reported limit.
MP1=$(fresh_copy_bridge mutant-pixelstride)
mutate_bridge "$MP1" \
  '                row[2 * c]     = srcU[c * plan.cb.pixelStride];
                row[2 * c + 1] = srcV[c * plan.cr.pixelStride];' \
  '                row[2 * c]     = srcU[c * 2];   // MUTANT: the HAL was asked nothing
                row[2 * c + 1] = srcV[c * 2];' \
  "ignore pixelStride"
build_planar pc-mutant-pixelstride fail -I"$MP1"

# MP2: the interleave forgotten — a planar Cb plane wearing Cr's clothes. Content, not size: only a
# byte-by-byte assertion sees it, which is why the harness compares against the source planes.
MP2=$(fresh_copy_bridge mutant-no-interleave)
mutate_bridge "$MP2" \
  '                row[2 * c + 1] = srcV[c * plan.cr.pixelStride];' \
  '                row[2 * c + 1] = srcU[c * plan.cb.pixelStride];  // MUTANT: the Cr plane is never read' \
  "forget to interleave"
build_planar pc-mutant-no-interleave fail -I"$MP2"

# MP3: THE ORIGINAL DEFECT, one line wide — the chroma row walked with the luma stride. The size
# pre-checks still pass (they use the plane's own rowStride), so this dies to ASan reading past the
# end of a Cb plane allocated to exactly its reported limit. That report is the proof.
MP3=$(fresh_copy_bridge mutant-chroma-luma-stride)
mutate_bridge "$MP3" \
  '            const uint8_t* srcU = plan.cb.data + r * static_cast<int64_t>(plan.cb.rowStride);
            const uint8_t* srcV = plan.cr.data + r * static_cast<int64_t>(plan.cr.rowStride);' \
  '            const uint8_t* srcU = plan.cb.data + r * stride;   // MUTANT: the luma stride, as pre-fix
            const uint8_t* srcV = plan.cr.data + r * stride;' \
  "chroma walked with the luma stride"
build_planar pc-mutant-chroma-luma-stride fail -I"$MP3"

# MP4: the frame charged with the SOURCE uv rowStride instead of the stride the encoder will use —
# OMTBridge.cpp:121's arithmetic, smuggled back into the packer. The gate refuses it, which is the
# whole reason the defect was diagnosable at all.
MP4=$(fresh_copy_bridge mutant-old-arithmetic)
mutate_bridge "$MP4" \
  '    const int64_t dataLength = h * stride + chromaRows * stride;' \
  '    const int64_t dataLength = h * stride + chromaRows * (plan.layout == Layout::PlanarI420 ? (int64_t)u.rowStride : stride);  // MUTANT: pre-fix arithmetic' \
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

# ---------------------------------------------------------------------------
# MQ1..MQ3: the gates that answer the second half of the defect — pixelStride==2 does NOT mean "the U
# buffer is a complete contiguous NV12 plane". Each mutant restores one of the three guesses the old
# code made, and each has to die. The anti-vacuity note under MQ1 is the reason the byte oracle in
# qa/nv12-oracle.h is written from the descriptors rather than from the packer's own decisions.
# ---------------------------------------------------------------------------

# MQ1: THE DEFECT, restored verbatim — copy the U buffer whenever pixelStride says 2. It dies to ASan
# on V1, the VU-backed shared buffer: the memcpy reads a full width-byte row from a Cb window that
# stops at its last Cb sample, i.e. 256 bytes past the end of the plane. Behind that crash sit the two
# byte-level deaths the same mutant causes and the oracle is written for — V3 (a VU pair whose window
# does cover the row) and S1/S2 (separate stride-2 allocations), which put the neighbour's Cr, or the
# HAL's own 0xFF padding, into every Cr slot. qa/build-host.sh scores the mutant as dead either way;
# the same two byte patterns are on record against the pre-fix packer in the pc-on-prepacker run.
MQ1=$(fresh_copy_bridge mutant-copy-u-on-pixelstride)
mutate_bridge "$MQ1" \
  '    if (plan.verbatimU) {' \
  '    if (u.pixelStride == 2) {  // MUTANT: pixelStride is the whole answer, as it used to be' \
  "copy the U buffer on pixelStride alone"
build_planar pc-mutant-copy-u-on-pixelstride fail -I"$MQ1"

# MQ2: the VU pair read in the wrong order — planes[1] is Cb because the CONTRACT says so, but this
# mutant takes the byte the HAL happened to put first. Cb lands in the Cr slots and vice versa; the
# fixture keeps the two components in disjoint value ranges precisely so this cannot pass.
MQ2=$(fresh_copy_bridge mutant-vu-ordering)
mutate_bridge "$MQ2" \
  '        plan->cb = u;   // MQ2 anchor: exchanging these two lines is the VU ordering defect — Cb is the
        plan->cr = v;   // byte the HAL pointed planes[1] at, whichever half of the pair that happens to be' \
  '        plan->cb = v;   // MUTANT: whichever byte of the pair came first, called it Cb
        plan->cr = u;' \
  "read a VU-backed pair in UV order"
build_planar pc-mutant-vu-ordering fail -I"$MQ2"

# MQ3: only the U window is bounded — the exact shape of the old code, which validated U and then
# copied U, and never looked at the V buffer at all. It is killed by D7 alone: the packer's promise is
# "nothing is read outside the plane's own window", and with the check gone the last Cr is read from
# outside the window while still sitting inside the allocation, so no ASan report is available and the
# named refusal is the only evidence there is. (A read outside the ALLOCATION is defect (2)'s shape,
# and jb-mutant-no-admission is where that one is proven by ASan.)
MQ3=$(fresh_copy_bridge mutant-bound-only-u)
mutate_bridge "$MQ3" \
  '    const Bound bounds[2] = { { "Cb", &plan.cb }, { "Cr", &plan.cr } };' \
  '    const Bound bounds[1] = { { "Cb", &plan.cb } };  // MUTANT: the V buffer is never asked how long it is' \
  "bound only the Cb window"
build_planar pc-mutant-bound-only-cr fail -I"$MQ3"

# WITHDRAWN MUTANT — recorded here so nobody re-adds it and calls the result evidence.
#
# The candidate was uWindowIsTheWholePlane() -> `return true`, i.e. drop the "the window reaches the
# last byte of the last pair" condition from the memcpy fast path. It is EQUIVALENT, and here is the
# proof by exhaustion rather than by hope: the function is called from exactly one place, the off==+1
# branch of planChroma(), which has already proved the two planes share one allocation. Every case
# that reaches it (A, A2, C1, C2, T1) has a shared chroma allocation whose union of the two reported
# windows covers the full last row — so the forced memcpy reads only bytes that planes[2]'s own window
# already guarantees, and it writes the same bytes the per-sample path would have written. No
# assertion can distinguish the two, and ASan cannot either, because the read is inside the image.
# What IS load-bearing about that condition is its false branch, and T1 pins it: a Cb view trimmed to
# its last sample must still be ACCEPTED (the pre-fix packer refused that frame outright). An
# assertion that cannot fail is worse than no assertion — it is the reason the suite would then
# survive the bug it was written for.

echo
echo "== jni-boundary: the bridge may only read what the buffer it was handed actually holds =="
# The scenario links the REAL app/src/main/cpp/OMTBridge.cpp — not a transcription of it — against
# qa/host-jni/jni.h, over the real sender and the real codec. Its *Size arguments are caller-supplied
# ints, and every bounds check the packer has trusts them; only GetDirectBufferCapacity() does not.
build_jni jb-fixed pass "$ROOT/app/src/main/cpp/OMTBridge.cpp"

# JB1: the admission loop removed. The claims in B1..B12 then reach the packer, whose checks are all
# `claimed >= need`, and the reads walk past plane allocations malloc'd to exactly the capacity the
# bridge was shown. The ASan heap-buffer-overflow READ inside packNv12 is the verdict — that report,
# not an assertion, is the proof the read was real.
JB1=$(fresh_copy_jni mutant-jni-no-admission)
mutate_jni "$JB1" OMTBridge.cpp \
  '    for (int i = 0; i < 3; ++i) {
        if (!omtpack::admitSource(claims[i], planeNames[i], why, static_cast<int>(sizeof(why)))) {' \
  '    for (int i = 0; i < 0; ++i) {   // MUTANT: the caller word is good enough, as it used to be
        if (!omtpack::admitSource(claims[i], planeNames[i], why, static_cast<int>(sizeof(why)))) {' \
  "never vet the caller's lengths"
build_jni jb-mutant-no-admission fail "$JB1/OMTBridge.cpp"

# JB2: the comparison off by one. A claim one byte above the capacity then passes, which is the
# smallest lie that still produces an out-of-bounds read (B1/B2/B3 and B9..B12 are exactly it). This
# mutant is what proves the gate is `>` and not `>=`: without it, "refuse everything" would satisfy
# the same scenarios.
JB2=$(fresh_copy_jni mutant-jni-off-by-one)
mutate_jni "$JB2" omtnv12.h \
  '    if (c.claimed > c.capacity) {' \
  '    if (c.claimed > c.capacity + 1) {  // MUTANT: one byte of trust is still trust' \
  "vet the claims with an off-by-one"
build_jni jb-mutant-off-by-one fail "$JB2/OMTBridge.cpp"

# JB3: the negative-length clause removed. A negative claim is not merely rude — it is the operand of
# every `claimed >= need` comparison in the packer, so it must be refused where the refusal can name
# the buffer. Without it the frame is refused lower down, by a message about sizes, and the operator
# reading logcat cannot tell a short plane from a lie about one.
JB3=$(fresh_copy_jni mutant-jni-no-negative)
mutate_jni "$JB3" omtnv12.h \
  '    if (c.claimed < 0) {' \
  '    if (false) {  // MUTANT: a negative length is just a very large need' \
  "stop refusing negative lengths by name"
build_jni jb-mutant-no-negative fail "$JB3/OMTBridge.cpp"

# JB4: the nanosecond-to-tick conversion dropped. The wire field is 100 ns ticks; handing it
# microseconds makes every frame look 10x too close, which changes the pacing a receiver derives and
# silences its gap detection. C1i is the only assertion that can see it.
JB4=$(fresh_copy_jni mutant-jni-pts-units)
mutate_jni "$JB4" OMTBridge.cpp \
  '    frame.Timestamp = ptsNs / 100;' \
  '    frame.Timestamp = ptsNs;  // MUTANT: microseconds on a 100 ns wire field' \
  "drop the tick conversion on the coded path"
build_jni jb-mutant-pts-units fail "$JB4/OMTBridge.cpp"

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


# ==================================================================================================
# receiver-validation: a peer that connects to port 6400 must not be able to spin the receiver
# thread at 100% CPU or make ~Channel() hang. Two defects, one file:
#   (1) the control frame was parsed straight out of the receive buffer and its SIGNED int32
#       dataLength went into `size_t totalFrameLen = FrameHeader::SIZE + header->dataLength` before
#       anything validated it. dataLength = -16 makes that 0, the shift at the bottom of the parse
#       loop consumes 0 bytes, and the same 16 bytes are re-parsed forever with no flag re-read —
#       a permanent core burn from one connect(), and ~Channel() joins that thread so the stream
#       cannot be stopped either.
#   (2) FD_SET(socketFd_) on a descriptor >= FD_SETSIZE (1024) writes outside the stack-resident
#       fd_set. Routine on a phone that has been up for a day. poll() has no ceiling.
# The pre-fix copy is the whole file from the revision this branch sits on; the header stays the
# tree's, so every TU agrees on Channel's layout (the hazard that produced the bogus
# staleEvicted() == -4702111234474983746 read once a mutant's header differed — see rule 15).
echo "== receiver-validation: header fields are validated before they reach the arithmetic =="
build_and_run rv-fixed "$ROOT/qa/receiver-validation.cpp" "$ROOT/libomt/cpp" pass

PFR="$SCRATCH/pre-fix-recv"; rm -rf "$PFR"; mkdir -p "$PFR"
cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$PFR"/
# Pinned to adf620a3 — the last commit on master to touch OMTChannel.cpp before this branch.
# The branch's original pin, d9814679, predated #22: its OMTChannel.cpp defines the 3-arg
# sendWithDeadline(data, length, timeoutMs), the tree's header declares the 4-arg
# sendWithDeadline(fd, data, length, timeoutMs), and the extracted copy stopped compiling
# against it the moment #22 merged (rv-prefix(build-failed) — refused to score, never a lie).
# adf620a3's file is exactly "this branch's product commit, removed" against the current tree:
# same FD_SET-in-receiverLoop defect, same absent validation, API-matched to the header. The
# pin is an immutable merged sha, so it stays truthful after this branch merges too. Headers
# stay the tree's so every TU agrees on Channel's layout (rule 15 — the CloseReason accessors
# the test asserts on are inline in the header, and the pre-fix .cpp simply never calls them:
# every close reads "none", which is the honest pre-fix observable).
BASE_RV=${QA_BASE:-adf620a3}
extract_rev_file "$BASE_RV" "libomt/cpp/OMTChannel.cpp" "$PFR/OMTChannel.cpp"
must_grep     "$PFR/OMTChannel.cpp" 'FD_SET(socketFd_'
must_not_grep "$PFR/OMTChannel.cpp" 'noteCloseReason' 
build_and_run rv-prefix "$ROOT/qa/receiver-validation.cpp" "$PFR" fail

# MR1: the negative-length gate removed. This is the -16 spin, in one line.
MR1=$(fresh_copy mutant-no-negcheck)
if apply_mutant rv-mutant-no-negcheck "$MR1/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (header.dataLength < 0) {"
assert s.count(old) == 1, "dataLength < 0 gate not found"
open(p, "w").write(s.replace(old, "            if (false && header.dataLength < 0) {  // MUTANT", 1))
PY
then build_and_run rv-mutant-no-negcheck "$ROOT/qa/receiver-validation.cpp" "$MR1" fail
fi

# MR2: the size ceiling removed — an absurd dataLength is then simply waited on forever, so the
# channel is never closed and never named.
MR2=$(fresh_copy mutant-no-maxcheck)
if apply_mutant rv-mutant-no-maxcheck "$MR2/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (header.dataLength > kMaxControlPayload) {"
assert s.count(old) == 1, "dataLength ceiling not found"
open(p, "w").write(s.replace(old, "            if (false && header.dataLength > kMaxControlPayload) {  // MUTANT", 1))
PY
then build_and_run rv-mutant-no-maxcheck "$ROOT/qa/receiver-validation.cpp" "$MR2" fail
fi

# MR3: the frame-type gate removed. A sender accepts Metadata only; Video/Audio records from a peer
# are malformed, and parsing them as XML is how a mis-framed stream starts.
MR3=$(fresh_copy mutant-no-frametype)
if apply_mutant rv-mutant-no-frametype "$MR3/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (header.frameType != static_cast<uint8_t>(FrameType::Metadata)) {"
assert s.count(old) == 1, "frameType gate not found"
open(p, "w").write(s.replace(old, "            if (false && header.frameType != static_cast<uint8_t>(FrameType::Metadata)) {  // MUTANT", 1))
PY
then build_and_run rv-mutant-no-frametype "$ROOT/qa/receiver-validation.cpp" "$MR3" fail
fi

# MR4: metadataLength no longer has to fit inside dataLength.
MR4=$(fresh_copy mutant-no-metalen)
if apply_mutant rv-mutant-no-metalen "$MR4/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (static_cast<int32_t>(header.metadataLength) > header.dataLength) {"
assert s.count(old) == 1, "metadataLength gate not found"
open(p, "w").write(s.replace(old, "            if (false && static_cast<int32_t>(header.metadataLength) > header.dataLength) {  // MUTANT", 1))
PY
then build_and_run rv-mutant-no-metalen "$ROOT/qa/receiver-validation.cpp" "$MR4" fail
fi

# MR5: the version gate removed (it existed before this branch; the assertion on its NAME did not).
MR5=$(fresh_copy mutant-no-versioncheck)
if apply_mutant rv-mutant-no-versioncheck "$MR5/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (header.version != 1) {"
assert s.count(old) == 1, "version gate not found"
open(p, "w").write(s.replace(old, "            if (false && header.version != 1) {  // MUTANT", 1))
PY
then build_and_run rv-mutant-no-versioncheck "$ROOT/qa/receiver-validation.cpp" "$MR5" fail
fi

# MR6: select()/FD_SET put back over the poll(). Only the high-fd case can see this, which is why
# the case exists: at fd < 1024 select() and poll() behave identically.
MR6=$(fresh_copy mutant-select)
if apply_mutant rv-mutant-select "$MR6/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        struct pollfd pfd{fd, POLLIN, 0};\n        int sel = ::poll(&pfd, 1, 100);"
assert s.count(old) == 1, "poll() call not found"
new = ("        fd_set readfds; FD_ZERO(&readfds); FD_SET(fd, &readfds);\n"
       "        struct timeval tv{0, 100000};\n"
       "        int sel = select(fd + 1, &readfds, nullptr, nullptr, &tv);")
s = s.replace(old, new, 1)
assert "#include <netinet/tcp.h>" in s
open(p, "w").write(s.replace("#include <netinet/tcp.h>", "#include <netinet/tcp.h>\n#include <sys/select.h>", 1))
PY
then build_and_run rv-mutant-select "$ROOT/qa/receiver-validation.cpp" "$MR6" fail
fi

# MR7: the flags stop being re-checked inside the parse loop. The running-flag case parks the
# receiver inside the quality callback, tears the channel down, and hands back exactly one permit —
# so a loop that ignores running_ walks into frame #2 and parks there forever, and ~Channel()'s
# join hangs. Forced, not sampled: the frames alternate High/Low because processMetadata only
# notifies on a CHANGE, and identical frames would have made the mutant green.
MR7=$(fresh_copy mutant-no-running-check)
if apply_mutant rv-mutant-no-running-check "$MR7/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("            if (!running_ || !connected_) break;\n"
       "\n            // Copy the header into a local BEFORE")
assert s.count(old) == 1, "in-loop alive check not found"
new = ("            if (false) break;  // MUTANT: teardown cannot interrupt a batch of frames\n"
       "\n            // Copy the header into a local BEFORE")
open(p, "w").write(s.replace(old, new, 1))
PY
then build_and_run rv-mutant-no-running-check "$ROOT/qa/receiver-validation.cpp" "$MR7" fail
fi

# MR8: the channel still closes for a negative length, but never SAYS why. Proves the named-error
# assertion is load-bearing and not decoration riding on the close that the validation already did.
MR8=$(fresh_copy mutant-no-reason)
if apply_mutant rv-mutant-no-reason "$MR8/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "                noteCloseReason(CloseReason::NegativeDataLength);"
assert s.count(old) == 1, "negative-dataLength reason not found"
open(p, "w").write(s.replace(old, "                ; // MUTANT: closed silently, no name on the failure", 1))
PY
then build_and_run rv-mutant-no-reason "$ROOT/qa/receiver-validation.cpp" "$MR8" fail
fi

echo "== receiver-peer-lifetime: what a read that returned nothing is allowed to conclude =="
build_and_run_channel rp-fixed "$ROOT/qa/receiver-peer-lifetime.cpp" "$ROOT/libomt/cpp" pass

# The fix reverted wholesale: OMTChannel.cpp from the commit this branch sits on. The HEADERS stay
# as the tree has them — the pre-fix .cpp never touches warnedReadTransient_, and reverting the
# header that merely declares it would fail the BUILD rather than the gate (see the sw-prefix note).
#
# Pinned to a051d730 — master, the commit this branch sits on. It already carries the poll()-based
# wait and the CloseReason bookkeeping (both arrived with #23), so what goes red here is the
# EOF/EAGAIN half and nothing else. The branch's original pin, 3d64d14a, was the old stack's #22
# tip: it predates #23, and against today's tree its file is missing the validation the receiver
# gate also asserts, so it would have measured two defects at once. This used to be HEAD~1, which
# is a defect, not a convenience: HEAD~1 is relative to whatever is checked out, and when a branch
# STACKS on this one (fix/omt-receiver-poll-fd-setsize sits on this commit), HEAD~1 on the child is
# THIS tip — already EOF-fixed — and the must-fail control silently extracts a fixed file and
# passes, turning the gate into a no-op (measured: rp-prefix went "pass but expected fail" on the
# stacked branch). A pre-fix baseline is an immutable commit. QA_BASE_RP overrides; it must name a
# commit that is reachable from master.

RPBASE=${QA_BASE_RP:-a051d730}
PFRP="$SCRATCH/pre-fix-peer-lifetime"; rm -rf "$PFRP"; mkdir -p "$PFRP"
cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$PFRP"/
extract_rev_file "$RPBASE" "libomt/cpp/OMTChannel.cpp" "$PFRP/OMTChannel.cpp"
must_not_grep "$PFRP/OMTChannel.cpp" 'IsPeerGoneErrno'
must_grep     "$PFRP/OMTChannel.cpp" 'connection closed by peer'
build_and_run_channel rp-prefix "$ROOT/qa/receiver-peer-lifetime.cpp" "$PFRP" fail

# RP1: read-half EOF back to channel death — the D1 defect in one line. A pure receiver (a peer that
# shutdown(SHUT_WR)s after subscribing: OBS, libomtnet, our own OmpIn) gets the greeting and nothing
# else, because recv() answering 0 was read as "the peer is gone" rather than "the peer has nothing
# left to say".
M_RP1=$(fresh_copy mutant-rp-eof-death)
if apply_mutant rp-mutant-eof-death "$M_RP1/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ('            LOGI("receiverLoop: peer write-half closed (EOF); stopping reads, keep streaming");\n'
       '            break;')
assert s.count(old) == 1, "EOF-as-state branch not found"
new = ('            LOGE("MUTANT: EOF is death");\n'
       '            connected_ = false;\n'
       '            break;')
open(p, "w").write(s.replace(old, new, 1))
PY
then build_and_run_channel rp-mutant-eof-death "$ROOT/qa/receiver-peer-lifetime.cpp" "$M_RP1" fail
fi

# RP2: the transient read back to channel death — the D4 defect. recv() answering EAGAIN after
# select() called the socket readable kills the channel before the SUBSCRIBE registers, so the
# viewer sits at 0 frames behind a socket that is still open.
M_RP2=$(fresh_copy mutant-rp-eagain-death)
if apply_mutant rp-mutant-eagain-death "$M_RP2/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {"
assert s.count(old) == 1, "transient-read branch not found"
open(p, "w").write(s.replace(old, "        if (false) {  // MUTANT: a transient read is fatal again", 1))
PY
then build_and_run_channel rp-mutant-eagain-death "$ROOT/qa/receiver-peer-lifetime.cpp" "$M_RP2" fail
fi

# RP3: the streak guard removed. "Keep the channel alive on a transient read" without a bound is not
# a fix, it is a spin: a socket that is permanently readable and permanently empty pins a core for
# the life of the app and is never reaped. The counter still runs; only the verdict is gone.
M_RP3=$(fresh_copy mutant-rp-no-streak-guard)
if apply_mutant rp-mutant-no-streak-guard "$M_RP3/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (++transientStreak > kMaxConsecutiveTransientReads) {"
assert s.count(old) == 1, "streak guard not found"
open(p, "w").write(s.replace(old, "            ++transientStreak;\n            if (false) {  // MUTANT: the transient path never ends", 1))
PY
then build_and_run_channel rp-mutant-no-streak-guard "$ROOT/qa/receiver-peer-lifetime.cpp" "$M_RP3" fail
fi

# RP4: the warning stops latching, i.e. it prints on every attempt. That is the shape that produced
# 802 identical warnings in three minutes against one slow OBS on the desktop copy; the flag is what
# makes it one line per episode, so a warning that cannot latch is the outage printed out.
M_RP4=$(fresh_copy mutant-rp-no-latch)
if apply_mutant rp-mutant-no-latch "$M_RP4/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (!warnedReadTransient_.exchange(true, std::memory_order_relaxed)) {"
assert s.count(old) == 1, "warn-once latch not found"
open(p, "w").write(s.replace(old, "            if (true) {  // MUTANT: logs per attempt, never latches", 1))
PY
then build_and_run_channel rp-mutant-no-latch "$ROOT/qa/receiver-peer-lifetime.cpp" "$M_RP4" fail
fi

# RP5: the warning latches and never re-arms. The storm is gone, but so is every stall after the
# first one — the second viewer in the room is silent, which is the failure the per-channel flag
# (rather than the old `static bool`) exists to avoid.
M_RP5=$(fresh_copy mutant-rp-no-rearm)
if apply_mutant rp-mutant-no-rearm "$M_RP5/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        warnedReadTransient_.store(false, std::memory_order_relaxed);"
assert s.count(old) == 1, "re-arm on progress not found"
open(p, "w").write(s.replace(old, "        ; // MUTANT: the episode never ends, so nothing new is ever reported", 1))
PY
then build_and_run_channel rp-mutant-no-rearm "$ROOT/qa/receiver-peer-lifetime.cpp" "$M_RP5" fail
fi

echo "== receiver-high-fd: the subscriber's descriptor may not index a stack-resident fd_set =="
build_and_run_channel hp-fixed "$ROOT/qa/receiver-high-fd.cpp" "$ROOT/libomt/cpp" pass

# The fix reverted wholesale: OMTChannel.cpp from a revision that still waits for the peer with
# select()/FD_SET. Headers stay as the tree's (see the sw-prefix note).
#
# Pinned to adf620a3 — the last master revision whose receiverLoop still calls FD_SET. It has to be
# a historical one: master's own file (a051d730, the commit the branch below this one sits on)
# ALREADY waits with poll(), because #23 carried the switch in with it. Pinning the parent commit
# would therefore extract an already-fixed file and hp-prefix would pass — the same no-op gate
# that made HEAD~1 a defect on stacked branches. adf620a3 is the same revision qa/build-host.sh
# pins receiver-validation's pre-fix copy to, so it is known to build against this tree's headers
# (its 4-arg sendWithDeadline matches the declaration), and it is reachable from master.
# This used to be 65f2924b — the old stack's #25 tip, which is no longer reachable from any remote
# ref, so a fresh clone could not resolve it. QA_BASE_HP overrides; it must name a commit whose
# receiverLoop still waits with select(), or hp-prefix measures nothing and must be refused.

HPBASE=${QA_BASE_HP:-adf620a3}
PFHP="$SCRATCH/pre-fix-high-fd"; rm -rf "$PFHP"; mkdir -p "$PFHP"
cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$PFHP"/
extract_rev_file "$HPBASE" "libomt/cpp/OMTChannel.cpp" "$PFHP/OMTChannel.cpp"
must_grep     "$PFHP/OMTChannel.cpp" 'FD_SET(socketFd_, &readfds)'
must_not_grep "$PFHP/OMTChannel.cpp" '::poll(&pfd'
build_and_run_channel hp-prefix "$ROOT/qa/receiver-high-fd.cpp" "$PFHP" fail

# HP1: the 0f95942 switch undone by hand, on the current tree. hp-prefix proves the historical
# state; this proves the same thing about THIS commit's source, with the read-result semantics of
# the commit below it unchanged — so the only difference the child can see is how it waits. Under a
# select() build the child dies on ASan's stack-buffer-overflow inside receiverLoop, and the parent
# scores the case red rather than reading a truncated log as a pass.
M_HP1=$(fresh_copy mutant-hp-select)
if apply_mutant hp-mutant-select "$M_HP1/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace("#include <netinet/tcp.h>\n",
              "#include <netinet/tcp.h>\n#include <sys/select.h>  // MUTANT: select() is back\n", 1)
old = "        struct pollfd pfd{fd, POLLIN, 0};\n        int sel = ::poll(&pfd, 1, 100);"
assert s.count(old) == 1, "the poll() wait is not where this tree put it"
new = """        fd_set readfds; FD_ZERO(&readfds); FD_SET(fd, &readfds);   // MUTANT: FD_SETSIZE is 1024
        struct timeval tv{0, 100000};
        int sel = select(fd + 1, &readfds, nullptr, nullptr, &tv);"""
open(p, "w").write(s.replace(old, new, 1))
PY
then build_and_run_channel hp-mutant-select "$ROOT/qa/receiver-high-fd.cpp" "$M_HP1" fail
fi

# ---------------------------------------------------------------------------
# vmx-thread-cap (tc-*): the codec may only fan its slice tasks out to cpus this process can run on.
#
# THE FAILURE. VMX_EncodePlanes (libvmx/src/vmxcodec.cpp:~1700) pushes one slice range per thread and
# then Joins EVERY one of them, so a frame is not done until its slowest slice is. The thread count
# comes from VMX_BITRATE_TABLE and is then overridden to 8 or 16 for 2160p/4320p whenever
# std::thread::hardware_concurrency() reports that many cpus — a number that counts every ONLINE cpu,
# not the ones this process may run on. On an 8-core big.LITTLE phone (or inside a 4-cpu cpuset, which
# is what a host can actually reproduce today) that is two rounds of scheduling per frame with Join
# holding the whole frame open while the last slice waits for a core somebody else is using.
#
# WHAT IS PROVEN HERE, and what is not: the cap, the >=1 clamp that keeps `totalSlices / numThreads`
# from being a SIGFPE, and — the case that says the cap is a scheduling decision and nothing more —
# that the same 1080p frame encodes to the SAME BYTES at 1, 2, 4, 8 and 16 worker threads. The
# phone's big.LITTLE case is UNPROVEN on this bench because the phone is not on this bench; the
# commit that lands it says so.
#
# The controls run under `taskset -c 0-3` deliberately: hardware_concurrency() answers 32 there too
# (measured — it reads the online count, not the affinity mask), so an unconstrained run cannot tell
# the capped codec from the uncapped one. Running the must-fail control on the whole machine would
# have produced a green "anti-vacuity" control, which is the worst thing a suite can do.
# ---------------------------------------------------------------------------

# The pre-cap codec, pinned to the immutable commit this branch sits on — VMX_Create AND
# thread_tasks.h from the same rev, because the build tag in that header moved with this change and
# a copy mixed with the tree's header is not the tree this control claims to run.
TC_PREFIX_REV=${TC_PREFIX_REV:-9c0820668b39353b999850b2884dca2d05083b40}

# compile_tc <name> <vmx-src-dir> — build the scenario over a chosen copy of the codec, do not score
# it. The constrained scenarios below run the SAME binary behind a CPU mask and score it under their
# own name, so building and scoring have to be separable; a scenario that was never compiled is not
# a red scenario, it is an absent one, and score_run_under says so as a harness error.
compile_tc() {
  local name=$1 vmxdir=$2
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS -I"$vmxdir" $INC "$ROOT/qa/vmx-thread-cap.cpp" \
       "$vmxdir/vmxcodec.cpp" "$vmxdir/vmxcodec_x86.cpp" "$vmxdir/vmxcodec_avx2.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -15
    FAILED+=("$name(build-failed)"); rc=1; return 1
  fi
  return 0
}

# build_tc <name> <expect pass|fail> <vmx-src-dir> — compile, then score on the whole machine.
build_tc() {
  local name=$1 expect=$2 vmxdir=$3
  compile_tc "$name" "$vmxdir" || return
  score_run "$name" "$expect"
}

# score_tc <name> <expect> — score_run, but only for a binary that EXISTS.
#
# WHY THIS EXISTS: tc-mutant-threads-999 came back "fail as expected" from a run that never happened.
# `timeout` had nothing to execute and printed "timeout: failed to run command ...: No such file or
# directory" into the run log; score_run's absent-run guard asks for EMPTY output, and this output was
# not empty, so a control nobody compiled was scored as a mutant the gate caught. That is the exact
# inversion this suite is built to prevent — an absent control looks identical to a passing one in the
# summary line, and here it even looked like the right colour. A scenario binary that is not there is a
# harness error, in this section, always; the check lives here rather than in master's score_run
# because the harness outside this section has to stay byte-for-byte what it was.
score_tc() {
  local name=$1 expect=$2
  [ -x "$SCRATCH/$name" ] \
    || harness_die "score_tc $name: no such scenario binary $SCRATCH/$name — it was never compiled, and scoring its absence would report an absent control as a caught one"
  score_run "$name" "$expect"
}

# score_run_under <name> <expect> <binary-name> <command...> — the same verdict, run behind a
# command prefix, and scored under its OWN scenario name. The constrained run below is the same
# program on a smaller CPU budget: it is a different scenario, so it gets a different name and a
# different run.log, and it must not be scored twice under the unrestricted one's name. taskset is
# not optional here: a missing one is a harness error, never a silently skipped control (a control
# nobody ran is not evidence, and it looks exactly like a control that passed).
score_run_under() {
  local name=$1 expect=$2 binary=$3; shift 3
  [ -x "$SCRATCH/$binary" ] \
    || harness_die "score_run_under $name: no such scenario binary $SCRATCH/$binary — the constrained run cannot be expressed"
  command -v "$1" >/dev/null 2>&1 \
    || harness_die "score_run_under $name: '$1' is not available — the constrained run cannot be expressed, and skipping it would score an absent control"
  timeout 180 "$@" "$SCRATCH/$binary" >"$SCRATCH/$name.run.log" 2>&1
  local status=$?
  local got=fail
  [ "$status" -eq 0 ] && got=pass
  if [ "$got" = "$expect" ]; then
    echo "  $name: $got as expected"
  else
    echo "  $name: $got but expected $expect  <-- PROBLEM"
    sed 's/^/      /' "$SCRATCH/$name.run.log" | tail -18
    FAILED+=("$name(verdict-$got)"); rc=1
  fi
}

echo "== vmx-thread-cap: the codec may only fan out to cpus this process can run on =="
build_tc tc-fixed pass "$ROOT/libvmx/src"
# The same program on a 4-CPU budget, scored as its own scenario. This is the run the cap is FOR:
# 2160p must come up with 4 slice threads, not the 8 the override asks for, and 4320p with 4, not 16.
# It deliberately has no binary of its own — it IS tc-fixed, behind a mask — so qa/repeat-run.sh
# measures its determinism as `repeat-run.sh $SCRATCH tc-fixed pass 5`: the binary is the same one,
# and a CPU mask that does not vary between runs adds no nondeterminism of its own to re-measure.
# (Copying the binary under this scenario's name would have made repeat-run able to target it, and
# it would have silently re-run it UNCONSTRAINED — a scenario that changes meaning depending on who
# launched it is worse than one repeat-run cannot reach.)
score_run_under tc-fixed-constrained pass tc-fixed taskset -c 0-3

PFTC="$SCRATCH/vmx-prefix-threadcap"; rm -rf "$PFTC"; mkdir -p "$PFTC"
cp "$ROOT"/libvmx/src/*.cpp "$ROOT"/libvmx/src/*.h "$PFTC"/
extract_rev_file "$TC_PREFIX_REV" "libvmx/src/vmxcodec.cpp"   "$PFTC/vmxcodec.cpp"   tc-prefix
extract_rev_file "$TC_PREFIX_REV" "libvmx/src/thread_tasks.h" "$PFTC/thread_tasks.h" tc-prefix
must_grep     "$PFTC/vmxcodec.cpp"   'instance->Tasks = CreateTasks(instance->Threads);' tc-prefix
must_not_grep "$PFTC/vmxcodec.cpp"   'VMX_AvailableCpuCount'                            tc-prefix
must_grep     "$PFTC/thread_tasks.h" 'vmx-thread-task-predicate-wait-p0-11'             tc-prefix
compile_tc tc-prefix "$PFTC" && score_run_under tc-prefix fail tc-prefix taskset -c 0-3

# TC1: the cap removed. On an unrestricted box this copy is indistinguishable from the fixed one —
# which is why the control runs it under the 4-CPU mask, where the difference is 8 threads vs 4.
TC1=$(fresh_copy_vmx mutant-no-thread-cap)
if apply_mutant tc-mutant-no-thread-cap "$TC1/vmxcodec.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("\t{\n"
       "\t\tint runnable = VMX_AvailableCpuCount();\n"
       "\t\tif (runnable < 1) runnable = 1;\n"
       "\t\tif (instance->Threads > runnable) instance->Threads = runnable;\n"
       "\t\tif (instance->Threads < 1) instance->Threads = 1;\n"
       "\t}\n")
assert s.count(old) == 1, "the worker-thread cap is not where this tree put it"
open(p, "w").write(s.replace(old, "\t// MUTANT: the table's number is the number, as it used to be\n", 1))
PY
then compile_tc tc-mutant-no-thread-cap "$TC1" && \
     score_run_under tc-mutant-no-thread-cap fail tc-mutant-no-thread-cap taskset -c 0-3
fi

# TC2: the sabotage the acceptance bar asks for — force an absurd worker count. It must still BUILD
# (nothing here is a compile-time assertion) and it must be caught by the gate at full machine
# width, with no taskset involved: 999 slice threads is > runnable on any box that can run the test.
TC2=$(fresh_copy_vmx mutant-threads-999)
if apply_mutant tc-mutant-threads-999 "$TC2/vmxcodec.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("\t{\n"
       "\t\tint runnable = VMX_AvailableCpuCount();\n"
       "\t\tif (runnable < 1) runnable = 1;\n"
       "\t\tif (instance->Threads > runnable) instance->Threads = runnable;\n"
       "\t\tif (instance->Threads < 1) instance->Threads = 1;\n"
       "\t}\n")
assert s.count(old) == 1, "the worker-thread cap is not where this tree put it"
open(p, "w").write(s.replace(old, "\tinstance->Threads = 999;   // MUTANT: sabotage the cap\n", 1))
PY
then compile_tc tc-mutant-threads-999 "$TC2" && score_tc tc-mutant-threads-999 fail
fi

# ---------------------------------------------------------------------------
# encode-pipeline (et-*): the ImageReader callback may not wait for the codec.
#
# THE FAILURE. OMTStreamingManager's onImageAvailable ran pack + encode + headers inline, and
# ImageReader is created with maxImages=3 — the camera cannot recycle a buffer until the callback
# returns, so the delivered frame rate IS 1/(the callback's cost), whatever the camera or the
# declared rate asked for. Measured on one rig, both ways: the native x86_64 emulator build gates
# green at 30.4/30.2 fps, the same build under arm64 translation delivers 11-13. Same camera, same
# declaration; only the callback's cost moved. That is the "~14 fps delivered vs 30 declared"
# report, and no scenario in this file could see it, because none of them ran a callback.
#
# WHAT CHANGED: the callback now vets the caller's plane claims and COPIES the plane bytes — the two
# things that must happen before the Image is closed — into a depth-3 drop-oldest ring, and a
# dedicated encoder thread runs packNv12 + omt_send (app/src/main/cpp/OMTBridge.cpp). qa/encode-pipeline
# .cpp drives that REAL translation unit through qa/host-jni/jni.h with the codec replaced at LINK
# time by qa/stub_vmx.cpp sleeping 70 ms per encode: the seam that makes "did the producer wait?" a
# measurable question on a host with no camera and no phone. The #31 admission checks moved with the
# read they guard — they belong to the copy now — and qa/jni-boundary.cpp still drives the same TU,
# so jb-mutant-no-admission still dies under ASan rather than being retired by the refactor.
#
# The invariants the mutants below are each one line wide: the callback returns while an encode is
# still running (E1); delivered frames are FIFO, byte-exact against the packer's own oracle, and
# each in ITS OWN destination buffer (E2/E2b); the excess is evicted AND COUNTED, newest frame always
# wins a slot (E3..E3d); teardown joins the encoder thread in bounded time and drains what it already
# took (E4); a lie about a plane is refused before the copy reads it (E5); and the copy preserves the
# address relationship planChroma() decides the chroma layout from (E6e) — the one refactor-shaped
# defect in this change that would not have changed a single output byte.
# ---------------------------------------------------------------------------

# The pre-decoupling capture path, pinned to the immutable commit this branch sits on — NOT HEAD~1,
# which changes meaning every time the branch grows a commit. OMTBridge.cpp AND omtnv12.h come from
# that one rev: a .cpp pinned to yesterday compiled against the tree's header today is not a program
# (see the header-mixing lesson in the aligned-stride and channel sections above).
ET_PREFIX_REV=${ET_PREFIX_REV:-9c0820668b39353b999850b2884dca2d05083b40}

# build_et <name> <expect pass|fail> <OMTBridge.cpp to compile> [extra flags]
# The bridge is the production TU; the codec is the link-time stub, which is what lets the scenario
# decide how long an encode takes. The real sender sits between them, so the frames under test
# travel the whole path the phone uses: bridge -> packNv12 -> Sender::send -> encodeFrame -> seam.
build_et() {
  local name=$1 expect=$2 bridge=$3; shift 3
  local extra="$*"
  [ -f "$bridge" ] || harness_die "build_et $name: no such bridge source: $bridge"
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS $extra $INC -I"$(dirname "$bridge")" -I"$ROOT/qa/host-jni" -I"$ROOT/app/src/main/cpp" \
       "$ROOT/qa/encode-pipeline.cpp" "$bridge" \
       "$ROOT/libomt/cpp/OMTChannel.cpp" "$ROOT/libomt/cpp/OMTAsyncPool.cpp" \
       "$ROOT/libomt/cpp/OMTSender.cpp" "$ROOT/libomt/cpp/OMTDiscovery.cpp" \
       "$ROOT/qa/stub_vmx.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -15
    FAILED+=("$name(build-failed)"); rc=1; return
  fi
  score_run "$name" "$expect"
}

echo "== encode-pipeline: the ImageReader callback must not wait for the codec =="
build_et et-fixed pass "$ROOT/app/src/main/cpp/OMTBridge.cpp"

# THE ANTI-VACUITY CONTROL. Same scenario, same assertions, the OMTBridge.cpp and the omtnv12.h it
# was written against — both from the SAME immutable rev, because a .cpp pinned to yesterday with
# its header from today is not a program (the header-mixing lesson recorded above). It must be red,
# and it must be red on the line that says the callback blocked in pack+encode+send.
PF_ET="$SCRATCH/et-prefix-bridge"; rm -rf "$PF_ET"; mkdir -p "$PF_ET"
extract_rev_file "$ET_PREFIX_REV" "app/src/main/cpp/OMTBridge.cpp" "$PF_ET/OMTBridge.cpp" et-prefix
extract_rev_file "$ET_PREFIX_REV" "app/src/main/cpp/omtnv12.h"     "$PF_ET/omtnv12.h"     et-prefix
must_grep     "$PF_ET/OMTBridge.cpp" 'return postFrame(env, dst, packed.stride' et-prefix
must_grep     "$PF_ET/OMTBridge.cpp" 'admitSource(claims'                       et-prefix
must_not_grep "$PF_ET/OMTBridge.cpp" 'nativeQueueFramePlanes'                   et-prefix
must_not_grep "$PF_ET/OMTBridge.cpp" 'OMT_RING_DEPTH'                           et-prefix
must_not_grep "$PF_ET/OMTBridge.cpp" 'g_ringDropped'                            et-prefix
build_et et-prefix fail "$PF_ET/OMTBridge.cpp"

# ET1: no eviction at all — the ring is full, so refuse the newest frame. The counts stop balancing
# and the drop counter never moves: exactly the "a healthy stream that is simply idle" lie the
# sender's own counters were written against, one level up.
ET1=$(fresh_copy_jni mutant-et-no-eviction)
mutate_jni "$ET1" OMTBridge.cpp \
  '        if (g_ringOrderLen == 0) {
            LOGE("Frame ring has no empty slot and nothing queued — refusing the frame");
            return JNI_FALSE;
        }
        idx = (int)g_ringOrder[0];
        evictedSeq = g_ring[idx].seq;
        for (size_t i = 1; i < g_ringOrderLen; ++i) g_ringOrder[i - 1] = g_ringOrder[i];
        --g_ringOrderLen;
        evicted = true;' \
  '        return JNI_FALSE;   // MUTANT: keep the queue as it is and refuse the new frame' \
  "never evict the oldest frame"
build_et et-mutant-no-eviction fail "$ET1/OMTBridge.cpp"

# ET2: claim from the BACK of the queue. The newest frame jumps in front of the ones already
# waiting, which is a reordering — the counts still balance, so only the per-frame payload oracle
# sees it.
ET2=$(fresh_copy_jni mutant-et-reorder)
mutate_jni "$ET2" OMTBridge.cpp \
  '                idx = (int)g_ringOrder[0];
                for (size_t i = 1; i < g_ringOrderLen; ++i) g_ringOrder[i - 1] = g_ringOrder[i];
                --g_ringOrderLen;
                g_ringState[idx] = SlotBusy;' \
  '                idx = (int)g_ringOrder[g_ringOrderLen - 1];   // MUTANT: LIFO — the newest frame goes first
                --g_ringOrderLen;
                g_ringState[idx] = SlotBusy;' \
  "claim frames out of order"
build_et et-mutant-reorder fail "$ET2/OMTBridge.cpp"

# ET3: the eviction counted twice. A counter that lies is worse than no counter, because the UI and
# this suite both read it as the difference between "the encoder is keeping up" and "it is not".
ET3=$(fresh_copy_jni mutant-et-counter-double-count)
mutate_jni "$ET3" OMTBridge.cpp \
  '        const unsigned long long total = g_ringDropped.fetch_add(1) + 1;' \
  '        const unsigned long long total = g_ringDropped.fetch_add(2) + 2;  // MUTANT: two per eviction' \
  "double-count the drop-oldest counter"
build_et et-mutant-counter-double-count fail "$ET3/OMTBridge.cpp"

# ET4: the teardown deadlock, in the shape it would actually take — join the encoder thread while
# holding the mutex the encoder needs to post. The scenario hangs inside nativeCleanup and the
# harness scores the hang red, exactly as the #24 stop-deadlock gate does.
ET4=$(fresh_copy_jni mutant-et-join-under-sender-lock)
mutate_jni "$ET4" OMTBridge.cpp \
  'void stopEncoderThread() {
    std::thread victim;' \
  'void stopEncoderThread() {
    std::lock_guard<std::mutex> senderLock(g_senderMutex);   // MUTANT: the join is taken under the mutex the encoder needs to post
    std::thread victim;' \
  "join the encoder thread under g_senderMutex"
build_et et-mutant-join-under-sender-lock fail "$ET4/OMTBridge.cpp"

# ET5: one destination buffer for every frame — the shared frameBuffer the ring replaced. The bytes
# still come out right (the encoder is single-threaded), which is precisely why the assertion is on
# the DESTINATIONS being distinct and not only on the payloads being right.
ET5=$(fresh_copy_jni mutant-et-shared-dst)
mutate_jni "$ET5" OMTBridge.cpp \
  '    resizeBytes(slot.packed, (size_t)need);
    omtpack::Result packed{};
    char why[256] = {};
    if (!packCopy(slot.copy, slot.packed.data(), (int64_t)slot.packed.size(), packed,' \
  '    static std::vector<uint8_t> sharedPacked;   // MUTANT: one frameBuffer reused across frames, as it was
    resizeBytes(sharedPacked, (size_t)need);
    omtpack::Result packed{};
    char why[256] = {};
    if (!packCopy(slot.copy, sharedPacked.data(), (int64_t)sharedPacked.size(), packed,' \
  "pack every frame into one shared buffer"
build_et et-mutant-shared-dst fail "$ET5/OMTBridge.cpp"

# ET6: drop-NEWEST, the policy qa/slow-subscriber.cpp documents the channel queue replacing. Every
# count still balances and the order is still FIFO; what changes is that the viewer keeps the OLDEST
# frames. E3d is the assertion that exists for this mutant.
ET6=$(fresh_copy_jni mutant-et-drop-newest)
mutate_jni "$ET6" OMTBridge.cpp \
  '        idx = (int)g_ringOrder[0];
        evictedSeq = g_ring[idx].seq;
        for (size_t i = 1; i < g_ringOrderLen; ++i) g_ringOrder[i - 1] = g_ringOrder[i];
        --g_ringOrderLen;
        evicted = true;' \
  '        idx = (int)g_ringOrder[g_ringOrderLen - 1];   // MUTANT: drop-newest, the policy the channel queue replaced
        evictedSeq = g_ring[idx].seq;
        --g_ringOrderLen;
        evicted = true;' \
  "evict the newest frame instead of the oldest"
build_et et-mutant-drop-newest fail "$ET6/OMTBridge.cpp"

# ET7: the chroma pair copied into two separate allocations. planChroma() decides CB_CR from CR_CB
# from "malformed" off the ADDRESSES of the two windows, so flattening them does not change a single
# output byte — it deletes a refusal. E6e is that refusal, seen from the capture path.
ET7=$(fresh_copy_jni mutant-et-chroma-offset)
mutate_jni "$ET7" OMTBridge.cpp \
  '    if (sharesBytes) {
        const uintptr_t lo = std::min(ua, va);' \
  '    if (false && sharesBytes) {   // MUTANT: every pair becomes two allocations, whatever the HAL reported
        const uintptr_t lo = std::min(ua, va);' \
  "copy the chroma pair apart and lose the address relationship"
build_et et-mutant-chroma-offset fail "$ET7/OMTBridge.cpp"


echo
echo "== coded-send: an already-encoded frame passes through, tagged with its own codec =="
build_and_run cs-hx "$ROOT/qa/coded-send.cpp" "$ROOT/libomt/cpp" pass

HX1=$(fresh_copy mutant-hx-codec)
if apply_mutant hx-mutant-codec "$HX1/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    ext.codec = codec;"
assert s.count(old) == 1, "codec assignment in buildVideoHeaders not found"
open(p, "w").write(s.replace(old, "    ext.codec = Codec::VMX1;  // MUTANT: tag every payload as VMX", 1))
PY
then build_and_run hx-mutant-codec "$ROOT/qa/coded-send.cpp" "$HX1" fail
fi

HX2=$(fresh_copy mutant-hx-geometry)
if apply_mutant hx-mutant-geometry "$HX2/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """            producer.width = frame.width;
            producer.height = frame.height;"""
assert s.count(old) == 1, "coded producer geometry not found"
open(p, "w").write(s.replace(old, """            producer.width = 1920;   // MUTANT: geometry captured once, not per frame
            producer.height = 1080;""", 1))
PY
then build_and_run hx-mutant-geometry "$ROOT/qa/coded-send.cpp" "$HX2" fail
fi


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
