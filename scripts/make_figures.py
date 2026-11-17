#!/usr/bin/env python3
"""Create the diagrams and graphs used in the report (needs matplotlib)."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs")
os.makedirs(OUT, exist_ok=True)

def box(ax, x, y, w, h, text, fc="#dbeafe", ec="#1e3a8a", fs=9, bold=False):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.02,rounding_size=0.08",
                                fc=fc, ec=ec, lw=1.4))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=fs,
            fontweight="bold" if bold else "normal")

def arrow(ax, p, q, text="", color="#111", style="-|>", ls="-", off=(0, 0.12), fs=8):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle=style, mutation_scale=14, lw=1.5, color=color, linestyle=ls))
    if text:
        ax.text((p[0] + q[0]) / 2 + off[0], (p[1] + q[1]) / 2 + off[1], text, ha="center",
                va="bottom", fontsize=fs, color=color)

# ---------------------------------------------------------------- diagram 1
fig, ax = plt.subplots(figsize=(10, 5.2))
ax.set_xlim(0, 10); ax.set_ylim(0, 5.4); ax.axis("off")
box(ax, 3.2, 4.2, 3.6, 0.9, "Parent / coordinator\nfork() x3, waitpid(), watchdog, restart", "#fef3c7", "#92400e", 9, True)
box(ax, 0.2, 1.7, 2.3, 1.3, "Stage 1: READER\nread file, validate,\nparse line -> frame", fs=9)
box(ax, 3.85, 1.7, 2.3, 1.3, "Stage 2: FILTER\nkeep severity >=\nthreshold", fs=9)
box(ax, 7.5, 1.7, 2.3, 1.3, "Stage 3: SUMMARIZER\ncount, rank messages,\nwrite report", fs=9)
arrow(ax, (2.5, 2.35), (3.85, 2.35), "pipe 1\nR / M / E frames", "#065f46", off=(0, 0.1))
arrow(ax, (6.15, 2.35), (7.5, 2.35), "pipe 2\nR / M / E frames", "#065f46", off=(0, 0.1))
for x in (1.35, 5.0, 7.95):
    arrow(ax, (5.0 if x == 5.0 else 5.0 + (x - 5.0) * 0.35, 4.2), (x, 3.0), "", "#92400e", ls="--")
ax.text(1.55, 3.55, "fork + waitpid", fontsize=8, color="#92400e")
box(ax, 0.2, 0.2, 2.3, 0.7, "Input log file\n(disk)", "#f3f4f6", "#374151", 8)
arrow(ax, (1.35, 0.9), (1.35, 1.7), "read", "#374151", off=(0.25, -0.1))
box(ax, 7.5, 0.2, 2.3, 0.7, "Report (stdout / -o file)", "#f3f4f6", "#374151", 8)
arrow(ax, (8.65, 1.7), (8.65, 0.9), "write", "#374151", off=(0.3, -0.1))
box(ax, 7.5, 3.5, 2.3, 0.7, "Checkpoint file\n(counts + last_seq)", "#fce7f3", "#9d174d", 8)
arrow(ax, (8.65, 3.0), (8.65, 3.5), "atomic rename", "#9d174d", off=(0.55, -0.1))
ax.text(5.0, 0.55, "Pipe EOF / EPIPE propagate failures: a dead stage closes its pipe ends,\n"
        "neighbours see EOF (downstream) or EPIPE (upstream) and exit instead of hanging.",
        ha="center", fontsize=8.5, style="italic", color="#444")
ax.set_title("Process communication diagram: three cooperating processes joined by two pipes", fontsize=11)
fig.tight_layout(); fig.savefig(f"{OUT}/process_communication_diagram.png", dpi=170); plt.close(fig)

# ---------------------------------------------------------------- diagram 2 : frame format & accounting
fig, ax = plt.subplots(figsize=(10, 3.6))
ax.set_xlim(0, 10); ax.set_ylim(0, 3.6); ax.axis("off")
rows = [("R <seq> <dropped> <epoch> <LEVEL> <message...>", "valid record (kept by filter)"),
        ("M <seq> <dropped>", "malformed input line (counted, never lost)"),
        ("E <last_seq> <dropped> <frames>", "end-of-stream trailer: receiver verifies the frame count")]
for i, (f, d) in enumerate(rows):
    y = 2.6 - i * 0.9
    box(ax, 0.2, y, 5.6, 0.62, f, "#ecfccb", "#3f6212", 9, True)
    ax.text(6.0, y + 0.31, d, va="center", fontsize=9)
ax.text(0.2, 0.15, "seq = input line number (unique, strictly increasing)   dropped = valid records the filter removed just before this frame\n"
        "No 'E' trailer before EOF  =>  upstream died  =>  stage exits 3 (never prints a partial report as if it were complete).",
        fontsize=8.5, color="#333")
ax.set_title("Wire format used on both pipes (one text frame per line)", fontsize=11)
fig.tight_layout(); fig.savefig(f"{OUT}/frame_format.png", dpi=170); plt.close(fig)

# ---------------------------------------------------------------- diagram 3 : restart
fig, ax = plt.subplots(figsize=(10, 3.9))
ax.set_xlim(0, 10); ax.set_ylim(0, 3.9); ax.axis("off")
ax.annotate("", xy=(9.6, 2.9), xytext=(0.3, 2.9), arrowprops=dict(arrowstyle="-|>", lw=1.5))
ax.text(0.3, 3.15, "input lines (seq)", fontsize=9)
ax.add_patch(plt.Rectangle((0.3, 2.55), 4.3, 0.28, fc="#bbf7d0", ec="#166534")); ax.text(2.45, 2.69, "committed (in checkpoint)", ha="center", va="center", fontsize=8)
ax.add_patch(plt.Rectangle((4.6, 2.55), 1.4, 0.28, fc="#fecaca", ec="#991b1b")); ax.text(5.3, 2.69, "lost in crash", ha="center", va="center", fontsize=8)
ax.add_patch(plt.Rectangle((6.0, 2.55), 3.6, 0.28, fc="#e5e7eb", ec="#374151")); ax.text(7.8, 2.69, "not read yet", ha="center", va="center", fontsize=8)
ax.plot([4.6, 4.6], [2.3, 3.2], color="#166534", lw=2); ax.text(4.6, 2.05, "checkpoint.last_seq", ha="center", fontsize=8, color="#166534")
ax.plot([6.0, 6.0], [2.3, 3.2], color="#991b1b", lw=2, ls="--"); ax.text(6.0, 2.05, "crash", ha="center", fontsize=8, color="#991b1b")
steps = ["1  Summarizer saves (counts + last_seq) with write-temp, fsync, rename().  The rename is atomic: old OR new file, never half.",
         "2  Crash: records after last_seq exist only in memory/pipes and are discarded with the failed pipeline.",
         "3  Restart: reader skips every line with seq <= last_seq; summarizer also ignores seq <= last_seq (defence in depth).",
         "4  Result: every line is counted exactly once -> no loss, no duplication."]
for i, s in enumerate(steps):
    ax.text(0.3, 1.5 - i * 0.37, s, fontsize=8.3)
ax.set_title("Restart challenge: state and position are committed together", fontsize=11)
fig.tight_layout(); fig.savefig(f"{OUT}/restart_design.png", dpi=170); plt.close(fig)

# ---------------------------------------------------------------- benchmark graphs
rows = list(csv.DictReader(open(f"{ROOT}/results/bench.csv")))
def series(ver, flt, key="wall_median"):
    r = sorted((x for x in rows if x["version"] == ver and x["filter"] == flt), key=lambda x: int(x["lines"]))
    return [int(x["lines"]) for x in r], [float(x[key]) for x in r]

fig, axs = plt.subplots(1, 2, figsize=(10, 4))
for flt, ls in (("WARN", "-"), ("DEBUG", "--")):
    for ver, c in (("single", "#1d4ed8"), ("pipeline", "#dc2626")):
        xs, ys = series(ver, flt)
        axs[0].plot(xs, ys, ls, color=c, marker="o", label=f"{ver} (-l {flt})")
axs[0].set_xscale("log"); axs[0].set_yscale("log")
axs[0].set_xlabel("input size (log lines)"); axs[0].set_ylabel("wall-clock time, median (s)")
axs[0].set_title("Execution time vs input size"); axs[0].grid(True, which="both", alpha=.3); axs[0].legend(fontsize=8)
for flt, c in (("WARN", "#7c3aed"), ("DEBUG", "#ea580c")):
    xs, a = series("single", flt); _, b = series("pipeline", flt)
    axs[1].plot(xs, [q / p for p, q in zip(a, b)], marker="s", color=c, label=f"-l {flt}")
axs[1].axhline(1.0, color="k", lw=1, ls=":"); axs[1].text(1.2e4, 1.08, "break-even (speedup = 1)", fontsize=8)
axs[1].set_xscale("log"); axs[1].set_xlabel("input size (log lines)"); axs[1].set_ylabel("pipeline time / single time")
axs[1].set_title("Slowdown of the pipeline (>1 = slower)"); axs[1].grid(True, which="both", alpha=.3); axs[1].legend(fontsize=8)
fig.tight_layout(); fig.savefig(f"{OUT}/performance.png", dpi=170); plt.close(fig)
print("figures written to", OUT)
