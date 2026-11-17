/*
 * logsingle.c - Single-process baseline.
 *
 * Does exactly what the three-stage pipeline does (read -> filter -> summarize)
 * but inside ONE process with ordinary function calls. Used for:
 *   - correctness comparison (reports must be byte-identical), and
 *   - performance comparison (what does IPC cost, and does it pay off?).
 *
 * Usage: logsingle [-l LEVEL] [-o REPORT_FILE] LOGFILE
 */
#define _GNU_SOURCE
#include "logcore.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    int min_level = LV_WARN;
    const char *outpath = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "l:o:")) != -1) {
        switch (opt) {
        case 'l':
            min_level = level_from_name(optarg, strlen(optarg));
            if (min_level < 0) { fprintf(stderr, "logsingle: bad level '%s'\n", optarg); return 2; }
            break;
        case 'o': outpath = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-l LEVEL] [-o REPORT] LOGFILE\n", argv[0]);
            return 2;
        }
    }
    if (optind + 1 != argc) {
        fprintf(stderr, "usage: %s [-l LEVEL] [-o REPORT] LOGFILE\n", argv[0]);
        return 2;
    }

    FILE *in = fopen(argv[optind], "r");
    if (!in) { perror(argv[optind]); return 2; }

    summary_t s;
    summary_init(&s);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    long seq = 0;

    while ((n = getline(&line, &cap, in)) != -1) {
        seq++;
        size_t len = (size_t)n;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;

        record_t r;
        if (n > LINE_MAX_LEN || memchr(line, '\0', (size_t)n) != NULL ||
            parse_log_line(line, len, &r) != 0) {
            summary_add_malformed(&s);
        } else if (r.level >= min_level) {
            summary_add_record(&s, &r);
        } else {
            summary_add_dropped(&s, 1);
        }
    }
    if (ferror(in)) { perror("read"); return 2; }
    fclose(in);
    free(line);

    FILE *out = stdout;
    if (outpath && !(out = fopen(outpath, "w"))) { perror(outpath); return 2; }
    summary_report(&s, out, min_level);
    if (fflush(out) != 0 || (out != stdout && fclose(out) != 0)) { perror("write report"); return 2; }

    summary_free(&s);
    return 0;
}
