#!/usr/bin/env bash
# Host-side QA for the vendored OMT C++ — no device, no JVM, no emulator.
#
# It also runs the A/B that makes the result meaningful: the same test is compiled against the
# pre-fix sources from `git show master:...`. A test that passes on both is not testing anything,
# so the pre-fix run is EXPECTED TO FAIL, and this script fails if it does not.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
SCRATCH=${SCRATCH:-/tmp/omt-qa}
mkdir -p "$SCRATCH"
CXX=${CXX:-g++}
# OMTChannel.cpp guards <android/log.h> behind #ifdef __ANDROID__ and falls back to printf,
# so the same sources build on the host with no shim at all.
# -include string: OMTChannel.h uses std::string without including <string>. The NDK's libc++
# happens to provide it transitively; host libstdc++ does not, and the A/B must compare BEHAVIOUR
# between fixed and pre-fix trees, not one that fails to compile. The fixed tree now includes it.
FLAGS="-std=c++17 -O1 -g -fsanitize=address,undefined -pthread -include string -I$ROOT/libomt/cpp"

run_case() { # name, source-dir, expect(pass|fail)
  local name=$1 srcdir=$2 expect=$3
  rm -f "$SCRATCH/$name"
  # shellcheck disable=SC2086
  $CXX $FLAGS "$ROOT/qa/slow-subscriber.cpp" \
       "$srcdir/OMTChannel.cpp" "$srcdir/OMTAsyncPool.cpp" \
       -o "$SCRATCH/$name" 2>"$SCRATCH/$name.build.log"
  if [ $? -ne 0 ]; then echo "  BUILD FAILED ($name):"; sed 's/^/    /' "$SCRATCH/$name.build.log" | head -12; return 2; fi
  # A build that produced no binary must never be scored as a run result.
  if [ ! -x "$SCRATCH/$name" ]; then echo "  NO BINARY for $name (refusing to score an absent run)"; return 2; fi
  # SO_RCVBUF/SO_SNDBUF caps and the deadline both need the kernel to behave; give it a hard cap
  # so a wedged peer really does stop absorbing bytes on loopback.
  timeout 90 "$SCRATCH/$name" >"$SCRATCH/$name.run.log" 2>&1
  local rc=$?
  local verdict=pass; [ $rc -ne 0 ] && verdict=fail
  if [ "$verdict" = "$expect" ]; then
    echo "  $name: $verdict as expected"
  else
    echo "  $name: $verdict but expected $expect  <-- PROBLEM"
    sed 's/^/    /' "$SCRATCH/$name.run.log" | tail -20
    return 1
  fi
}

echo "slow-subscriber (stalled Wi-Fi viewer must not take the stream down):"
rc=0
run_case fixed "$ROOT/libomt/cpp" pass || rc=1

# pre-fix copy: master's channel/pool, master's headers (the test only uses public API)
OLD="$SCRATCH/pre-fix"; rm -rf "$OLD"; mkdir -p "$OLD"
for f in OMTChannel.cpp OMTAsyncPool.cpp OMTChannel.h OMTConstants.h OMTProtocol.h OMTAsyncPool.h; do
  git show "master:libomt/cpp/$f" > "$OLD/$f" 2>/dev/null || { echo "  cannot extract $f from master"; exit 2; }
done
run_case prefix "$OLD" fail || rc=1

echo
if [ $rc -ne 0 ]; then echo "QA FAILED"; exit 1; fi
echo "QA PASSED (fixed code green, pre-fix code red on the same assertions)" 
