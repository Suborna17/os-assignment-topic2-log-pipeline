/*
 * loggen.c - deterministic synthetic log generator (same seed => same file).
 *
 * Usage: loggen -n LINES [-s SEED] [-m MALFORMED_PER_MILLE] [-o FILE]
 *
 * Severity mix: DEBUG 30%, INFO 45%, WARN 12%, ERROR 11%, FATAL 2%.
 * Malformed lines (when -m > 0) use several different defects so the parser's
 * validation paths are exercised.
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t rng_state = 88172645463325252ULL;
static uint32_t rnd(void)                 /* xorshift64* */
{
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 2685821657736338717ULL) >> 32);
}

static const char *DEBUG_MSG[] = { "cache lookup miss", "entering handler", "query plan selected", "heartbeat tick" };
static const char *INFO_MSG[]  = { "user login ok", "request completed", "session created", "config reloaded", "job scheduled" };
static const char *WARN_MSG[]  = { "slow query detected", "retrying request", "disk usage above 80%", "deprecated API call" };
static const char *ERR_MSG[]   = { "database connection timeout", "payment gateway unreachable", "failed to write file",
                                   "null pointer in handler", "authentication token expired", "upstream service returned 503" };
static const char *FATAL_MSG[] = { "out of memory", "unrecoverable data corruption" };
static const char *LV[] = { "DEBUG", "INFO", "WARN", "ERROR", "FATAL" };

#define PICK(a) ((a)[rnd() % (sizeof(a) / sizeof((a)[0]))])

int main(int argc, char **argv)
{
    long n = 0, mal = 0;
    const char *outpath = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "n:s:m:o:")) != -1) {
        switch (opt) {
        case 'n': n = atol(optarg); break;
        case 's': rng_state ^= (uint64_t)atol(optarg) * 0x9E3779B97F4A7C15ULL; break;
        case 'm': mal = atol(optarg); break;
        case 'o': outpath = optarg; break;
        default: fprintf(stderr, "usage: %s -n LINES [-s SEED] [-m PER_MILLE_MALFORMED] [-o FILE]\n", argv[0]); return 2;
        }
    }
    if (n <= 0) { fprintf(stderr, "loggen: -n LINES is required\n"); return 2; }
    for (int i = 0; i < 10; i++) rnd();

    FILE *out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) { perror(outpath); return 2; }
    static char obuf[1 << 16];
    setvbuf(out, obuf, _IOFBF, sizeof obuf);

    time_t t = 1767225600;                /* 2026-01-01 00:00:00 UTC */
    for (long i = 0; i < n; i++) {
        t += rnd() % 4;
        struct tm tm;
        gmtime_r(&t, &tm);
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

        uint32_t p = rnd() % 100;
        int lv = p < 30 ? 0 : p < 75 ? 1 : p < 87 ? 2 : p < 98 ? 3 : 4;
        const char *msg = lv == 0 ? PICK(DEBUG_MSG) : lv == 1 ? PICK(INFO_MSG) :
                          lv == 2 ? PICK(WARN_MSG)  : lv == 3 ? PICK(ERR_MSG) : PICK(FATAL_MSG);

        if (mal > 0 && (long)(rnd() % 1000) < mal) {
            switch (rnd() % 7) {
            case 0: fprintf(out, "%s %s\n", ts, msg); break;                          /* missing level   */
            case 1: fprintf(out, "%s CRITICAL %s\n", ts, msg); break;                 /* unknown level   */
            case 2: fprintf(out, "26-13-45 99:99:99 %s %s\n", LV[lv], msg); break;    /* bad timestamp   */
            case 3: fprintf(out, "%s %s\n", ts, LV[lv]); break;                       /* empty message   */
            case 4: fprintf(out, "garbage ### %u\n", rnd()); break;                   /* not a log line  */
            case 5: fprintf(out, "%s %s %s\n", ts, "error", msg); break;              /* lower-case level*/
            default: fprintf(out, "\n"); break;                                       /* blank line      */
            }
        } else {
            fprintf(out, "%s %s %s\n", ts, LV[lv], msg);
        }
    }
    if (fflush(out) != 0 || (out != stdout && fclose(out) != 0)) { perror("write"); return 2; }
    return 0;
}
