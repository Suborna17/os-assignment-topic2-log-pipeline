# Topic 2 - Interprocess Communication Through a Log-Processing Pipeline

A multi-process log analyser written in C. Three cooperating processes joined by
POSIX pipes read a log file, filter events by severity and produce a summary of
error counts. A single-process version is included for comparison.

```
 file --> [reader] --pipe1--> [filter] --pipe2--> [summarizer] --> report
              ^                    ^                   ^
              +---- fork()/waitpid() by the parent (coordinator) ----+
```

## Requirements

| Item | Version used / needed |
|---|---|
| OS | Linux (tested on Ubuntu 24.04, kernel 6.x). macOS also works (POSIX only). |
| Compiler | gcc >= 9 (or clang), `make` |
| Python 3 | only for the test oracle, benchmark and figures |
| matplotlib, reportlab | only to re-create graphs / the PDF report (`pip install matplotlib reportlab`) |
| `bc` | used by the test script for timings |

## Build

```bash
make            # builds bin/logpipe, bin/logsingle, bin/loggen
make test       # builds and runs the 52-case test suite
make bench      # runs the benchmark (takes ~2 minutes), writes results/bench.csv
make clean
```

## Usage

```bash
# generate a test log: 100000 lines, seed 7, 2% malformed lines
./bin/loggen -n 100000 -s 7 -m 20 -o app.log

# multi-process pipeline (report on stdout, lifecycle log on stderr)
./bin/logpipe app.log
./bin/logpipe -l ERROR -o report.txt app.log        # keep only ERROR and FATAL

# single-process baseline (identical report)
./bin/logsingle app.log
```

Log line format: `YYYY-MM-DD HH:MM:SS LEVEL message` with LEVEL one of
`DEBUG INFO WARN ERROR FATAL`. Everything else is *malformed* and is counted.

### `logpipe` options

| Option | Meaning |
|---|---|
| `-l, --min-level LEVEL` | keep events at or above LEVEL (default `WARN`) |
| `-o, --output FILE` | write report to FILE (atomic rename) instead of stdout |
| `-c, --checkpoint FILE` | enable checkpointing of the summarizer state |
| `-n, --ckpt-every N` | checkpoint every N frames (default 1000) |
| `-r, --max-restarts N` | restart a failed pipeline up to N times (needs `-c`) |
| `-t, --timeout SEC` | watchdog: kill the pipeline's own children after SEC seconds |
| `-q, --quiet` | suppress the lifecycle log on stderr |
| `--inject STAGE:MODE:N` | fault injection for tests: STAGE = reader/filter/summarizer, MODE = kill/exit/hang, fires after N frames |
| `--inject-always` | inject on every attempt (default: first attempt only) |

Exit codes of `logpipe`: `0` success, `1` pipeline failed, `2` usage / input error.
Stage exit codes (visible in the lifecycle log): `3` truncated or corrupt stream,
`4` broken pipe (downstream stage died), `2` I/O error.

### Restart example (challenge)

```bash
./bin/logpipe -c /tmp/ck.dat -n 777 -r 2 --inject summarizer:kill:30001 app.log
# the summarizer is killed with SIGKILL, the pipeline restarts from the
# checkpoint, and the final report is identical to ./bin/logsingle app.log
```

## Repository layout

```
src/logcore.[ch]   shared parser, summary/report, atomic checkpoint
src/logpipe.c      coordinator + reader/filter/summarizer stages (fork, pipe, waitpid)
src/logsingle.c    single-process baseline
src/loggen.c       deterministic synthetic log generator
tests/             run_tests.sh (52 checks), oracle.py (independent reference), data/
scripts/           bench.py, make_figures.py, make_report.py
results/           test_results.txt, bench.csv, environment.txt, samples/ (logs, reports)
docs/              diagrams and graph (PNG), report PDF
```

## Testing

`make test` covers normal cases (hand-computed report, 200k-line comparison with
the single-process version and with an independent Python oracle), malformed
input (12 different defects), boundary cases (empty file, one line, no trailing
newline, every severity threshold, missing file), failure scenarios (each stage
killed / exiting / hanging, broken pipe, watchdog timeout, corrupt checkpoint)
and the restart challenge (each stage killed mid-run and restarted).

## Limitations

* Text frames are simple and debuggable but expensive: each stage re-parses every frame.
  On a 1-core machine the pipeline is 2.7x-4.4x *slower* than the single process (see report).
* Restart assumes the input file is not modified between attempts (line numbers are the identity of a record).
* Messages are truncated to 255 characters; physical lines over 4096 bytes are treated as malformed.
* One input file per run; the summarizer is the only stage that checkpoints.
