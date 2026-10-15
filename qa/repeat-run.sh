#!/usr/bin/env bash
# Re-run one already-built scenario binary N times and report the pass/fail tally.
#
#   qa/repeat-run.sh <scratch-dir> <binary-name> <expect pass|fail> [N]
#
# Why this exists: qa/build-host.sh scores each scenario ONCE and compares the verdict with what it
# predicted. For a deterministic gate that is enough. For a gate that observes a race — two writers
# on one socket, a teardown against an in-flight write — a single red run proves nothing, because a
# single run is exactly what a coin flip looks like. sw-mutant-video-unlocked stayed green for a
# whole suite run on the strength of that: S1 only goes red if the kernel happens to let the two
# writers collide, so the suite was measuring the scheduler, not the lock.
#
# The rule this encodes: a mutant must be red N/N, and the fixed build green N/N, before anyone
# calls the gate proven. If either comes back split, the gate is timing-dependent — go make the
# collision structural (see S4 in qa/socket-write-order.cpp: prove the write is stuck from outside,
# then release the second writer into window rounds the peer controls), do not ship the coin flip.
#
# It runs the binaries build-host.sh already produced, so mutant definitions live in exactly one
# place. Re-run qa/build-host.sh first if the tree or the test changed.
set -uo pipefail
SCRATCH=${1:?usage: qa/repeat-run.sh <scratch-dir> <binary-name> <expect pass|fail> [N]}
NAME=${2:?usage: qa/repeat-run.sh <scratch-dir> <binary-name> <expect pass|fail> [N]}
EXPECT=${3:?usage: qa/repeat-run.sh <scratch-dir> <binary-name> <expect pass|fail> [N]}
N=${4:-5}
[ -x "$SCRATCH/$NAME" ] || { echo "$NAME: no such binary in $SCRATCH (run qa/build-host.sh first)"; exit 2; }
[ "$EXPECT" = pass ] || [ "$EXPECT" = fail ] || { echo "expect must be 'pass' or 'fail'"; exit 2; }

p=0; f=0; firstfail=""
for i in $(seq 1 "$N"); do
  timeout 120 "$SCRATCH/$NAME" >"$SCRATCH/$NAME.repeat.$i.log" 2>&1
  status=$?
  if [ "$status" -eq 0 ]; then p=$((p+1)); got=pass; else f=$((f+1)); got=fail; [ -z "$firstfail" ] && firstfail="$SCRATCH/$NAME.repeat.$i.log"; fi
  echo "  run $i: $got"
done
echo "$NAME: pass=$p fail=$f of $N (expected $EXPECT)"
if { [ "$EXPECT" = pass ] && [ "$p" -eq "$N" ]; } || { [ "$EXPECT" = fail ] && [ "$f" -eq "$N" ]; }; then
  echo "$NAME: deterministic $N/$N as expected"
  exit 0
fi
echo "$NAME: NOT DETERMINISTIC — $p/$N passed but every run was expected to $EXPECT"
[ -n "$firstfail" ] && echo "  first opposite verdict: $firstfail"
exit 1
