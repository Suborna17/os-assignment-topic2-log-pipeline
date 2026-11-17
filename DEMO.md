# Live demonstration and oral defence cheat-sheet

## 5-minute demo script
```bash
make && make test | tail -3                       # 52 passed, 0 failed
./bin/loggen -n 100000 -s 7 -m 20 -o app.log
./bin/logpipe app.log                             # 1. normal run: PIDs, PPIDs, reaped statuses, report
./bin/logsingle app.log | diff - <(./bin/logpipe -q app.log 2>/dev/null) && echo IDENTICAL
./bin/logpipe --inject summarizer:kill:100 app.log   # 2. kill a stage: EPIPE ripples up, exit 1, no hang
./bin/logpipe --inject reader:kill:30000 app.log     # 3. EOF without trailer detected downstream
./bin/logpipe -t 2 --inject filter:hang:1000 app.log # 4. watchdog kills a hung stage
./bin/logpipe -c /tmp/ck -n 777 -r 2 --inject summarizer:kill:30001 app.log   # 5. restart, same report
python3 tests/oracle.py app.log | diff - <(./bin/logpipe -q app.log 2>/dev/null) && echo ORACLE-MATCH
```
Second terminal while a big run is going: `ps -eo pid,ppid,stat,comm --forest | grep logpipe`

## Likely questions
* **Why close unused pipe ends?** EOF is delivered only when *all* write ends are closed; a leaked write end = hang.
* **Why ignore SIGPIPE?** Default action kills the process silently; ignoring turns it into EPIPE so we can log and exit with a code.
* **How do you know nothing was lost?** `E` trailer frame count + `dropped` field accounting + comparison with an independent Python oracle.
* **Why is the pipeline slower?** 1 CPU, and the 3 text parse/format passes cost ~5x user CPU (0.925 s vs 0.185 s per 1M lines); kernel time is only ~4%.
* **Would more cores help?** Estimated: not for this light per-record work (slowest stage > whole single-process job); yes for heavy per-record work. Not measured.
* **Why is rename() used?** Atomic: checkpoint/report is old or new, never half-written.
* **Where is a failed record's identity?** `seq` = input line number.
* **Modify on demand ideas:** change default severity, add a new level, add a 4th stage, change the frame format, add per-minute error counts.
