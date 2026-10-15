#!/usr/bin/env bash
# A/B: build the host QA scenarios with the SAME x86 ISA flags the Android x86_64 APK uses.
#
# Why this exists. qa/build-host.sh compiles the host copy of the codec with -march=haswell, so the
# desktop suite has only ever run the codec with LZCNT/TZCNT/BMI present. The Android x86_64 build
# deliberately does NOT: its floor is -msse4.1 (see app/src/main/cpp/CMakeLists.txt), and
# libvmx/src/vmxcodec_x86.h supplies portable spellings of __lzcnt64/_tzcnt_u64 when the target does
# not advertise them. Those fallbacks are therefore compiled by a build the desktop suite never
# reproduces — which means "the host QA is green" says nothing about them, and an error in the
# `? : 64` guard would ship to the emulator unnoticed.
#
# This script closes that gap: it rebuilds the fixed (must-pass) scenarios with the shipping flag
# set — -msse4.1 everywhere, -mavx2 -mlzcnt -mbmi confined to vmxcodec_avx2.cpp exactly as CMake
# does — and runs them. Nothing here relaxes an assertion: the tests, their expectations and their
# sanitizers are build-host.sh's, and the ONLY variable is the ISA flags.
#
# If this goes red while build-host.sh is green, the portable bit-ops are wrong; fix
# vmxcodec_x86.h, not the test.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SCRATCH=${SCRATCH:-/tmp/omt-qa-x86baseline}
mkdir -p "$SCRATCH"

if [ -z "${CXX:-}" ]; then
  for c in "${ANDROID_HOME:-$HOME/android-sdk}"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ \
           "$(command -v clang++ 2>/dev/null)"; do
    [ -x "$c" ] && CXX="$c" && break
  done
fi
CXX=${CXX:-g++}
DECLEXT="-fms-extensions"
case "$($CXX --version 2>/dev/null | head -1)" in *clang*) DECLEXT="-fms-extensions -fdeclspec";; esac
if [ -z "${ASAN_SYMBOLIZER_PATH:-}" ]; then
  for s in "${ANDROID_HOME:-$HOME/android-sdk}"/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-symbolizer; do
    [ -x "$s" ] && ASAN_SYMBOLIZER_PATH="$s" && break
  done
  export ASAN_SYMBOLIZER_PATH
fi

# The two flag sets, copied from app/src/main/cpp/CMakeLists.txt's x86_64 branch. -fsanitize=address
# and the rest of build-host.sh's FLAGS are kept identical so the only difference is the ISA.
BASE="-std=c++17 -O1 -g -fsanitize=address -pthread -include string -include cstring $DECLEXT"
X86_BASELINE="-msse4.1"                 # vmxcodec.cpp, vmxcodec_x86.cpp, all of libomt, the test
AVX2_ISA="-mavx2 -mlzcnt -mbmi"         # vmxcodec_avx2.cpp only, behind VMX_Create's avx2&&bmi2 gate
INC="-I$ROOT/libomt/cpp -I$ROOT/libvmx/src -I$ROOT/libomt"

rc=0
run_case() { # <name> <test.cpp>
  local name=$1 test=$2
  rm -f "$SCRATCH/$name" "$SCRATCH"/avx2-*.o
  # shellcheck disable=SC2086
  $CXX $BASE $AVX2_ISA $INC -c "$ROOT/libvmx/src/vmxcodec_avx2.cpp" \
       -o "$SCRATCH/avx2.o" 2>"$SCRATCH/$name.avx2.log" || { echo "  $name: avx2 TU BUILD FAILED"; sed 's/^/      /' "$SCRATCH/$name.avx2.log" | head -8; rc=1; return; }
  # shellcheck disable=SC2086
  $CXX $BASE $X86_BASELINE $INC "$test" \
       "$ROOT/libomt/cpp/OMTChannel.cpp" "$ROOT/libomt/cpp/OMTAsyncPool.cpp" \
       "$ROOT/libomt/cpp/OMTSender.cpp" "$ROOT/libomt/cpp/OMTDiscovery.cpp" \
       "$ROOT/libvmx/src/vmxcodec.cpp" "$ROOT/libvmx/src/vmxcodec_x86.cpp" \
       "$SCRATCH/avx2.o" -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  # $? in the statement that reads it — see the note in build-host.sh's score_run.
  local status=$?
  if [ $status -ne 0 ] || [ ! -x "$SCRATCH/$name" ]; then
    echo "  $name: BUILD FAILED (refusing to score an absent run)"
    sed 's/^/      /' "$SCRATCH/$name.build.log" | head -12
    rc=1; return
  fi
  timeout 90 "$SCRATCH/$name" >"$SCRATCH/$name.run.log" 2>&1
  local got=$?
  # The whole point: the same scenario that build-host.sh scores pass must still pass when the
  # codec is built at the Android x86_64 ISA floor. Anything other than exit 0 is a real failure.
  if [ $got -eq 0 ]; then
    echo "  $name: pass as expected (SSE4.1 baseline + gated AVX2 TU)"
  else
    echo "  $name: fail but expected pass  <-- PORTABLE LZCNT/TZCNT FALLBACK IS WRONG"
    sed 's/^/      /' "$SCRATCH/$name.run.log" | tail -18
    rc=1
  fi
}

echo "== x86 ISA A/B: the fixed scenarios, rebuilt at the Android x86_64 flag set =="

# Part 0 — the fallbacks themselves, against the hardware instructions they replace.
#
# Parts 1..N below can only prove the codec still works; they cannot prove the portable spellings
# are *equal* to LZCNT/TZCNT, because on this host VMX_Create's `avx2 && bmi2` gate is satisfied and
# the decoder takes the 256-bit path, leaving the 128-bit bit-reader — the only place the fallbacks
# are compiled in — largely unreached. So compare them directly: same translation unit, built twice,
# once at the shipping floor (fallbacks active) and once at -march=haswell (hardware instructions
# active), asserting the two agree on every input including the zero that the builtins leave
# undefined. If the `? : 64` guard is wrong, this is what says so.
cat > "$SCRATCH/bitops-equiv.cpp" <<'CPP'
// Compiled twice by this script: once with the Android x86_64 baseline flags (so
// vmxcodec_x86.h's portable __lzcnt64/_tzcnt_u64 are in play) and once with -march=haswell (so the
// hardware instructions are). Both runs must print the same digest, and it must equal the value
// Intel's SDM defines for LZCNT/TZCNT — 64 for a zero operand, not whatever the raw builtin does.
#include <cstdio>
#include <cstring>
#include "vmxcodec_x86.h"
int main() {
  unsigned long long acc = 1469598103934665603ull;  // FNV offset, so the digest is order-sensitive
  unsigned long long probes[] = {0ull, 1ull, 2ull, 3ull, 0x8000000000000000ull,
                                 0x00000000ffffffffull, 0x0001000000000000ull, ~0ull};
  for (unsigned long long t : probes) {
    unsigned long long l = __lzcnt64(t), z = _tzcnt_u64(t);
    if (l != (t ? __builtin_clzll(t) : 64) || z != (t ? (unsigned long long)__builtin_ctzll(t) : 64)) {
      printf("MISMATCH t=%llx lzcnt=%llu tzcnt=%llu\n", t, l, z);
      return 1;
    }
    acc ^= l; acc *= 1099511628211ull;
    acc ^= z; acc *= 1099511628211ull;
  }
  printf("bitops digest %llx\n", acc);
  return 0;
}
CPP
# shellcheck disable=SC2086
$CXX $BASE $X86_BASELINE $INC "$SCRATCH/bitops-equiv.cpp" -o "$SCRATCH/bitops-portable" \
    2>"$SCRATCH/bitops-portable.log" || { echo "  bitops-equiv (SSE4.1): BUILD FAILED"; sed 's/^/      /' "$SCRATCH/bitops-portable.log" | head -10; rc=1; }
# shellcheck disable=SC2086
$CXX $BASE -march=haswell $INC "$SCRATCH/bitops-equiv.cpp" -o "$SCRATCH/bitops-hardware" \
    2>"$SCRATCH/bitops-hardware.log" || { echo "  bitops-equiv (haswell): BUILD FAILED"; sed 's/^/      /' "$SCRATCH/bitops-hardware.log" | head -10; rc=1; }
if [ -x "$SCRATCH/bitops-portable" ] && [ -x "$SCRATCH/bitops-hardware" ]; then
  p=$("$SCRATCH/bitops-portable" | tail -1); h=$("$SCRATCH/bitops-hardware" | tail -1)
  if [ "$p" = "$h" ] && [ "${p#bitops digest }" != "$p" ]; then
    echo "  bitops-equiv: pass as expected (portable and hardware spellings agree: $p)"
  else
    echo "  bitops-equiv: FAIL — portable=[$p] hardware=[$h]"
    rc=1
  fi
fi

run_case fv-fixed "$ROOT/qa/frame-validation.cpp"
run_case vs-fixed "$ROOT/qa/vmx-saveto-contract.cpp"
run_case dp-fixed "$ROOT/qa/drop-policy.cpp"
run_case cs-fixed "$ROOT/qa/colorspace-gate.cpp"
run_case ss-fixed "$ROOT/qa/slow-subscriber.cpp"
echo
if [ $rc -ne 0 ]; then echo "X86 BASELINE A/B FAILED"; exit 1; fi
echo "X86 BASELINE A/B PASSED (the codec is correct at the shipped SSE4.1 floor, not only at -march=haswell)"
