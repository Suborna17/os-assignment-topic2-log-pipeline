/*
 * logpipe.c - Multi-process log-processing pipeline using POSIX pipes.
 *
 *   parent (coordinator)
 *     |-- fork --> [1] reader      : file -> parse/validate -> frames  --pipe1-->
 *     |-- fork --> [2] filter      : frames -> severity filter -> frames --pipe2-->
 *     '-- fork --> [3] summarizer  : frames -> counts -> final report
 *
 * WIRE FORMAT (text, one frame per line, '\n' terminated):
 *   R <seq> <dropped> <epoch> <LEVEL> <message...>   valid record
 *   M <seq> <dropped>                                malformed input line
 *   E <last_seq> <dropped> <frames>                  end-of-stream trailer
 *
 *   seq      line number in the input file (unique, strictly increasing)
 *   dropped  number of valid records the filter removed immediately BEFORE
 *            this frame (so removed records are still accounted for)
 *   frames   number of R/M frames the sender emitted in this run
 * The receiver checks the trailer: no trailer => truncated stream (upstream
 * died); frame count mismatch => lost/duplicated data. Either way the stage
 * exits non-zero instead of printing a wrong report.
 *
 * NO-HANG DESIGN
 *   - SIGPIPE is ignored, so a write to a dead reader returns EPIPE.
 *   - Every pipe end is closed in every process that does not use it, so a
 *     dying stage always produces EOF (downstream) or EPIPE (upstream).
 *   - An optional watchdog (--timeout) and a grace timer after the first
 *     failure SIGKILL any surviving child of THIS program (never other PIDs).
 *
 * RESTART CHALLENGE (--checkpoint FILE --max-restarts N)
 *   The summarizer atomically saves (counts + last_seq) to the checkpoint.
 *   After a failure the coordinator restarts the pipeline; the reader skips
 *   every line with seq <= checkpoint.last_seq, and the summarizer also
 *   ignores any frame with seq <= last_seq. State and position are committed
 *   in the same file rename, so no record is lost and none is counted twice.
 */
#define _GNU_SOURCE
#include "logcore.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ST_READER, ST_FILTER, ST_SUMM, NSTAGES };
static const char *STAGE_NAME[NSTAGES] = { "reader", "filter", "summarizer" };

enum { INJ_NONE, INJ_KILL, INJ_EXIT, INJ_HANG };
enum { RC_OK = 0, RC_IO = 2, RC_PROTO = 3, RC_PIPE = 4 };   /* stage exit codes */

#define GRACE_SECONDS 5

static struct {
    const char *input, *outpath, *ckpt;
    int   min_level, quiet, max_restarts, timeout;
    long  ckpt_every;
    int   inj_stage, inj_mode, inj_always;
    long  inj_after;
} cfg = { NULL, NULL, NULL, LV_WARN, 0, 0, 0, 1000, -1, INJ_NONE, 0, 0 };

static int g_attempt = 0;
static volatile sig_atomic_t g_alarm = 0;

/* ------------------------------------------------------------------ */
/* Logging (all diagnostics go to stderr; the report goes to stdout)  */
/* ------------------------------------------------------------------ */

static void vlog(int always, const char *who, const char *fmt, va_list ap)
{
    if (!always && cfg.quiet) return;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld] [%-10s pid=%d] %s\n", tm.tm_hour, tm.tm_min,
            tm.tm_sec, ts.tv_nsec / 1000000, who, (int)getpid(), buf);
}
static void log_info(const char *who, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vlog(0, who, fmt, ap); va_end(ap); }
static void log_err(const char *who, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vlog(1, who, fmt, ap); va_end(ap); }

/* ------------------------------------------------------------------ */
/* Fault injection (for the failure tests)                            */
/* ------------------------------------------------------------------ */

static void maybe_inject(int stage, long done)
{
    if (cfg.inj_stage != stage || cfg.inj_mode == INJ_NONE) return;
    if (g_attempt > 0 && !cfg.inj_always) return;      /* fail only once */
    if (done != cfg.inj_after) return;
    log_err(STAGE_NAME[stage], "INJECTED FAULT after %ld frames", done);
    fflush(NULL);
    if (cfg.inj_mode == INJ_KILL) raise(SIGKILL);
    if (cfg.inj_mode == INJ_EXIT) _exit(7);
    for (;;) pause();                                   /* INJ_HANG */
}

/* ------------------------------------------------------------------ */
/* Frame parsing                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char type;
    long seq, dropped, frames;
    long long epoch;
    int level;
    const char *msg;
} frame_t;

enum { FR_BAD = -1, FR_EOF = 0, FR_OK = 1 };

static int parse_frame(char *line, frame_t *f)
{
    char *p = line, *end;
    memset(f, 0, sizeof *f);
    f->type = p[0];
    if ((f->type != 'R' && f->type != 'M' && f->type != 'E') || p[1] != ' ') return -1;
    p += 2;
    f->seq = strtol(p, &end, 10);
    if (end == p || *end != ' ') return -1;
    p = end + 1;
    f->dropped = strtol(p, &end, 10);
    if (end == p || f->seq < 0 || f->dropped < 0) return -1;
    if (f->type == 'M') return *end == '\0' ? 0 : -1;
    if (*end != ' ') return -1;
    p = end + 1;
    if (f->type == 'E') {
        f->frames = strtol(p, &end, 10);
        return (end != p && *end == '\0' && f->frames >= 0) ? 0 : -1;
    }
    f->epoch = strtoll(p, &end, 10);                       /* 'R' */
    if (end == p || *end != ' ') return -1;
    p = end + 1;
    char *sp = strchr(p, ' ');
    if (!sp) return -1;
    f->level = level_from_name(p, (size_t)(sp - p));
    if (f->level < 0 || sp[1] == '\0') return -1;
    f->msg = sp + 1;
    return 0;
}

static int read_frame(FILE *in, char **buf, size_t *cap, frame_t *f)
{
    ssize_t n = getline(buf, cap, in);
    if (n < 0) return FR_EOF;
    if (n == 0 || (*buf)[n - 1] != '\n') return FR_BAD;   /* cut off mid-frame */
    (*buf)[n - 1] = '\0';
    return parse_frame(*buf, f) == 0 ? FR_OK : FR_BAD;
}

static int out_failed(FILE *out, const char *who)
{
    if (ferror(out)) {
        log_err(who, "write to downstream failed: %s (downstream stage is gone)",
                errno ? strerror(errno) : "I/O error");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Stage 1: reader                                                    */
/* ------------------------------------------------------------------ */

static int run_reader(FILE *out, long resume_seq)
{
    const char *who = "reader";
    FILE *in = fopen(cfg.input, "r");
    if (!in) { log_err(who, "cannot open '%s': %s", cfg.input, strerror(errno)); return RC_IO; }

    char *line = NULL; size_t cap = 0; ssize_t n;
    long seq = 0, frames = 0, bad = 0;

    while (1) {
        maybe_inject(ST_READER, frames);
        if ((n = getline(&line, &cap, in)) == -1) break;
        seq++;
        if (seq <= resume_seq) continue;                  /* already committed */

        size_t len = (size_t)n;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;

        record_t r;
        if (n > LINE_MAX_LEN || memchr(line, '\0', (size_t)n) ||
            parse_log_line(line, len, &r) != 0) {
            fprintf(out, "M %ld 0\n", seq);
            bad++;
        } else {
            fprintf(out, "R %ld 0 %lld %s %s\n", seq, (long long)r.epoch,
                    level_name(r.level), r.msg);
        }
        frames++;
        if (out_failed(out, who)) { free(line); fclose(in); return RC_PIPE; }
    }
    if (ferror(in)) { log_err(who, "read error: %s", strerror(errno)); return RC_IO; }
    fclose(in);
    free(line);

    fprintf(out, "E %ld 0 %ld\n", seq, frames);
    if (fflush(out) != 0 || out_failed(out, who)) return RC_PIPE;
    log_info(who, "done: %ld lines scanned, %ld frames sent (%ld malformed)", seq, frames, bad);
    return RC_OK;
}

/* ------------------------------------------------------------------ */
/* Stage 2: filter                                                    */
/* ------------------------------------------------------------------ */

static int run_filter(FILE *in, FILE *out)
{
    const char *who = "filter";
    char *buf = NULL; size_t cap = 0; frame_t f;
    long frames_in = 0, frames_out = 0, pending = 0, prev = 0, removed = 0;
    int rc = RC_OK;

    while (1) {
        maybe_inject(ST_FILTER, frames_in);
        int r = read_frame(in, &buf, &cap, &f);
        if (r == FR_EOF) {
            log_err(who, "upstream closed the pipe without an end marker (reader died?)");
            rc = RC_PROTO; break;
        }
        if (r == FR_BAD) { log_err(who, "malformed frame from upstream"); rc = RC_PROTO; break; }

        if (f.type == 'E') {
            if (f.frames != frames_in) {
                log_err(who, "frame count mismatch: trailer says %ld, received %ld",
                        f.frames, frames_in);
                rc = RC_PROTO; break;
            }
            fprintf(out, "E %ld %ld %ld\n", f.seq, pending, frames_out);
            if (fflush(out) != 0 || out_failed(out, who)) rc = RC_PIPE;
            else log_info(who, "done: %ld frames in, %ld forwarded, %ld removed",
                          frames_in, frames_out, removed);
            free(buf);
            return rc;
        }
        if (f.seq <= prev || f.dropped != 0) {
            log_err(who, "protocol violation (seq %ld after %ld)", f.seq, prev);
            rc = RC_PROTO; break;
        }
        prev = f.seq;
        frames_in++;

        if (f.type == 'M') {
            fprintf(out, "M %ld %ld\n", f.seq, pending);
            pending = 0; frames_out++;
        } else if (f.level >= cfg.min_level) {
            fprintf(out, "R %ld %ld %lld %s %s\n", f.seq, pending, f.epoch,
                    level_name(f.level), f.msg);
            pending = 0; frames_out++;
        } else {
            pending++; removed++;                      /* reported in next frame */
        }
        if (out_failed(out, who)) { rc = RC_PIPE; break; }
    }
    fflush(out);              /* hand over what we already processed */
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Stage 3: summarizer                                                */
/* ------------------------------------------------------------------ */

static void commit(summary_t *s)
{
    if (cfg.ckpt && summary_save(s, cfg.ckpt) != 0)
        log_err("summarizer", "checkpoint write failed: %s", strerror(errno));
}

static int run_summarizer(FILE *in)
{
    const char *who = "summarizer";
    summary_t s;
    summary_init(&s);
    int lr = cfg.ckpt ? summary_load(&s, cfg.ckpt) : 1;
    if (lr < 0) { log_err(who, "checkpoint '%s' is corrupt", cfg.ckpt); return RC_IO; }
    if (lr == 0) log_info(who, "resumed from checkpoint at line %ld (%ld records)",
                          s.last_seq, s.valid + s.malformed);

    char *buf = NULL; size_t cap = 0; frame_t f;
    long frames_in = 0, dups = 0;
    int rc = RC_OK;

    while (1) {
        maybe_inject(ST_SUMM, frames_in);
        int r = read_frame(in, &buf, &cap, &f);
        if (r == FR_EOF) {
            log_err(who, "upstream closed the pipe without an end marker (filter died?)");
            rc = RC_PROTO; break;
        }
        if (r == FR_BAD) { log_err(who, "malformed frame from upstream"); rc = RC_PROTO; break; }

        if (f.type == 'E') {
            if (f.frames != frames_in) {
                log_err(who, "frame count mismatch: trailer says %ld, received %ld",
                        f.frames, frames_in);
                rc = RC_PROTO; break;
            }
            summary_add_dropped(&s, f.dropped);
            if (f.seq > s.last_seq) s.last_seq = f.seq;
            commit(&s);

            FILE *out = stdout;
            char tmp[4096];
            if (cfg.outpath) {
                snprintf(tmp, sizeof tmp, "%s.tmp", cfg.outpath);
                if (!(out = fopen(tmp, "w"))) {
                    log_err(who, "cannot write report: %s", strerror(errno));
                    summary_free(&s); free(buf); return RC_IO;
                }
            }
            summary_report(&s, out, cfg.min_level);
            int bad = fflush(out) != 0;
            if (out != stdout) {
                bad |= fclose(out) != 0;
                bad |= rename(tmp, cfg.outpath) != 0;
            }
            if (bad) { log_err(who, "report write failed"); rc = RC_IO; }
            else log_info(who, "done: %ld lines accounted for (%ld kept, %ld filtered, "
                          "%ld malformed, %ld duplicates ignored)",
                          s.valid + s.malformed, s.kept, s.dropped, s.malformed, dups);
            summary_free(&s); free(buf);
            return rc;
        }

        frames_in++;
        if (f.seq <= s.last_seq) { dups++; continue; }    /* already committed */
        summary_add_dropped(&s, f.dropped);
        if (f.type == 'M') {
            summary_add_malformed(&s);
        } else {
            record_t rec = { .seq = f.seq, .epoch = (time_t)f.epoch, .level = f.level };
            snprintf(rec.msg, sizeof rec.msg, "%s", f.msg);
            summary_add_record(&s, &rec);
        }
        s.last_seq = f.seq;
        if (cfg.ckpt && cfg.ckpt_every > 0 && frames_in % cfg.ckpt_every == 0) commit(&s);
    }
    commit(&s);                       /* keep the progress made so far */
    summary_free(&s);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Coordinator                                                        */
/* ------------------------------------------------------------------ */

static void on_alarm(int sig) { (void)sig; g_alarm = 1; }

static long checkpoint_position(void)
{
    if (!cfg.ckpt) return 0;
    summary_t s;
    int r = summary_load(&s, cfg.ckpt);
    if (r == 0) { long p = s.last_seq; summary_free(&s); return p; }
    if (r < 0) { log_err("parent", "checkpoint '%s' unreadable; starting from scratch", cfg.ckpt); }
    return 0;
}

static void child_main(int stage, int p1[2], int p2[2], long resume)
{
    int rc;
    if (stage == ST_READER) {
        close(p1[0]); close(p2[0]); close(p2[1]);
        FILE *out = fdopen(p1[1], "w");
        rc = out ? run_reader(out, resume) : RC_IO;
        if (out) fclose(out);
    } else if (stage == ST_FILTER) {
        close(p1[1]); close(p2[0]);
        FILE *in = fdopen(p1[0], "r"), *out = fdopen(p2[1], "w");
        rc = (in && out) ? run_filter(in, out) : RC_IO;
        if (out) fclose(out);
        if (in) fclose(in);
    } else {
        close(p1[0]); close(p1[1]); close(p2[1]);
        FILE *in = fdopen(p2[0], "r");
        rc = in ? run_summarizer(in) : RC_IO;
        if (in) fclose(in);
    }
    fflush(NULL);
    _exit(rc);
}

static void describe(int status, char *buf, size_t n)
{
    if (WIFEXITED(status)) snprintf(buf, n, "exited with code %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(buf, n, "killed by signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
    else snprintf(buf, n, "status 0x%x", status);
}

/* Returns 0 if every stage succeeded, 1 otherwise. */
static int run_attempt(int attempt)
{
    g_attempt = attempt;
    long resume = checkpoint_position();
    log_info("parent", "attempt %d: starting pipeline (resume after line %ld)", attempt, resume);

    int p1[2], p2[2];
    if (pipe(p1) != 0 || pipe(p2) != 0) { log_err("parent", "pipe: %s", strerror(errno)); return 1; }

    pid_t pid[NSTAGES] = { -1, -1, -1 };
    int alive = 0;
    fflush(NULL);
    for (int i = 0; i < NSTAGES; i++) {
        pid[i] = fork();
        if (pid[i] < 0) {
            log_err("parent", "fork failed for %s: %s", STAGE_NAME[i], strerror(errno));
            break;
        }
        if (pid[i] == 0) child_main(i, p1, p2, resume);
        log_info("parent", "forked %-10s pid=%d (parent pid=%d)", STAGE_NAME[i], (int)pid[i],
                 (int)getpid());
        alive++;
    }
    /* The parent must close its copies, or EOF would never be delivered. */
    close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);

    int failed = (alive < NSTAGES);
    if (failed)                              /* partial start: tear down */
        for (int i = 0; i < NSTAGES; i++) if (pid[i] > 0) kill(pid[i], SIGKILL);

    if (cfg.timeout > 0 && !failed) alarm((unsigned)cfg.timeout);
    int grace_armed = 0;

    while (alive > 0) {
        int st;
        pid_t w = waitpid(-1, &st, 0);
        if (w < 0) {
            if (errno == EINTR) {
                if (g_alarm) {
                    g_alarm = 0;
                    log_err("parent", "%s: killing remaining pipeline stages",
                            grace_armed ? "grace period expired" : "TIMEOUT exceeded");
                    for (int i = 0; i < NSTAGES; i++)
                        if (pid[i] > 0) kill(pid[i], SIGKILL);   /* only our own children */
                    failed = 1;
                }
                continue;
            }
            break;                                    /* ECHILD */
        }
        int idx = -1;
        for (int i = 0; i < NSTAGES; i++) if (pid[i] == w) idx = i;
        if (idx < 0) continue;
        pid[idx] = -1; alive--;

        char d[96];
        describe(st, d, sizeof d);
        int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        if (ok) log_info("parent", "reaped  %-10s pid=%d: %s", STAGE_NAME[idx], (int)w, d);
        else    log_err("parent", "reaped  %-10s pid=%d: %s  <-- FAILURE", STAGE_NAME[idx], (int)w, d);
        if (!ok) {
            failed = 1;
            if (!grace_armed && alive > 0) {   /* survivors should notice EOF/EPIPE soon */
                grace_armed = 1;
                alarm(GRACE_SECONDS);
            }
        }
    }
    alarm(0);
    return failed;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [options] LOGFILE\n"
        "  -l, --min-level LEVEL    keep events >= LEVEL (DEBUG|INFO|WARN|ERROR|FATAL, default WARN)\n"
        "  -o, --output FILE        write report to FILE instead of stdout\n"
        "  -c, --checkpoint FILE    enable checkpointing (needed for restarts)\n"
        "  -n, --ckpt-every N       checkpoint every N frames (default 1000)\n"
        "  -r, --max-restarts N     restart a failed pipeline up to N times (needs -c)\n"
        "  -t, --timeout SEC        watchdog: kill the pipeline after SEC seconds\n"
        "  -q, --quiet              suppress lifecycle log messages\n"
        "      --inject STAGE:MODE:N  fault injection: STAGE=reader|filter|summarizer,\n"
        "                           MODE=kill|exit|hang, fire after N frames\n"
        "      --inject-always      inject on every attempt, not only the first\n", p);
}

int main(int argc, char **argv)
{
    static struct option lo[] = {
        { "min-level", 1, 0, 'l' }, { "output", 1, 0, 'o' }, { "checkpoint", 1, 0, 'c' },
        { "ckpt-every", 1, 0, 'n' }, { "max-restarts", 1, 0, 'r' }, { "timeout", 1, 0, 't' },
        { "quiet", 0, 0, 'q' }, { "inject", 1, 0, 1000 }, { "inject-always", 0, 0, 1001 },
        { 0, 0, 0, 0 } };
    int opt;
    while ((opt = getopt_long(argc, argv, "l:o:c:n:r:t:q", lo, NULL)) != -1) {
        switch (opt) {
        case 'l':
            cfg.min_level = level_from_name(optarg, strlen(optarg));
            if (cfg.min_level < 0) { fprintf(stderr, "bad level '%s'\n", optarg); return 2; }
            break;
        case 'o': cfg.outpath = optarg; break;
        case 'c': cfg.ckpt = optarg; break;
        case 'n': cfg.ckpt_every = atol(optarg); break;
        case 'r': cfg.max_restarts = atoi(optarg); break;
        case 't': cfg.timeout = atoi(optarg); break;
        case 'q': cfg.quiet = 1; break;
        case 1001: cfg.inj_always = 1; break;
        case 1000: {
            char st[32], md[32]; long n;
            if (sscanf(optarg, "%31[^:]:%31[^:]:%ld", st, md, &n) != 3) { usage(argv[0]); return 2; }
            for (int i = 0; i < NSTAGES; i++) if (!strcmp(st, STAGE_NAME[i])) cfg.inj_stage = i;
            cfg.inj_mode = !strcmp(md, "kill") ? INJ_KILL : !strcmp(md, "exit") ? INJ_EXIT
                         : !strcmp(md, "hang") ? INJ_HANG : INJ_NONE;
            cfg.inj_after = n;
            if (cfg.inj_stage < 0 || cfg.inj_mode == INJ_NONE) { usage(argv[0]); return 2; }
            break;
        }
        default: usage(argv[0]); return 2;
        }
    }
    if (optind + 1 != argc) { usage(argv[0]); return 2; }
    cfg.input = argv[optind];
    if (cfg.max_restarts > 0 && !cfg.ckpt) {
        fprintf(stderr, "--max-restarts requires --checkpoint (restarts must not lose or repeat records)\n");
        return 2;
    }
    if (access(cfg.input, R_OK) != 0) {
        fprintf(stderr, "logpipe: cannot read '%s': %s\n", cfg.input, strerror(errno));
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);                 /* get EPIPE instead of dying silently */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;                 /* no SA_RESTART: waitpid must return EINTR */
    sigaction(SIGALRM, &sa, NULL);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = 1, attempt;
    for (attempt = 0; attempt <= cfg.max_restarts; attempt++) {
        if (attempt > 0) log_err("parent", "restarting pipeline (attempt %d of %d)", attempt, cfg.max_restarts);
        if ((rc = run_attempt(attempt)) == 0) break;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    if (rc == 0 && cfg.ckpt) unlink(cfg.ckpt);   /* run complete: drop stale state */
    log_err("parent", "RESULT %s attempts=%d elapsed=%.3fs", rc == 0 ? "OK" : "FAILED",
            rc == 0 ? attempt + 1 : cfg.max_restarts + 1, el);
    return rc == 0 ? 0 : 1;
}
