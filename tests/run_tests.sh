#!/usr/bin/env bash
# Test suite for the log-processing pipeline.
#   - normal cases          (known hand-computed answer, oracle comparison)
#   - boundary cases        (empty file, 1 line, no trailing newline, levels)
#   - malformed input       (12 different defects in one file)
#   - failure scenarios     (a stage is killed / exits / hangs: must NOT hang)
#   - restart challenge     (failed stage restarted: no lost / duplicated records)
# Usage: bash tests/run_tests.sh      (run from the project root, after `make`)

cd "$(dirname "$0")/.." || exit 1
PIPE=./bin/logpipe; SINGLE=./bin/logsingle; GEN=./bin/loggen
T=$(mktemp -d /tmp/logpipe-tests.XXXXXX)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0

ok()   { echo "  [PASS] $1"; pass=$((pass+1)); }
bad()  { echo "  [FAIL] $1"; fail=$((fail+1)); }
check() { local name=$1; shift; if "$@" >/dev/null 2>&1; then ok "$name"; else bad "$name"; fi; }
section() { echo; echo "== $1"; }

[ -x $PIPE ] && [ -x $SINGLE ] && [ -x $GEN ] || { echo "run 'make' first"; exit 1; }

# ---------------------------------------------------------------- normal
section "Normal cases"
$PIPE -q tests/data/sample.log > $T/a.out 2>/dev/null
check "N1 pipeline matches hand-computed report (sample.log)" diff $T/a.out tests/data/sample.expected
$SINGLE tests/data/sample.log > $T/b.out
check "N2 single-process matches hand-computed report"        diff $T/b.out tests/data/sample.expected

$GEN -n 200000 -s 42 -m 20 -o $T/big.log
$PIPE -q $T/big.log > $T/big.pipe 2>/dev/null
$SINGLE $T/big.log > $T/big.single
python3 tests/oracle.py $T/big.log > $T/big.oracle
check "N3 pipeline == single-process (200k lines, 2% malformed)" cmp $T/big.pipe $T/big.single
check "N4 pipeline == independent Python oracle"                 cmp $T/big.pipe $T/big.oracle

# ---------------------------------------------------------------- malformed
section "Malformed log entries"
M=$T/malformed.log
{
  echo "2026-03-01 08:00:00 INFO valid info line"                      # valid, filtered out
  echo "2026-03-01 08:00:01 ERROR valid error line"                    # valid
  echo "2026-03-01 08:00:00 error lower case level"                    # 1 bad level case
  echo "2026-13-01 08:00:00 ERROR bad month"                           # 2
  echo "2026-02-30 08:00:00 ERROR bad day"                             # 3
  echo "2026-03-01 25:00:00 ERROR bad hour"                            # 4
  echo "2026-03-01 08:00:00 CRITICAL unknown level"                    # 5
  echo "2026-03-01 08:00:00 ERROR"                                     # 6 no message
  echo ""                                                               # 7 blank line
  echo "this is not a log line at all"                                 # 8
  printf '2026-03-01 08:00:00 ERROR has\0nul byte\n'                   # 9 NUL byte
  printf '2026-03-01 08:00:00 ERROR %s\n' "$(head -c 5000 /dev/zero | tr '\0' 'x')"  # 10 over-long
  echo "2026-03-01 08:00 ERROR short timestamp"                        # 11
  echo "2026-03-01 08:00:00 ERROR     "                                # 12 blank message
  echo "2024-02-29 12:00:00 ERROR leap day is valid"                   # valid (2024 is a leap year)
  printf '2026-03-01 08:00:02 WARN %s\n' "$(head -c 300 /dev/zero | tr '\0' 'y')"    # valid, message truncated
  printf '2026-03-01 09:00:00 WARN no trailing newline'                # valid, no final newline
} > $M
$PIPE -q $M > $T/m.pipe 2>/dev/null; echo "$?" > $T/m.rc
$SINGLE $M > $T/m.single
python3 tests/oracle.py $M > $T/m.oracle
check "M1 pipeline exits 0 on malformed input"                  test "$(cat $T/m.rc)" = 0
check "M2 pipeline report == oracle"                            cmp $T/m.pipe $T/m.oracle
check "M3 single-process report == oracle"                      cmp $T/m.single $T/m.oracle
check "M4 exactly 12 malformed lines counted"                   grep -q "malformed            : 12$" $T/m.pipe
check "M5 5 valid lines, 4 kept, 1 filtered"                    bash -c "grep -q 'well-formed          : 5$' $T/m.pipe && grep -q 'kept                 : 4$' $T/m.pipe && grep -q 'filtered out         : 1$' $T/m.pipe"
check "M6 total lines scanned = 17 (nothing lost)"              grep -q "Lines scanned          : 17$" $T/m.pipe

# ---------------------------------------------------------------- boundary
section "Boundary cases"
: > $T/empty.log
$PIPE -q $T/empty.log > $T/e.pipe 2>/dev/null; rc=$?
check "B1 empty file: exit 0"                          test $rc = 0
check "B2 empty file: report == oracle"                bash -c "python3 tests/oracle.py $T/empty.log | cmp - $T/e.pipe"
echo "2026-01-01 00:00:00 FATAL only line" > $T/one.log
check "B3 single-line file == oracle"                  bash -c "$PIPE -q $T/one.log 2>/dev/null | cmp - <(python3 tests/oracle.py $T/one.log)"
printf 'junk\nmore junk\n\n' > $T/allbad.log
check "B4 file with only malformed lines == oracle"    bash -c "$PIPE -q $T/allbad.log 2>/dev/null | cmp - <(python3 tests/oracle.py $T/allbad.log)"
for lv in DEBUG INFO WARN ERROR FATAL; do
  check "B5 min-level $lv: pipeline == oracle"         bash -c "$PIPE -q -l $lv $T/big.log 2>/dev/null | cmp - <(python3 tests/oracle.py -l $lv $T/big.log)"
done
$PIPE -q /nonexistent/file.log > /dev/null 2>$T/ne.err; rc=$?
check "B6 missing input file: exit code 2, clear message"  bash -c "test $rc = 2 && grep -q 'cannot read' $T/ne.err"
$PIPE -q -l BOGUS $T/one.log >/dev/null 2>&1; check "B7 invalid level rejected (non-zero exit)" test $? -ne 0
$PIPE -q -o $T/out.rep $T/one.log 2>/dev/null
check "B8 -o writes report file atomically (no .tmp left)"  bash -c "test -s $T/out.rep && ! test -e $T/out.rep.tmp"

# ---------------------------------------------------------------- failures
section "Failure scenarios (must terminate, must not print a wrong report)"
for spec in reader:kill:30000 filter:kill:30000 summarizer:kill:30000 \
            reader:exit:30000 filter:exit:30000 summarizer:exit:30000; do
  start=$(date +%s.%N)
  timeout 30 $PIPE -t 20 --inject $spec $T/big.log > $T/f.out 2> $T/f.err; rc=$?
  el=$(printf "%.1f" "$(echo "$(date +%s.%N) - $start" | bc)")
  check "F  $spec: exits 1 (not hang: ${el%.*}s, rc=$rc)"  bash -c "test $rc = 1 && test $(echo "$el < 10" | bc) = 1"
  check "F  $spec: no partial report on stdout"            test ! -s $T/f.out
done
timeout 30 $PIPE --inject summarizer:kill:100 $T/big.log >/dev/null 2>$T/pipe.err
check "F7 broken pipe: upstream stages detect EPIPE and exit 4"  bash -c "grep -q 'downstream stage is gone' $T/pipe.err && grep -q 'exited with code 4' $T/pipe.err"
timeout 30 $PIPE --inject reader:kill:100 $T/big.log >/dev/null 2>$T/eof.err
check "F8 killed reader: filter reports truncated stream"        grep -q "without an end marker" $T/eof.err
start=$(date +%s.%N)
timeout 30 $PIPE -q -t 2 --inject filter:hang:1000 $T/big.log >/dev/null 2>$T/hang.err; rc=$?
el=$(printf "%.1f" "$(echo "$(date +%s.%N) - $start" | bc)")
check "F9 hung stage: watchdog kills pipeline after 2s (took ${el%.*}s)" bash -c "test $rc = 1 && test $(echo "$el < 8" | bc) = 1 && grep -q TIMEOUT $T/hang.err"
echo "garbage" > $T/bad.ckpt
timeout 30 $PIPE -q -c $T/bad.ckpt $T/one.log >/dev/null 2>$T/ck.err; rc=$?
check "F10 corrupt checkpoint: fails cleanly (rc=$rc), no hang"  test $rc = 1

# ---------------------------------------------------------------- restart
section "Restart challenge: no lost / duplicated records"
# The summarizer only sees ~27% of the frames (after filtering), so it is killed earlier.
for stage in reader:90001 filter:90001 summarizer:30001; do
  rm -f $T/ck.dat
  stage_name=${stage%%:*}
  timeout 60 $PIPE -c $T/ck.dat -n 777 -r 2 --inject $stage_name:kill:${stage##*:} $T/big.log > $T/r.out 2> $T/r.err; rc=$?
  check "R  $stage_name killed mid-run, restarted: exit 0"            test $rc = 0
  check "R  $stage_name restart: report identical to single-process"  cmp $T/r.out $T/big.single
  check "R  $stage_name restart: RESULT shows 2 attempts"             grep -q "attempts=2" $T/r.err
  check "R  $stage_name restart: checkpoint removed after success"    test ! -e $T/ck.dat
done
grep -h "resumed from checkpoint" $T/r.err | head -1 | sed 's/^/        evidence: /'
rm -f $T/ck.dat
timeout 60 $PIPE -q -c $T/ck.dat -n 777 -r 2 --inject summarizer:kill:0 --inject-always $T/big.log >/dev/null 2>/dev/null; rc=$?
check "R4 fault on every attempt: gives up after max restarts (rc=1)"  test $rc = 1
timeout 30 $PIPE -q -r 1 $T/one.log >/dev/null 2>&1
check "R5 --max-restarts without --checkpoint is rejected"       test $? -eq 2

echo
echo "RESULT: $pass passed, $fail failed"
[ $fail -eq 0 ]
