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
VMX="$ROOT/libvmx/src/vmxcodec.cpp $ROOT/libvmx/src/vmxcodec_x86.cpp $ROOT/libvmx/src/vmxcodec_avx2.cpp"

rc=0

# build_and_run <binary-name> <test.cpp> <omt-source-dir> <expect pass|fail> [post-compile sed on a copy]
build_and_run() {
  local name=$1 test=$2 srcdir=$3 expect=$4
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS $INC "$test" "$srcdir/OMTChannel.cpp" "$srcdir/OMTAsyncPool.cpp" \
       "$srcdir/OMTSender.cpp" "$srcdir/OMTDiscovery.cpp" $VMX \
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
if [ $rc -ne 0 ]; then echo "QA FAILED"; exit 1; fi
echo "QA PASSED (every gate has a scenario where disabling it turns the suite red)"
