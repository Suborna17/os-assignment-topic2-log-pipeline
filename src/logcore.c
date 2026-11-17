/* logcore.c - parsing, summary statistics, report and checkpoint code. */
#define _GNU_SOURCE
#include "logcore.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *LV_NAMES[LV_COUNT] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

const char *level_name(int lv)
{
    return (lv >= 0 && lv < LV_COUNT) ? LV_NAMES[lv] : "?";
}

int level_from_name(const char *s, size_t n)
{
    for (int i = 0; i < LV_COUNT; i++)
        if (strlen(LV_NAMES[i]) == n && memcmp(LV_NAMES[i], s, n) == 0)
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Line parser                                                        */
/* ------------------------------------------------------------------ */

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int num2(const char *p) { return (p[0] - '0') * 10 + (p[1] - '0'); }
static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in_month(int y, int m)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap(y)) ? 29 : d[m - 1];
}

int parse_log_line(const char *line, size_t len, record_t *out)
{
    /* "YYYY-MM-DD HH:MM:SS " is 20 chars, then LEVEL, a space, a message */
    if (len < 26)
        return -1;

    static const char layout[] = "dddd-dd-dd dd:dd:dd ";
    for (int i = 0; i < 20; i++) {
        if (layout[i] == 'd') {
            if (!is_digit(line[i])) return -1;
        } else if (line[i] != layout[i]) {
            return -1;
        }
    }

    int year = (line[0] - '0') * 1000 + (line[1] - '0') * 100 +
               (line[2] - '0') * 10 + (line[3] - '0');
    int mon = num2(line + 5), day = num2(line + 8);
    int hh = num2(line + 11), mm = num2(line + 14), ss = num2(line + 17);

    if (year < 1970 || mon < 1 || mon > 12) return -1;
    if (day < 1 || day > days_in_month(year, mon)) return -1;
    if (hh > 23 || mm > 59 || ss > 59) return -1;

    /* LEVEL token */
    const char *end = line + len;
    const char *q = line + 20;
    while (q < end && *q != ' ') q++;
    if (q >= end) return -1;                       /* no message */
    int lv = level_from_name(line + 20, (size_t)(q - (line + 20)));
    if (lv < 0) return -1;

    /* message: skip separator spaces, trim trailing whitespace */
    while (q < end && *q == ' ') q++;
    while (end > q && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    if (end <= q) return -1;                       /* empty message */

    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = year - 1900; tm.tm_mon = mon - 1; tm.tm_mday = day;
    tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = ss;

    size_t mlen = (size_t)(end - q);
    if (mlen > MSG_MAX - 1) mlen = MSG_MAX - 1;
    memcpy(out->msg, q, mlen);
    out->msg[mlen] = '\0';
    out->epoch = timegm(&tm);
    out->level = lv;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Summary                                                            */
/* ------------------------------------------------------------------ */

void summary_init(summary_t *s) { memset(s, 0, sizeof *s); }

void summary_free(summary_t *s)
{
    for (int i = 0; i < MSG_BUCKETS; i++) {
        msgent_t *e = s->buckets[i];
        while (e) { msgent_t *n = e->next; free(e); e = n; }
        s->buckets[i] = NULL;
    }
}

static unsigned hash_str(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33u + (unsigned char)*s++;
    return h % MSG_BUCKETS;
}

static int msg_add(summary_t *s, const char *msg, long n)
{
    unsigned b = hash_str(msg);
    for (msgent_t *e = s->buckets[b]; e; e = e->next)
        if (strcmp(e->msg, msg) == 0) { e->count += n; return 0; }
    msgent_t *e = malloc(sizeof *e);
    if (!e) return -1;
    snprintf(e->msg, sizeof e->msg, "%s", msg);
    e->count = n;
    e->next = s->buckets[b];
    s->buckets[b] = e;
    return 0;
}

void summary_add_record(summary_t *s, const record_t *r)
{
    s->kept++;
    s->valid++;
    s->by_level[r->level]++;
    if (!s->have_time) { s->first = s->last = r->epoch; s->have_time = 1; }
    if (r->epoch < s->first) s->first = r->epoch;
    if (r->epoch > s->last)  s->last  = r->epoch;
    if (msg_add(s, r->msg, 1) != 0) { perror("malloc"); exit(2); }
}

void summary_add_dropped(summary_t *s, long n) { s->dropped += n; s->valid += n; }
void summary_add_malformed(summary_t *s)       { s->malformed++; }

static int cmp_ent(const void *a, const void *b)
{
    const msgent_t *x = *(msgent_t *const *)a, *y = *(msgent_t *const *)b;
    if (x->count != y->count) return (y->count > x->count) ? 1 : -1;
    return strcmp(x->msg, y->msg);
}

static msgent_t **collect(const summary_t *s, size_t *n_out)
{
    size_t n = 0;
    for (int i = 0; i < MSG_BUCKETS; i++)
        for (msgent_t *e = s->buckets[i]; e; e = e->next) n++;
    msgent_t **v = malloc((n ? n : 1) * sizeof *v);
    if (!v) { perror("malloc"); exit(2); }
    size_t k = 0;
    for (int i = 0; i < MSG_BUCKETS; i++)
        for (msgent_t *e = s->buckets[i]; e; e = e->next) v[k++] = e;
    qsort(v, n, sizeof *v, cmp_ent);
    *n_out = n;
    return v;
}

void summary_report(const summary_t *s, FILE *out, int min_level)
{
    char a[32] = "n/a", b[32] = "n/a";
    if (s->have_time) {
        struct tm tm;
        gmtime_r(&s->first, &tm); strftime(a, sizeof a, "%Y-%m-%d %H:%M:%S", &tm);
        gmtime_r(&s->last,  &tm); strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    }
    fprintf(out, "=== Log Summary Report ===\n");
    fprintf(out, "Lines scanned          : %ld\n", s->valid + s->malformed);
    fprintf(out, "  well-formed          : %ld\n", s->valid);
    fprintf(out, "  malformed            : %ld\n", s->malformed);
    fprintf(out, "Filter                 : severity >= %s\n", level_name(min_level));
    fprintf(out, "  kept                 : %ld\n", s->kept);
    fprintf(out, "  filtered out         : %ld\n", s->dropped);
    fprintf(out, "Time range (kept, UTC) : %s .. %s\n", a, b);
    fprintf(out, "Kept events by severity:\n");
    for (int i = 0; i < LV_COUNT; i++)
        fprintf(out, "  %-6s %ld\n", level_name(i), s->by_level[i]);

    size_t n;
    msgent_t **v = collect(s, &n);
    fprintf(out, "Top event messages (kept):\n");
    for (size_t i = 0; i < n && i < 5; i++)
        fprintf(out, "  %zu. %ld  %s\n", i + 1, v[i]->count, v[i]->msg);
    if (n == 0) fprintf(out, "  (none)\n");
    free(v);
}

/* ------------------------------------------------------------------ */
/* Checkpoint (used by the restart challenge)                         */
/* ------------------------------------------------------------------ */

int summary_save(const summary_t *s, const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "LOGSUM1\n");
    fprintf(f, "last_seq %ld\nvalid %ld\nkept %ld\ndropped %ld\nmalformed %ld\n",
            s->last_seq, s->valid, s->kept, s->dropped, s->malformed);
    fprintf(f, "time %d %lld %lld\n", s->have_time, (long long)s->first, (long long)s->last);
    fprintf(f, "levels");
    for (int i = 0; i < LV_COUNT; i++) fprintf(f, " %ld", s->by_level[i]);
    fprintf(f, "\n");
    for (int i = 0; i < MSG_BUCKETS; i++)
        for (msgent_t *e = s->buckets[i]; e; e = e->next)
            fprintf(f, "msg %ld %s\n", e->count, e->msg);
    fprintf(f, "END\n");
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { fclose(f); unlink(tmp); return -1; }
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }   /* atomic commit */
    return 0;
}

int summary_load(summary_t *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return (errno == ENOENT) ? 1 : -1;
    char *line = NULL; size_t cap = 0; ssize_t n;
    int ok = 0, have_end = 0;
    summary_init(s);

    if ((n = getline(&line, &cap, f)) < 0 || strcmp(line, "LOGSUM1\n") != 0) goto bad;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n') line[n - 1] = '\0';
        long long a, b; long c[LV_COUNT]; int t; long v;
        if (sscanf(line, "last_seq %ld", &v) == 1)       s->last_seq = v;
        else if (sscanf(line, "valid %ld", &v) == 1)     s->valid = v;
        else if (sscanf(line, "kept %ld", &v) == 1)      s->kept = v;
        else if (sscanf(line, "dropped %ld", &v) == 1)   s->dropped = v;
        else if (sscanf(line, "malformed %ld", &v) == 1) s->malformed = v;
        else if (sscanf(line, "time %d %lld %lld", &t, &a, &b) == 3) {
            s->have_time = t; s->first = (time_t)a; s->last = (time_t)b;
        } else if (sscanf(line, "levels %ld %ld %ld %ld %ld",
                          &c[0], &c[1], &c[2], &c[3], &c[4]) == LV_COUNT) {
            for (int i = 0; i < LV_COUNT; i++) s->by_level[i] = c[i];
        } else if (strncmp(line, "msg ", 4) == 0) {
            char *end; long cnt = strtol(line + 4, &end, 10);
            if (*end != ' ' || cnt <= 0) goto bad;
            if (msg_add(s, end + 1, cnt) != 0) goto bad;
        } else if (strcmp(line, "END") == 0) {
            have_end = 1; break;
        } else goto bad;
    }
    ok = have_end;
bad:
    free(line);
    fclose(f);
    if (!ok) { summary_free(s); summary_init(s); return -1; }
    return 0;
}
