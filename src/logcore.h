/*
 * logcore.h - Shared logic used by BOTH the multi-process pipeline (logpipe)
 * and the single-process baseline (logsingle).
 *
 * Keeping parsing, filtering rules and the summary/report code in one place
 * guarantees that the two programs implement the same specification, so their
 * reports can be compared byte-for-byte.
 *
 * Log line format (one event per line):
 *     YYYY-MM-DD HH:MM:SS LEVEL message text...
 * LEVEL is one of DEBUG INFO WARN ERROR FATAL (upper-case).
 * Anything else is a "malformed" line and is counted, never silently lost.
 */
#ifndef LOGCORE_H
#define LOGCORE_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

enum level { LV_DEBUG = 0, LV_INFO, LV_WARN, LV_ERROR, LV_FATAL, LV_COUNT };

#define MSG_MAX      256    /* message stored/transferred up to 255 chars   */
#define LINE_MAX_LEN 4096   /* longer physical lines are treated as malformed */
#define MSG_BUCKETS  1024

typedef struct {
    long   seq;             /* 1-based line number in the input file        */
    time_t epoch;           /* timestamp (UTC) as seconds since 1970        */
    int    level;           /* enum level                                   */
    char   msg[MSG_MAX];    /* message text (trimmed, truncated if long)    */
} record_t;

const char *level_name(int lv);
int         level_from_name(const char *s, size_t n);   /* -1 if unknown */

/* Parse one line (without trailing newline). 0 = OK, -1 = malformed. */
int parse_log_line(const char *line, size_t len, record_t *out);

/* ---- Summary (the "final report") state ------------------------------ */

typedef struct msgent {
    char           msg[MSG_MAX];
    long           count;
    struct msgent *next;
} msgent_t;

typedef struct {
    long     valid;                 /* well-formed records (kept + dropped) */
    long     kept;                  /* passed the severity filter           */
    long     dropped;               /* well-formed but filtered out         */
    long     malformed;             /* malformed lines                      */
    long     by_level[LV_COUNT];    /* kept records per severity            */
    int      have_time;
    time_t   first, last;           /* time range of kept records           */
    long     last_seq;              /* highest input line number applied    */
    msgent_t *buckets[MSG_BUCKETS]; /* message -> count hash table          */
} summary_t;

void summary_init(summary_t *s);
void summary_free(summary_t *s);
void summary_add_record(summary_t *s, const record_t *r);  /* kept record  */
void summary_add_dropped(summary_t *s, long n);            /* filtered out */
void summary_add_malformed(summary_t *s);
void summary_report(const summary_t *s, FILE *out, int min_level);

/* Atomic checkpoint (write temp file, fsync, rename). 0 ok, -1 error. */
int  summary_save(const summary_t *s, const char *path);
/* 0 = loaded, 1 = no checkpoint file, -1 = corrupt/unreadable. */
int  summary_load(summary_t *s, const char *path);

#endif
