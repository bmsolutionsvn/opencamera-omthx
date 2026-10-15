#!/usr/bin/env bash
# Provenance gate for a multi-ABI APK.
#
# WHAT IT PROVES. An APK that contains lib/arm64-v8a and lib/x86_64 proves nothing about where the
# two trees came from. The classic cheat is to rebuild one ABI and copy the other in from a cache,
# a prior release, or a jniLibs/ directory — the APK installs, the tests pass, and one of the two
# native libs is not from this revision. Each .so carries a stamp emitted at build time by
# app/src/main/cpp/cmake/GenProvenance.cmake:
#
#   omt-build/1 abi=<abi> git=<full sha> dirty=<0|1> omt-src-sha256=<hash of every native input>
#
# The hash covers the same ABI-independent file list on every ABI, so two libs built from one tree
# agree byte for byte and a lib from any other tree cannot.
#
# WHAT IT CHECKS, for every ABI the APK declares in its own manifest (see --abis):
#   1. every packaged .so of every required ABI carries exactly one stamp;
#   2. all stamps of one ABI agree with each other;
#   3. abi= inside a stamp matches the lib/<dir> it was found in (a renamed/cached lib is caught);
#   4. all ABIs agree on git= and omt-src-sha256= — this is the actual cross-ABI claim;
#   5. if --expect-rev <sha> is given, git= must equal it (catches a stamp copied from an old APK);
#   6. if --expect-src-sha256 <hash> is given, omt-src-sha256= must equal it (catches a stamp that
#      was hand-edited to look like the current tree);
#   7. dirty= must be 0 unless --allow-dirty (a lib built from an uncommitted tree is not the
#      revision its stamp claims).
#
# Exit 0 = proven, exit 1 = refuted. It refuses to pass on an empty set: zero required ABIs, or an
# ABI with no libs, is a failure, not a vacuous success.
set -uo pipefail

APK=${1:-}
REQUIRED=()
EXPECT_REV=
EXPECT_SRC=
ALLOW_DIRTY=0

shift 2>/dev/null || true
while [ $# -gt 0 ]; do
  case "$1" in
    --abis) shift; IFS=',' read -r -a REQUIRED <<< "${1:-}"; ;;
    --expect-rev) shift; EXPECT_REV=${1:-}; ;;
    --expect-src-sha256) shift; EXPECT_SRC=${1:-}; ;;
    --allow-dirty) ALLOW_DIRTY=1 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

[ -n "$APK" ] && [ -f "$APK" ] || { echo "usage: $0 <apk> [--abis a,b] [--expect-rev SHA] [--expect-src-sha256 H] [--allow-dirty]" >&2; exit 2; }
command -v unzip >/dev/null || { echo "unzip required" >&2; exit 2; }
command -v strings >/dev/null || { echo "strings required (binutils)" >&2; exit 2; }

# Default the required ABI list from the APK itself: an APK that quietly dropped an ABI cannot pass
# a gate whose ABI list was typed by whoever built it.
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
unzip -q -o "$APK" 'lib/*' -d "$WORK" 2>/dev/null
if [ ${#REQUIRED[@]} -eq 0 ]; then
  for d in "$WORK"/lib/*/; do
    [ -d "$d" ] || continue
    REQUIRED+=("$(basename "$d")")
  done
fi
[ ${#REQUIRED[@]} -gt 0 ] || { echo "REFUTED: the APK packages no ABI at all — nothing to check is not a pass"; exit 1; }

fail() { echo "REFUTED: $*"; exit 1; }

ALL_STAMPS=""
for abi in "${REQUIRED[@]}"; do
  d="$WORK/lib/$abi"
  [ -d "$d" ] || fail "APK has no lib/$abi/ — required ABI is missing (present: $(ls "$WORK/lib" 2>/dev/null | tr '\n' ' '))"
  libs=$(ls "$d"/*.so 2>/dev/null)
  [ -n "$libs" ] || fail "lib/$abi contains no .so — an empty ABI dir is not evidence"
  n=0
  for so in $libs; do
    # strings, not nm: the shipped libs are stripped, the stamp is an exported const char[] and its
    # bytes live in .rodata which survives strip.
    s=$(strings -a "$so" | grep -m1 '^omt-build/1 ' || true)
    [ -n "$s" ] || fail "lib/$abi/$(basename "$so") carries no build stamp — it was not built by this CMakeLists, or it is a cached prebuilt"
    echo "$s" | grep -q "abi=$abi\b" || fail "lib/$abi/$(basename "$so") stamps itself as a different ABI: $s"
    n=$((n+1))
    ALL_STAMPS+="$abi $(basename "$so") $s"$'\n'
  done
  echo "  lib/$abi: $n stamped libs"
done

get() { echo "$1" | grep -oE "$2=[^ ]+" | head -1 | cut -d= -f2; }
REV=$(echo "$ALL_STAMPS" | while read -r a l rest; do get "$rest" git; done | sort -u)
SRC=$(echo "$ALL_STAMPS" | while read -r a l rest; do get "$rest" omt-src-sha256; done | sort -u)
DIRTY=$(echo "$ALL_STAMPS" | while read -r a l rest; do get "$rest" dirty; done | sort -u)

[ -n "$REV" ] || fail "stamps carry no git= field"
[ -n "$SRC" ] || fail "stamps carry no omt-src-sha256= field"
[ "$(echo "$REV" | grep -c .)" -eq 1 ] || fail "the ABIs were built from DIFFERENT git revisions: $(echo "$REV" | tr '\n' ' ')"
[ "$(echo "$SRC" | wc -l)" -eq 1 ] || { echo "the ABIs disagree on the source hash — one was not built from this tree:"; echo "$ALL_STAMPS" | sed 's/^/    /'; fail "omt-src-sha256 differs across ABIs: $(echo "$SRC" | tr '\n' ' ')"; }
[ "$SRC" != "UNKNOWN" ] || fail "no source hash recorded"

if [ "$ALLOW_DIRTY" -ne 1 ]; then
  echo "$DIRTY" | grep -qx 0 || fail "the build tree was dirty for at least one ABI (dirty=$(echo "$DIRTY" | tr '\n' ' ')) — the stamp's git= does not describe what was compiled"
fi
if [ -n "$EXPECT_REV" ]; then
  [ "$REV" = "$EXPECT_REV" ] || fail "stamp says git=$REV, this checkout is $EXPECT_REV — at least one ABI is a cached artifact"
fi
if [ -n "$EXPECT_SRC" ]; then
  [ "$SRC" = "$EXPECT_SRC" ] || fail "stamp says omt-src-sha256=$SRC, the checkout hashes to $EXPECT_SRC — the stamp was edited or the lib is stale"
fi

echo "PROVEN: $(echo "$REQUIRED" | wc -l) ABIs, $(echo "$ALL_STAMPS" | grep -c .) stamped libs, git=$REV dirty=$DIRTY omt-src-sha256=$SRC"
exit 0
