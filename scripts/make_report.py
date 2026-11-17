#!/usr/bin/env python3
"""Build report/Topic2_IPC_Log_Pipeline_Report.pdf from the real result files
(results/bench.csv, results/test_results.txt, results/samples/*)."""
import csv, os, re
from reportlab.lib import colors
from reportlab.lib.enums import TA_JUSTIFY
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm
from reportlab.platypus import (Image, KeepTogether, PageBreak, Paragraph, Preformatted,
                                SimpleDocTemplate, Spacer, Table, TableStyle)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "report", "Topic2_IPC_Log_Pipeline_Report.pdf")
os.makedirs(os.path.dirname(OUT), exist_ok=True)

ss = getSampleStyleSheet()
body = ParagraphStyle("body", parent=ss["BodyText"], fontName="Helvetica", fontSize=9.6,
                      leading=13.2, alignment=TA_JUSTIFY, spaceAfter=5)
h1 = ParagraphStyle("h1", parent=ss["Heading1"], fontName="Helvetica-Bold", fontSize=14,
                    spaceBefore=10, spaceAfter=5, textColor=colors.HexColor("#1e3a8a"))
h2 = ParagraphStyle("h2", parent=ss["Heading2"], fontName="Helvetica-Bold", fontSize=11,
                    spaceBefore=7, spaceAfter=3, textColor=colors.HexColor("#1f2937"))
cap = ParagraphStyle("cap", parent=body, fontSize=8.3, leading=10.5, alignment=1,
                     textColor=colors.HexColor("#444444"), spaceAfter=8)
cell = ParagraphStyle("cell", parent=body, fontSize=8.4, leading=10.4, alignment=0, spaceAfter=0)
cellb = ParagraphStyle("cellb", parent=cell, fontName="Helvetica-Bold")
code = ParagraphStyle("code", fontName="Courier", fontSize=6.4, leading=8,
                      backColor=colors.HexColor("#f3f4f6"), borderPadding=4, spaceAfter=8,
                      leftIndent=4, rightIndent=4)

def P(t, s=body): return Paragraph(t, s)
def C(t): return f'<font name="Courier" size="8.6">{t}</font>'
def img(name, w=16.6):
    path = os.path.join(ROOT, "docs", name)
    from PIL import Image as PI
    iw, ih = PI.open(path).size
    return Image(path, width=w * cm, height=w * cm * ih / iw)
def table(data, widths, header=True, fs=None):
    rows = [[c if not isinstance(c, str) else P(c, cellb if (header and i == 0) else cell) for c in r]
            for i, r in enumerate(data)]
    t = Table(rows, colWidths=[w * cm for w in widths], repeatRows=1 if header else 0)
    st = [("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#9ca3af")),
          ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
          ("TOPPADDING", (0, 0), (-1, -1), 2.5), ("BOTTOMPADDING", (0, 0), (-1, -1), 2.5)]
    if header:
        st.append(("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#dbeafe")))
    t.setStyle(TableStyle(st))
    return t
def readf(p, n=None):
    s = open(os.path.join(ROOT, p)).read().rstrip("\n")
    return "\n".join(s.splitlines()[:n]) if n else s

# ---------------------------------------------------------------- data
rows = list(csv.DictReader(open(os.path.join(ROOT, "results", "bench.csv"))))
def get(n, flt, ver, key):
    for r in rows:
        if int(r["lines"]) == n and r["filter"] == flt and r["version"] == ver:
            return float(r[key])
sizes = sorted({int(r["lines"]) for r in rows})
env = {l.split(":", 1)[0].strip(): l.split(":", 1)[1].strip()
       for l in open(os.path.join(ROOT, "results", "environment.txt")) if ":" in l}
ratios = [get(n, f, "pipeline", "wall_median") / get(n, f, "single", "wall_median")
          for n in sizes for f in ("WARN", "DEBUG")]
rmin, rmax = min(ratios), max(ratios)
tests = readf("results/test_results.txt").splitlines()[-1]

S = []
# ---------------------------------------------------------------- title
S += [P("Operating Systems Assignment - Topic 2", ParagraphStyle("t0", parent=body, fontSize=10, alignment=1, textColor=colors.grey)),
      P("Interprocess Communication Through a Log-Processing Pipeline",
        ParagraphStyle("t1", parent=h1, fontSize=19, leading=23, alignment=1, textColor=colors.HexColor("#111827"))),
      P("Technical report: design, tests, experiments and findings",
        ParagraphStyle("t2", parent=body, fontSize=11, alignment=1)),
      Spacer(1, 4),
      table([["Student name:", "______________________________", "Student ID:", "______________________"],
             ["Course:", "Operating Systems", "Topic:", "2 - IPC, pipes, process coordination"]],
            [2.6, 6.0, 2.2, 5.8], header=False),
      Spacer(1, 6)]

# ---------------------------------------------------------------- 1
S += [P("1. Introduction and objectives", h1),
      P("This project implements a log-processing system in which <b>separate operating-system processes</b> "
        "perform the stages of a data-processing pipeline: a <i>reader</i> process parses an application log, a "
        "<i>filter</i> process keeps events at or above a severity threshold, and a <i>summarizer</i> process "
        "produces a report of error counts. The processes communicate only through <b>POSIX pipes</b>; no data is "
        "shared through function calls or shared memory. A parent <i>coordinator</i> process creates the pipes, "
        "forks the three stages, waits for them with " + C("waitpid()") + " and supervises failures."),
      P("The assignment tasks are covered as follows: reading a timestamped log with severity levels (Section 3.1); "
        "dividing the work among three cooperating processes (3.2); transferring data over pipes (3.3); handling "
        "end-of-file, broken pipes and process failures without hanging (3.4); making sure the final report "
        "represents the input exactly (3.5, 5); and comparing against a single-process version (6). The "
        "<b>challenge</b> - restarting a failed stage without silently losing or duplicating records - is "
        "implemented and tested (Section 4)."),
      P("<b>Deliverables</b> (all in the GitHub repository): source code in " + C("src/") + ", README, test suite "
        "in " + C("tests/") + ", sample input and output in " + C("results/samples/") + ", the process communication "
        "diagram (Figure 1), error-handling tests (Section 5) and the performance comparison (Section 6).")]

# ---------------------------------------------------------------- 2
S += [P("2. Operating-system concepts used", h1),
      table([["Concept", "How it is used in this project"],
             ["Process creation: " + C("fork()"), "The coordinator forks three children. Each child has its own address space; "
              "after " + C("fork()") + " it inherits copies of all open file descriptors."],
             ["Pipe: " + C("pipe()"), "A pipe is a kernel-managed, unidirectional byte buffer (64 KiB by default on Linux). "
              "Two pipes connect reader -> filter -> summarizer."],
             ["Blocking and back-pressure", "A " + C("read()") + " on an empty pipe blocks; a " + C("write()") + " to a full pipe blocks. "
              "So a slow downstream stage automatically slows the upstream stage - no explicit flow control is needed."],
             ["End-of-file", "A reader sees EOF only when <i>every</i> write end is closed. Therefore every process closes the pipe "
              "ends it does not use, otherwise EOF would never arrive and the pipeline could hang."],
             ["Broken pipe: EPIPE / SIGPIPE", "Writing to a pipe with no reader raises SIGPIPE (default action: terminate silently). "
              "The program ignores SIGPIPE so " + C("write()") + " fails with EPIPE and the stage can log a clear message and exit with code 4."],
             [C("waitpid()") + ", exit status", "The coordinator reaps every child, decodes " + C("WIFEXITED/WIFSIGNALED") + " and reports which stage failed and how. "
              "Reaping prevents zombie processes."],
             ["Signals and timers", C("alarm()") + " + " + C("SIGALRM") + " implement a watchdog; " + C("SIGKILL") + " is sent only to PIDs the coordinator itself forked."],
             ["Atomic file update", C("write temp; fsync(); rename()") + " is atomic on POSIX file systems: readers see the old or the new file, never a mixture. "
              "Used for checkpoints and the report file."]],
            [4.3, 12.3]),
      Spacer(1, 4)]

# ---------------------------------------------------------------- 3
S += [P("3. Design", h1),
      P("3.1 Input format and parsing", h2),
      P("A line is <i>well-formed</i> if it matches " + C("YYYY-MM-DD HH:MM:SS LEVEL message") + ", the date and time are valid "
        "(real month lengths, leap years, hour &lt; 24, minute and second &lt; 60), LEVEL is one of DEBUG, INFO, WARN, ERROR, FATAL "
        "(upper case) and the message is not empty. Everything else - wrong date, unknown or lower-case level, missing message, blank "
        "line, binary NUL byte, line longer than 4096 bytes - is <b>malformed</b>. Malformed lines are never dropped silently: they "
        "travel through the pipeline as " + C("M") + " frames and appear in the report. The same parser (" + C("src/logcore.c") + ") "
        "is linked into both the pipeline and the single-process program."),
      P("3.2 Process architecture", h2),
      img("process_communication_diagram.png", 15.6),
      P("Figure 1 - Process communication diagram. Solid arrows are pipes; dashed arrows are parent-child supervision.", cap),
      P("The reader does all file I/O and parsing; the filter applies the severity rule; the summarizer owns the statistics (counts per "
        "severity, time range of kept events, top-5 messages) and writes the report. Because each pipe has exactly one writer and one "
        "reader, no locking is needed."),
      P("3.3 Data format on the pipes", h2),
      img("frame_format.png", 15.6),
      P("Figure 2 - Wire format. One text frame per line; the message is always the last field, so it may contain any character except newline.", cap),
      P("A text format was chosen deliberately: it is easy to inspect (" + C("strace") + ", " + C("tee") + ") and to test. The " + C("dropped") + " field is the key to "
        "exact accounting: the filter does not send frames for records it removes, but it tells the next frame how many valid records were "
        "removed just before it. The summarizer can therefore reconstruct the total number of lines (kept + filtered + malformed) "
        "even though most records never reach it. The trailer " + C("E") + " carries the number of frames the sender emitted; the receiver compares it with "
        "the number it received, so loss or duplication in transit is detected."),
      P("3.4 Error handling and the \"never hang\" rules", h2),
      table([["Situation", "Detection", "Outcome"],
             ["Input file missing / unreadable", C("access()") + " in the parent, " + C("fopen()") + " in the reader", "Clear message and exit code 2 from the parent before any stage is started"],
             ["Downstream stage dies (broken pipe)", C("write()") + " returns EPIPE (SIGPIPE ignored)", "Stage logs \"downstream stage is gone\", exits 4; the failure ripples upstream"],
             ["Upstream stage dies", C("read()") + " returns EOF with no " + C("E") + " trailer", "Stage hands over what it processed, logs \"without an end marker\", exits 3"],
             ["Corrupt frame / count mismatch / seq out of order", "Frame parser, trailer check, monotonic seq check", "Exit 3; no report is printed"],
             ["A stage hangs (infinite loop, blocked)", "Watchdog " + C("--timeout") + " and a 5 s grace timer after the first failure", "SIGKILL to the coordinator's own children only; exit 1"],
             ["Unused pipe ends left open", "Prevented by design: every process closes unused ends, the parent closes all four after forking", "EOF is always delivered"],
             ["Corrupt checkpoint", "Header and END marker validated on load", "Summarizer fails with exit 2; coordinator exits 1, never uses bad state"]],
            [4.3, 5.9, 6.4]),
      Spacer(1, 3),
      P("The coordinator never kills unrelated processes: " + C("kill()") + " is only called with PIDs returned by its own " + C("fork()") + " calls."),
      P("3.5 Single-process version", h2),
      P(C("logsingle") + " performs the same read -> parse -> filter -> summarize sequence in one process with ordinary function calls, using the same "
        "parser and report code. Its report is required to be <b>byte-identical</b> to the pipeline's; the test suite and the benchmark both assert this.")]

# ---------------------------------------------------------------- 4
S += [P("4. Challenge: restarting a failed stage without loss or duplication", h1),
      img("restart_design.png", 15.6),
      P("Figure 3 - The summarizer commits its counters and its position (" + C("last_seq") + ") in one atomic file replacement.", cap),
      P("The coordinator runs the pipeline in a loop (" + C("--max-restarts N") + "). On every restart it reads " + C("last_seq") + " from the checkpoint and tells the "
        "reader to skip lines with " + C("seq <= last_seq") + ". The summarizer loads the saved counters, and as a second line of defence ignores any frame with "
        + C("seq <= last_seq") + " (counted as a duplicate; the count is 0 in the run shown below). Because counters and position live in the <b>same</b> atomically renamed "
        "file, they can never disagree: anything after the last checkpoint is recomputed exactly once, anything before it is never recomputed. "
        "When the upstream dies, the summarizer also commits its progress before exiting so less work is repeated. After success the checkpoint is deleted so a "
        "later run on a changed file cannot use stale state."),
      P("Evidence (genuine output, " + C("results/samples/restart_summarizer_killed.txt") + "): the summarizer is killed with SIGKILL after 30,001 frames; "
        "attempt 1 resumes at line 111786 and the final report is byte-identical to the single-process report for the same 200,000-line file.")]
S += [Preformatted("\n".join(re.sub(r"^\[[0-9:.]+\] ", "", l)[:125] for l in readf("results/samples/restart_summarizer_killed.txt").splitlines()
                              if any(k in l for k in ("INJECTED", "FAILURE", "restarting", "attempt 1", "resumed", "done:", "RESULT")))
                   , code)]

# ---------------------------------------------------------------- 5
S += [P("5. Testing", h1),
      P("The suite (" + C("tests/run_tests.sh") + ", run with " + C("make test") + ") contains 52 automated checks. Correctness is judged against two independent references: a "
        "<b>hand-computed</b> report for a 15-line log, and a <b>Python oracle</b> (" + C("tests/oracle.py") + ") that re-implements the specification with regular "
        "expressions and shares no code with the C programs. Final line of the recorded run: <b>" + tests + "</b>."),
      table([["Category", "Checks", "What is verified"],
             ["Normal", "4", "Hand-computed report (pipeline and single); 200,000-line random log (2% malformed): pipeline = single = Python oracle, byte for byte"],
             ["Malformed entries", "6", "One file with 12 different defects (bad month/day/hour, unknown and lower-case level, empty message, blank line, NUL byte, 5000-char line, short timestamp, "
              "blank message) plus a leap-day line, a 300-char message and a final line without newline. Exactly 12 malformed, 5 valid, 17 lines scanned"],
             ["Boundary", "12", "Empty file, one-line file, only-malformed file, all five severity thresholds, missing input file (exit 2), invalid level, atomic " + C("-o") + " output"],
             ["Failure scenarios", "16", "Each stage killed (SIGKILL) and each stage " + C("exit(7)") + ": pipeline ends within seconds with exit 1 and prints no report; broken-pipe and truncated-stream messages; "
              "hung stage ended by the watchdog after 2 s; corrupt checkpoint"],
             ["Restart", "14", "Each stage killed mid-run and restarted: report identical to single-process, 2 attempts, checkpoint cleaned up; fault on every attempt gives up after the restart limit"]],
            [3.0, 1.5, 12.1]),
      Spacer(1, 3),
      P("Selected genuine terminal output (killed summarizer: the failure propagates upstream through EPIPE and the coordinator reports every stage's fate):"),
      Preformatted("\n".join(re.sub(r"^\[[0-9:.]+\] ", "", l)[:125] for l in readf("results/samples/failure_summarizer_killed.txt").splitlines()
                              if "forked" not in l and "attempt" not in l), code),
      P("Process hierarchy observed with " + C("ps -eo pid,ppid,stat,etimes,comm,args --forest") + " during a 3-million-line run (" + C("pstree") + " was not installed on the test machine; "
        "the forest view shows the same tree). The coordinator (PID 1502) has three children: two are running (R) and one is sleeping (S) on a pipe, as expected for a pipeline in which the stages hand data to each other:"),
      Preformatted(readf("results/samples/process_tree_ps.txt"), code)]

# ---------------------------------------------------------------- 6
S += [P("6. Experimental investigation: does adding processes help?", h1),
      P("<b>Environment.</b> CPU: " + env["CPU"] + "; logical CPUs: <b>" + env["Logical CPUs"].split()[0] + "</b>; memory " + env["Memory"] + "; kernel " + env["Kernel"] + "; " + env["OS"] + "; " + env["Compiler"] +
        "; flags " + C("-O2 -Wall -Wextra -Wpedantic -std=gnu11") + ". Input: synthetic logs from " + C("loggen") + " (seed 42, 2% malformed), page cache warmed, 3-15 repetitions per cell "
        "(more for small inputs), median reported. Wall time is measured with " + C("perf_counter") + "; CPU time (user+sys of the process and its reaped children) with " + C("wait4()") + " rusage. "
        "Before timing every configuration, the script asserts that both programs print the same report. Two workloads: " + C("-l WARN") + " (about 27% of records pass the filter) "
        "and " + C("-l DEBUG") + " (every record crosses both pipes)."),
      ]
hdr = ["Lines", "Size (MB)", "Filter", "Single wall (s)", "Pipeline wall (s)", "Pipeline / single", "Single CPU (s)", "Pipeline CPU (s)"]
data = [hdr]
for n in sizes:
    for f in ("WARN", "DEBUG"):
        s, p = get(n, f, "single", "wall_median"), get(n, f, "pipeline", "wall_median")
        sm = [r for r in rows if int(r["lines"]) == n][0]["size_mb"]
        data.append([f"{n:,}", sm, f, f"{s:.3f}", f"{p:.3f}", f"{p/s:.1f}x", f"{get(n,f,'single','cpu_median'):.3f}", f"{get(n,f,'pipeline','cpu_median'):.3f}"])
S += [KeepTogether([table(data, [1.9, 1.7, 1.5, 2.3, 2.4, 2.3, 2.2, 2.3]),
                    P("Table 1 - Median wall-clock and CPU time (full data in results/bench.csv).", cap)]),
      img("performance.png", 16.0),
      P("Figure 4 - Left: time against input size (log-log). Right: pipeline time divided by single-process time. All points are above 1, i.e. the pipeline is slower.", cap),
      P("6.1 Findings", h2),
      P(f"<b>1. The pipeline was slower in every measurement, by {rmin:.1f}x to {rmax:.1f}x.</b> Both versions grow roughly linearly with input size (process start-up matters only at 10,000 lines), so "
        "the gap is a per-record factor, not a fixed start-up cost. Run-to-run noise is noticeable (for example the 1M-line WARN pipeline runs ranged from 0.52 s to 0.85 s), so the exact ratio "
        "per cell should not be over-interpreted; the conclusion \"slower by a factor of roughly 3-4\" is robust."),
      P("<b>2. Why no speed-up here: the machine has one CPU.</b> The three processes cannot run in parallel; the scheduler interleaves them. CPU time is almost equal to wall time for every "
        "run (Table 1), confirming that the processes were never overlapped. Additional processes can only help when there are idle cores <i>and</i> the work per record is larger "
        "than the cost of moving the record."),
      P("<b>3. Where the extra time goes.</b> A measurement with " + C("wait4()") + " rusage (1,000,000 lines, " + C("-l DEBUG") + ", median of 7 runs, " + C("results/context_switches.txt") + "):"),
      table([["Version", "User CPU", "System CPU", "Voluntary ctx switches", "Involuntary ctx switches"],
             ["single", "0.185 s", "0.004 s", "1", "8"],
             ["pipeline (3 processes)", "0.925 s", "0.037 s", "2,773", "762"]], [4.0, 2.6, 2.6, 3.8, 3.6]),
      Spacer(1, 4),
      P("My initial expectation was that kernel overhead (system calls and context switches) would dominate. The data does not support that: system time is only about 4% of the pipeline's CPU. "
        "<b>About 95% of the cost is user-space CPU</b>, a five-fold increase. The explanation consistent with the design is serialization: the single process parses each line once, "
        "whereas the pipeline formats the record into a text frame (reader), parses the frame and formats it again (filter) and parses it a third time (summarizer). The "
        "per-record work of the real task (validate, compare level, increment a counter) is only about 185 ns, so the cost of encoding and decoding the record is several times larger than the "
        "work itself. The context switches (about 3,500 versus 9) are the visible price of the blocking pipe hand-offs, but they cost far less than the serialization."),
      P("<b>4. Effect of the filter.</b> With " + C("-l DEBUG") + " every record crosses both pipes and enters the summarizer's hash table; the single process gets only 3% slower at 1M lines (0.186 s against 0.180 s) and 15% slower at 3M lines (0.730 s against 0.634 s), "
        "whereas the pipeline gets 20% slower at 1M lines (0.809 s against 0.672 s) and 47% slower at 3M lines (2.587 s against 1.760 s). IPC volume is therefore a cost that is paid mostly by the pipeline."),
      P("<b>5. Would more cores help?</b> This was <i>not measured</i> (only one CPU was available) and the following is an estimate. Even with three free cores, the pipeline runs at the speed of its slowest stage. "
        "The reader alone does parsing plus frame formatting; from the CPU figures above it plausibly takes a good fraction of the 0.9 s total, which is likely still more than the 0.185 s "
        "the single process needs for the whole job. So for this light per-record work I expect the pipeline to stay slower even on a multi-core machine. The design would pay off when "
        "the per-record work is heavy (regex matching, DNS look-ups, compression, writing to a database) or when stages must be isolated for fault tolerance. The benchmark script " + C("make bench") + " can be re-run on a multi-core computer to test this."),
      P("<b>6. What the extra processes do buy.</b> Fault isolation and recoverability: a crash in one stage is detected, reported precisely and, with checkpoints, recovered from without losing or repeating a record (Section 4) - "
        "something the single process cannot offer, since one crash loses everything.")]

# ---------------------------------------------------------------- 7
S += [P("7. Reflection", h1),
      P("<b>Limitation.</b> The text wire format makes every stage re-parse every record, which is why the pipeline is 3-4x slower than the single process on the measured workloads; "
        "the pipeline spends its CPU on encoding, not on analysis. A second, smaller limitation is that restart identifies records by line number, so it assumes the input file is not "
        "modified between attempts."),
      P("<b>Improvement.</b> Replace the per-record text frames with batched binary frames (for example 4,096 records per " + C("write()") + " with a length prefix and a fixed-size record struct, "
        "so the filter reads the level byte without parsing text). That removes two of the three text parsing passes and most system calls. A follow-up improvement would store the input file's "
        "size and modification time in the checkpoint and refuse to resume if they changed."),
      P("8. Conclusion", h1),
      P("The system demonstrates real interprocess communication with " + C("fork()") + ", " + C("pipe()") + " and " + C("waitpid()") + ", a defined data format with end-of-stream verification, "
        "and failure handling that never hangs: killed, exiting or hung stages are all detected within seconds. All 52 tests pass, the pipeline's report is byte-identical to the single-process version and to an independent "
        "oracle (including malformed input), and a failed stage can be restarted without losing or duplicating records. The experiments show an honest and instructive result: more processes do <b>not</b> "
        "automatically mean more speed - when the per-record work is small, IPC and serialization cost dominate, and on a single CPU nothing can run in parallel."),
      P("Appendix A - Reproducing the results", h2),
      Preformatted("git clone <your-repository-url> && cd log-pipeline-ipc\n"
                   "make                     # build\n"
                   "make test                # 52 checks, writes nothing outside /tmp\n"
                   "./bin/loggen -n 100000 -s 7 -m 20 -o app.log\n"
                   "./bin/logpipe app.log                                   # pipeline report + lifecycle log\n"
                   "./bin/logpipe --inject summarizer:kill:100 app.log      # failure demo (exit 1, no hang)\n"
                   "./bin/logpipe -c ck.dat -r 2 --inject filter:kill:5000 app.log   # restart demo\n"
                   "make bench               # performance comparison -> results/bench.csv\n"
                   "python3 scripts/make_figures.py && python3 scripts/make_report.py", code)]

def footer(c, d):
    c.saveState(); c.setFont("Helvetica", 8); c.setFillColor(colors.grey)
    c.drawString(2 * cm, 1.2 * cm, "Topic 2 - IPC through a log-processing pipeline")
    c.drawRightString(A4[0] - 2 * cm, 1.2 * cm, f"Page {d.page}")
    c.restoreState()

doc = SimpleDocTemplate(OUT, pagesize=A4, leftMargin=2 * cm, rightMargin=2 * cm, topMargin=1.7 * cm,
                        bottomMargin=1.9 * cm, title="Topic 2 - IPC Log Pipeline Report", author="")
doc.build(S, onFirstPage=footer, onLaterPages=footer)
print("wrote", OUT)
