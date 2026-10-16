#!/usr/bin/env bash
# Anti-vacuity harness for qa/abi-provenance.sh.
#
# A gate that has never gone red proves nothing, so this file does not accept "it printed PROVEN" as
# evidence. It takes a real dual-ABI APK and performs the five cheats the gate exists to catch,
# each by byte surgery on a copy of the APK — no rebuild, no trust in what the build tree claims.
# Every cheat MUST be REFUTED; if any of them passes, the gate is decorative and this script exits 1.
#
# usage: qa/abi-provenance-selftest.sh <apk>
#
# The positive control runs with --allow-dirty: whether the tree was dirty is a property of the
# machine that built the APK, and the self-test is about whether the gate can see at all. The dirty
# path is exercised for real by the dirty=1 stamp the gate refuses on a development tree.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
GATE="$HERE/abi-provenance.sh"
APK=${1:-}
[ -n "$APK" ] && [ -f "$APK" ] || { echo "usage: $0 <dual-abi apk>" >&2; exit 2; }
# Absolute, because the ABI scan below runs from a scratch directory and a relative path would
# silently resolve to nothing — which the scan then reports as "the APK packages no lib/
# directories", a wrong answer that reads like a real finding.
APK=$(readlink -f "$APK")

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
rc=0

# mutate_dir <name> — unzip the APK into a scratch tree and echo its path. Mutate that tree, then
# pack <name> to get an APK back. META-INF goes with it: the signature no longer describes the
# contents we just edited, and the gate reads lib/ only. A checker that scored a signed-but-
# unreadable APK as a pass would be worse than one that scores an unsigned one.
mutate_dir() {
  local d="$W/$1"
  rm -rf "$d" "$W/$1.apk"; mkdir -p "$d"
  unzip -q -o "$APK" -d "$d"; rm -rf "$d/META-INF"
  echo "$d"
}
pack() { # <name> — zip the mutated tree back up, echo the APK path
  # python's zipfile, not the zip CLI: `zip` is not present on every build host and a self-test
  # that silently no-ops because a binary is missing is worse than one that fails.
  python3 - "$W/$1" "$W/$1.apk" <<'PY'
import sys, os, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(src):
        for f in sorted(files):
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, src))
PY
  echo "$W/$1.apk"
}

expect() { # <verdict PROVEN|REFUTED> <label> <apk> [gate args...]
  local want=$1 label=$2 apk=$3; shift 3
  local out; out=$(bash "$GATE" "$apk" "$@" 2>&1); local got=$?
  local word=PROVEN; [ $got -ne 0 ] && word=REFUTED
  if [ "$word" = "$want" ]; then
    echo "  $label: $word as expected    $(echo "$out" | tail -1 | cut -c1-110)"
  else
    echo "  $label: $word but expected $want  <-- GATE IS VACUOUS"
    echo "$out" | sed 's/^/      /'
    rc=1
  fi
}

ABIS=$(cd "$W" && unzip -l "$APK" 'lib/*' 2>/dev/null | grep -oE 'lib/[^/]+/' | sed 's|lib/||;s|/||' | sort -u | paste -sd, -)
[ -n "$ABIS" ] || { echo "  the APK packages no lib/ directories — nothing to test"; exit 1; }
echo "== abi-provenance self-test on $(basename "$APK") (abis: $ABIS) =="

# 0. the honest APK must pass, or every red below only says the gate is broken.
expect PROVEN "control: unmodified APK" "$APK" --abis "$ABIS" --allow-dirty

# 1. a lib with no stamp: the prebuilt/copied-in case, and the case where someone drops the
#    generated TU from the target. Blanking the stamp bytes is what a lib with no provenance looks
#    like from the outside.
D=$(mutate_dir nostamp)
python3 - "$D" <<'PY'
import sys, glob
for so in glob.glob(sys.argv[1] + "/lib/*/*.so"):
    d = bytearray(open(so, "rb").read()); k = d.find(b"omt-build/1 ")
    if k < 0: continue
    e = d.index(b"\0", k)
    d[k:e] = b"x" * (e - k)
    open(so, "wb").write(bytes(d))
PY
A=$(pack nostamp)
expect REFUTED "cheat: stamp erased from every lib" "$A" --abis "$ABIS" --allow-dirty

# 2. one ABI cached from another tree: forge its stamp's source hash. This is the whole claim of the
#    gate — the two ABIs were compiled from one revision — so the test for it must be a lie about
#    exactly that, not a missing file.
D=$(mutate_dir forged)
python3 - "$D" <<'PY'
import sys, glob
# rewrite only the x86_64 libs' hash, leaving arm64 asserting the truth
for so in glob.glob(sys.argv[1] + "/lib/x86_64/*.so"):
    d = bytearray(open(so, "rb").read()); k = d.find(b"omt-src-sha256=")
    if k < 0: continue
    d[k + len(b"omt-src-sha256="):k + len(b"omt-src-sha256=") + 64] = b"0" * 64
    open(so, "wb").write(bytes(d))
PY
A=$(pack forged)
expect REFUTED "cheat: x86_64 libs stamped with a different source hash" "$A" --abis "$ABIS" --allow-dirty

# 3. the same lib copied into both ABI directories — the "we only rebuilt one and reused the other"
#    shortcut, caught by abi= inside the stamp rather than by the directory name.
D=$(mutate_dir copied)
first=$(ls "$D/lib" | head -1); second=$(ls "$D/lib" | tail -1)
if [ "$first" = "$second" ]; then
  echo "  cheat: one ABI only — nothing to cross-copy  <-- NEEDS A DUAL-ABI APK"; rc=1
else
  cp "$D/lib/$first/libvmx.so" "$D/lib/$second/libvmx.so"
  A=$(pack copied)
  expect REFUTED "cheat: $first's libvmx.so parked in lib/$second/" "$A" --abis "$ABIS" --allow-dirty
fi

# 4. an ABI that quietly disappeared from the APK.
expect REFUTED "cheat: a required ABI is absent" "$APK" --abis "$ABIS,armeabi-v7a" --allow-dirty

# 5. a stamp copied from an old release, asserting a revision this checkout never had.
expect REFUTED "cheat: --expect-rev disagrees with the stamp" "$APK" --abis "$ABIS" --allow-dirty \
  --expect-rev 0000000000000000000000000000000000000000

echo
if [ $rc -ne 0 ]; then echo "SELF-TEST FAILED — abi-provenance.sh cannot see at least one of these cheats"; exit 1; fi
echo "SELF-TEST PASSED (the gate is green on the honest APK and red on all five fabrications)"
