#!/usr/bin/env python3
"""Benchmark: single-process (logsingle) vs 3-process pipeline (logpipe).

For each input size and severity filter the script runs each program several
times and records wall-clock time (median / min / max) and CPU time
(user+sys of the process AND its waited-for children, via os.wait4 rusage).
Results are written to results/bench.csv; plots are produced by make_plots.py.
Environment details are written to results/environment.txt.
"""
import csv, os, platform, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin")
RES = os.path.join(ROOT, "results")
DATA = "/tmp/logbench"
SIZES = [10_000, 100_000, 1_000_000, 3_000_000]
FILTERS = ["WARN", "DEBUG"]          # WARN: ~27% of records pass; DEBUG: 100% pass
REPS = {10_000: 15, 100_000: 9, 1_000_000: 5, 3_000_000: 3}
MALFORMED_PER_MILLE = 20


def run(cmd):
    """Run a command, return (wall, cpu) seconds."""
    t0 = time.perf_counter()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    _, status, ru = os.wait4(p.pid, 0)
    wall = time.perf_counter() - t0
    if os.waitstatus_to_exitcode(status) != 0:
        sys.exit(f"command failed: {cmd}")
    return wall, ru.ru_utime + ru.ru_stime


def write_env():
    def sh(c):
        return subprocess.run(c, shell=True, capture_output=True, text=True).stdout.strip()
    lines = [
        f"Date            : {sh('date -u +%Y-%m-%dT%H:%M:%SZ')}",
        f"CPU             : {sh('lscpu | grep \"Model name\" | cut -d: -f2')}",
        f"Logical CPUs    : {os.cpu_count()}  (nproc={sh('nproc')})",
        f"Memory          : {sh('free -m | awk \"NR==2{print \\$2}\"')} MB",
        f"Kernel          : {platform.release()}",
        f"OS              : {sh('. /etc/os-release; echo $PRETTY_NAME')}",
        f"Compiler        : {sh('gcc --version | head -1')}",
        f"Compile flags   : -O2 -Wall -Wextra -Wpedantic -std=gnu11",
        f"Pipe capacity   : {sh('cat /proc/sys/fs/pipe-max-size')} bytes max (default pipe buffer 65536)",
        f"Input data      : synthetic, {MALFORMED_PER_MILLE/10:.1f}% malformed lines, seed 42",
    ]
    open(os.path.join(RES, "environment.txt"), "w").write("\n".join(lines) + "\n")
    print("\n".join(lines))


def main():
    os.makedirs(DATA, exist_ok=True)
    os.makedirs(RES, exist_ok=True)
    write_env()
    rows = []
    for n in SIZES:
        path = f"{DATA}/log_{n}.log"
        subprocess.run([f"{BIN}/loggen", "-n", str(n), "-s", "42", "-m", str(MALFORMED_PER_MILLE),
                        "-o", path], check=True)
        size_mb = os.path.getsize(path) / 1e6
        for flt in FILTERS:
            progs = {
                "single": [f"{BIN}/logsingle", "-l", flt, path],
                "pipeline": [f"{BIN}/logpipe", "-q", "-l", flt, path],
            }
            # correctness check before timing: reports must be identical
            a = subprocess.run(progs["single"], capture_output=True).stdout
            b = subprocess.run(progs["pipeline"], capture_output=True).stdout
            assert a == b, f"report mismatch for {n} lines / {flt}"
            run(progs["single"])                     # warm the page cache
            for name, cmd in progs.items():
                walls, cpus = [], []
                for _ in range(REPS[n]):
                    w, c = run(cmd)
                    walls.append(w); cpus.append(c)
                row = dict(lines=n, size_mb=round(size_mb, 1), filter=flt, version=name,
                           reps=REPS[n], wall_median=statistics.median(walls),
                           wall_min=min(walls), wall_max=max(walls),
                           cpu_median=statistics.median(cpus))
                rows.append(row)
                print(f"{n:>9} lines {flt:<5} {name:<8} wall median {row['wall_median']:.3f}s "
                      f"(min {row['wall_min']:.3f}, max {row['wall_max']:.3f})  cpu {row['cpu_median']:.3f}s")
    with open(os.path.join(RES, "bench.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader(); w.writerows(rows)


if __name__ == "__main__":
    main()
