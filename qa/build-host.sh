#!/usr/bin/env bash
# Host-side QA for the vendored OMT C++ — no device, no JVM, no emulator.
#
# Every test here is run twice: against the current tree (must pass) and against a copy with the
# safety gate deliberately disabled (must fail). A test that is green in both cases is not
# evidence of anything — it just means nobody wrote the assertion.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SCRATCH=${SCRATCH:-/tmp/omt-qa}
mkdir -p "$SCRATCH"
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
    rc=1; return
  fi
  timeout 90 "$SCRATCH/$name" >"$SCRATCH/$name.run.log" 2>&1
  # The status must be captured in the SAME statement that reads $?: two earlier versions of this
  # line (`local got=pass; [ $? -ne 0 ]` and `local got; if [ $? ...`) each scored every run as a
  # pass, because the intervening `local` is itself a command and resets $? to 0. A harness that
  # cannot go red is not evidence.
  local status=$?
  local got=fail
  [ "$status" -eq 0 ] && got=pass
  if [ "$got" = "$expect" ]; then
    echo "  $name: $got as expected"
  else
    echo "  $name: $got but expected $expect  <-- PROBLEM"
    sed 's/^/      /' "$SCRATCH/$name.run.log" | tail -18
    rc=1
  fi
}

fresh_copy() { # <dir> — an isolated tree a mutant may deface
  local d="$SCRATCH/$1"; rm -rf "$d"; mkdir -p "$d"
  cp "$ROOT"/libomt/cpp/*.cpp "$ROOT"/libomt/cpp/*.h "$d"/
  echo "$d"
}

echo "== slow-subscriber: a stalled Wi-Fi viewer must not take the stream down =="
build_and_run ss-fixed "$ROOT/qa/slow-subscriber.cpp" "$ROOT/libomt/cpp" pass
OLD="$SCRATCH/pre-fix"; rm -rf "$OLD"; mkdir -p "$OLD"
for f in OMTChannel.cpp OMTAsyncPool.cpp OMTSender.cpp OMTDiscovery.cpp \
         OMTChannel.h OMTSender.h OMTDiscovery.h OMTConstants.h OMTProtocol.h OMTAsyncPool.h; do
  git show "master:libomt/cpp/$f" > "$OLD/$f" 2>/dev/null || { echo "  cannot extract $f from master"; rc=1; }
done
build_and_run ss-prefix "$ROOT/qa/slow-subscriber.cpp" "$OLD" fail

echo
echo "== colorspace-gate: refuse what the encoder cannot honour =="
build_and_run cs-fixed "$ROOT/qa/colorspace-gate.cpp" "$ROOT/libomt/cpp" pass

M1=$(fresh_copy mutant-gate)
python3 - "$M1/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (frame.colorSpace != ColorSpace::Undefined &&"
assert s.count(old) == 1, "gate not found to disable"
open(p, "w").write(s.replace(old, "    if (false && frame.colorSpace != ColorSpace::Undefined &&", 1))
PY
build_and_run cs-mutant-gate "$ROOT/qa/colorspace-gate.cpp" "$M1" fail

M2=$(fresh_copy mutant-count)
python3 - "$M2/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        encodeFailures_++;"
assert s.count(old) == 1, "counter not found to disable"
open(p, "w").write(s.replace(old, "        ; // MUTANT: the failure goes uncounted", 1))
PY
build_and_run cs-mutant-count "$ROOT/qa/colorspace-gate.cpp" "$M2" fail

echo
echo "== frame-validation: refuse a frame the buffer cannot honour, before the codec reads it =="
build_and_run fv-fixed "$ROOT/qa/frame-validation.cpp" "$ROOT/libomt/cpp" pass

# M5: the gate removed wholesale. B1 must die with an ASan heap-buffer-overflow READ in
# VMX_NV12ToPlanar — that report, not the assertion, is the proof the crash was real.
M5=$(fresh_copy mutant-validate-call)
python3 - "$M5/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (!validateFrame(frame, why)) {"
assert s.count(old) == 1, "validateFrame call not found to disable"
open(p, "w").write(s.replace(old, "    if (false && !validateFrame(frame, why)) {", 1))
PY
build_and_run fv-mutant-validate-call "$ROOT/qa/frame-validation.cpp" "$M5" fail

# M6: the gate present, but charging NV12 only for the luma plane — the exact arithmetic error that
# let the chroma rows walk off the ImageReader allocation. Same ASan death, narrower cause.
M6=$(fresh_copy mutant-nv12-need)
python3 - "$M6/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        need = h * s + (h / 2ull) * s;"
assert s.count(old) == 1, "NV12 need clause not found"
open(p, "w").write(s.replace(old, "        need = h * s;  // MUTANT: the UV plane is not charged for", 1))
PY
build_and_run fv-mutant-nv12-need "$ROOT/qa/frame-validation.cpp" "$M6" fail

# M7: P0 #16 removed — a zero/negative frame-rate numerator would be stamped into the wire header.
M7=$(fresh_copy mutant-frameraten)
python3 - "$M7/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (frame.frameRateN <= 0)"
assert s.count(old) == 1, "frameRateN clause not found"
open(p, "w").write(s.replace(old, "    if (false && frame.frameRateN <= 0)", 1))
PY
build_and_run fv-mutant-frameraten "$ROOT/qa/frame-validation.cpp" "$M7" fail

# M8: the encode-buffer sizing put back the way it was: `int maxSize = width * height * 4`, which
# wraps, plus the clamp that quietly turns the wrapped negative into the 1 MB floor.
M8=$(fresh_copy mutant-narrow-cap)
python3 - "$M8/OMTSender.cpp" <<'PY'
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
build_and_run fv-mutant-narrow-cap "$ROOT/qa/frame-validation.cpp" "$M8" fail

echo
echo "== encoder-create-failure: what the sender says when the codec says no =="
# Links qa/stub_vmx.cpp rather than the codec: VMX_Create cannot be driven to NULL through the real
# one any more now that validateFrame() enforces the same dimension limits the codec checks.
build_and_run ec-fixed "$ROOT/qa/encoder-create-failure.cpp" "$ROOT/libomt/cpp" pass STUB

M9=$(fresh_copy mutant-create-null)
python3 - "$M9/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    if (!vmxInstance_) {\n        why = \"VMX_Create refused \""
assert s.count(old) == 1, "createEncoder NULL check not found"
open(p, "w").write(s.replace("    if (!vmxInstance_) {", "    if (false) {  // MUTANT: NULL is cached like a success", 1))
PY
build_and_run ec-mutant-create-null "$ROOT/qa/encoder-create-failure.cpp" "$M9" fail STUB

echo
echo "== drop-policy: when the ring is full, the newest frame must be the one that survives =="
build_and_run dp-fixed "$ROOT/qa/drop-policy.cpp" "$ROOT/libomt/cpp" pass

M3=$(fresh_copy mutant-keep-oldest)
python3 - "$M3/OMTChannel.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if (!pendingQueue_.empty()) { stale = pendingQueue_.front(); pendingQueue_.pop(); }"
assert s.count(old) == 1, "evict-oldest not found"
# the original policy: keep what is queued, drop the frame in hand
open(p, "w").write(s.replace(old, "            if (false) { stale = pendingQueue_.front(); pendingQueue_.pop(); }", 1))
PY
build_and_run dp-mutant-keep-oldest "$ROOT/qa/drop-policy.cpp" "$M3" fail

M4=$(fresh_copy mutant-blind-drops)
python3 - "$M4/OMTSender.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    const int64_t delta = total - drops_baseline_.exchange(total);"
assert s.count(old) == 1, "drop window not found"
# the state before this commit: the C entry point returned a hardcoded 0
open(p, "w").write(s.replace(old, "    const int64_t delta = 0; (void)total;", 1))
PY
build_and_run dp-mutant-blind "$ROOT/qa/drop-policy.cpp" "$M4" fail

echo
if [ $rc -ne 0 ]; then echo "QA FAILED"; exit 1; fi
echo "QA PASSED (every gate has a scenario where disabling it turns the suite red)"
