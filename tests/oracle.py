#!/usr/bin/env python3
"""Independent reference implementation (Python, regex based) used as a test
oracle. It re-implements the report from the specification, NOT from the C
code, so agreement between the two is meaningful evidence of correctness.

usage: oracle.py [-l LEVEL] LOGFILE   -> prints the expected report
"""
import re, sys, calendar
from collections import Counter

LEVELS = ["DEBUG", "INFO", "WARN", "ERROR", "FATAL"]
LINE = re.compile(rb"^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2}) (DEBUG|INFO|WARN|ERROR|FATAL) +(.*\S.*)$", re.S)

def valid_date(y, mo, d, h, mi, s):
    if y < 1970 or not 1 <= mo <= 12 or h > 23 or mi > 59 or s > 59:
        return False
    try:
        return 1 <= d <= calendar.monthrange(y, mo)[1]
    except Exception:
        return False

def main():
    args = sys.argv[1:]
    minlv = "WARN"
    if args and args[0] == "-l":
        minlv = args[1]; args = args[2:]
    path = args[0]
    data = open(path, "rb").read()
    lines = data.split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()                          # trailing newline is not a line
    total = valid = kept = bad = 0
    by = Counter(); msgs = Counter(); first = last = None
    for raw in lines:
        total += 1
        ln = raw.rstrip(b"\r")
        m = LINE.match(ln) if (b"\0" not in raw and len(raw) + 1 <= 4096) else None
        if not m or not valid_date(*[int(x) for x in m.groups()[:6]]):
            bad += 1
            continue
        valid += 1
        lv = m.group(7).decode()
        if LEVELS.index(lv) < LEVELS.index(minlv):
            continue
        kept += 1
        by[lv] += 1
        g = m.groups()
        t = calendar.timegm(tuple(int(x) for x in g[:6]) + (0, 0, 0))
        first = t if first is None else min(first, t)
        last = t if last is None else max(last, t)
        msgs[g[7].decode("latin-1").rstrip(" \t\r")[:255]] += 1
    fmt = lambda t: "n/a" if t is None else __import__("time").strftime("%Y-%m-%d %H:%M:%S", __import__("time").gmtime(t))
    out = ["=== Log Summary Report ===",
           f"Lines scanned          : {total}",
           f"  well-formed          : {valid}",
           f"  malformed            : {bad}",
           f"Filter                 : severity >= {minlv}",
           f"  kept                 : {kept}",
           f"  filtered out         : {valid - kept}",
           f"Time range (kept, UTC) : {fmt(first)} .. {fmt(last)}",
           "Kept events by severity:"]
    out += [f"  {lv:<6} {by[lv]}" for lv in LEVELS]
    out.append("Top event messages (kept):")
    top = sorted(msgs.items(), key=lambda kv: (-kv[1], kv[0]))[:5]
    out += [f"  {i+1}. {c}  {m}" for i, (m, c) in enumerate(top)] or ["  (none)"]
    print("\n".join(out))

main()
