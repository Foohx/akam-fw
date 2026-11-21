/* RAM copy of the log (tmpfs, never flash): two segments `path` and `path.1`, written in turn, each capped at `max` bytes. When the current one is full the other is truncated and written next (no rename).
 * Every segment starts with a header line "[ramlog seq=N]": N grows by one at every rotation, so the two files can always be put in order, also after an app restart (the ms clock of the lines restarts with the
 * process): the first write of a process continues on the segment with the higher N (appending, after an "[ramlog seq=N app start]" marker line) and rotates when it is full. Header-only so the host test
 * (tests/rot2k_ramlog_unit.c) runs the real code; the caller serialises the calls (main.c holds its log mutex). */
#ifndef AKAM2K_RAMLOG_H
#define AKAM2K_RAMLOG_H

#include <stdio.h>
#include <string.h>

#define A2K_RAMLOG_MAX 65536

struct a2k_ramlog {
    FILE *f;
    int seg;
    long sz, max;
    unsigned long seq;
    int started; /* the first write of this process chose its segment */
    char path[2][96];
};

#define A2K_RAMLOG_INIT { NULL, 0, 0, A2K_RAMLOG_MAX, 0, 0, { "/tmp/akam2k_main.log", "/tmp/akam2k_main.log.1" } }

/* closes the current file and points the ring at another pair (tests) / the same one again (a new "process") */
static inline void a2k_ramlog_set(struct a2k_ramlog *r, const char *path, long max)
{
    if (r->f)
        fclose(r->f);
    r->f = NULL;
    r->sz = 0;
    r->seg = 0;
    r->seq = 0;
    r->started = 0;
    snprintf(r->path[0], sizeof r->path[0], "%s", path);
    snprintf(r->path[1], sizeof r->path[1], "%s.1", path);
    r->max = max > 0 ? max : A2K_RAMLOG_MAX;
}

/* the sequence number of a segment file: its header line, 0 when absent / not ours */
static inline unsigned long a2k_ramlog_seq_of(const char *path)
{
    char line[64];
    unsigned long n = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (fgets(line, sizeof line, f) && sscanf(line, "[ramlog seq=%lu]", &n) != 1)
        n = 0;
    fclose(f);
    return n;
}

static inline int a2k_ramlog_open(struct a2k_ramlog *r, int seg, int truncate)
{
    r->f = fopen(r->path[seg], truncate ? "w" : "a");
    if (!r->f)
        return -1;
    r->seg = seg;
    if (truncate) {
        r->sz = 0;
        r->sz += fprintf(r->f, "[ramlog seq=%lu]\n", r->seq);
    } else {
        fseek(r->f, 0, SEEK_END);
        r->sz = ftell(r->f);
        if (r->sz < 0)
            r->sz = 0;
    }
    return 0;
}

static inline void a2k_ramlog_put(struct a2k_ramlog *r, const char *line, size_t n)
{
    if (!r->started) { /* the first write of this process: continue on the newer segment, so that the order of the two files is never lost to a restart */
        unsigned long s0 = a2k_ramlog_seq_of(r->path[0]), s1 = a2k_ramlog_seq_of(r->path[1]);
        int seg = s1 > s0 ? 1 : 0;
        r->seq = s1 > s0 ? s1 : s0;
        r->started = 1;
        if (r->seq == 0) { /* neither file is ours: a fresh start on segment 0 */
            r->seq = 1;
            if (a2k_ramlog_open(r, 0, 1) != 0)
                return;
        } else {
            if (a2k_ramlog_open(r, seg, 0) != 0)
                return;
            if (r->sz > 0) { /* a marker: the ms clock of the lines below restarts here */
                char m[64];
                int k = snprintf(m, sizeof m, "[ramlog seq=%lu app start]\n", r->seq);
                if (fwrite(m, 1, (size_t)k, r->f) == (size_t)k)
                    r->sz += k;
            }
        }
    }
    if (!r->f && a2k_ramlog_open(r, r->seg, 0) != 0) /* an earlier open failed (tmpfs full, a vanished directory): tried again */
        return;
    if (r->f && r->sz + (long)n > r->max) {
        fclose(r->f);
        r->f = NULL;
        r->seq++;
        if (a2k_ramlog_open(r, r->seg ^ 1, 1) != 0)
            return;
    }
    if (!r->f)
        return;
    if (fwrite(line, 1, n, r->f) == n) {
        fflush(r->f);
        r->sz += (long)n;
    }
}

#endif
