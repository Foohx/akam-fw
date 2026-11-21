/* Tiny host CLI around src/features/track/drift_match.c: the displacement between two raw 8-bit gray frames (the same scene, the camera at "home" before and after a motion test).
 * Usage: drift_shift W H REF.gray LIVE.gray      (W, H multiples of 4, at most 320x192; ffmpeg ... -vf scale=320:180,format=gray -f rawvideo)
 * Prints one line: "verdict=<ok|inconclusive|flat|noref|bad> dx=<px> dy=<px> dx_q=<1/4 px> dy_q=<1/4 px> conf=<permille> fine=<0|1>"; dx,dy = how far the live content moved against the
 * reference (+ = right / down). Exit 0 when a displacement was measured (even 0,0), 3 when the matcher refused (inconclusive / flat), 2 on usage or read errors. */
#include "drift_match.h"
#include <stdio.h>
#include <stdlib.h>

static dm_work_t wk;
static dm_ref_t ref;

static uint8_t *slurp(const char *path, size_t n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = malloc(n);
    if (!f || !b || fread(b, 1, n, f) != n) {
        fprintf(stderr, "cannot read %zu bytes from %s\n", n, path);
        if (f) fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    unsigned w = 0, h = 0;
    uint8_t *a, *b;
    dm_res_t r;
    if (argc == 5) { w = (unsigned)atoi(argv[1]); h = (unsigned)atoi(argv[2]); }
    if (argc != 5 || w == 0 || h == 0 || w % 4 || h % 4 || w > DM_MAX_W || h > DM_MAX_H) {
        fprintf(stderr, "usage: %s W H REF.gray LIVE.gray\n", argv[0]);
        return 2;
    }
    a = slurp(argv[3], (size_t)w * h);
    b = a ? slurp(argv[4], (size_t)w * h) : NULL;
    if (!a || !b) return 2;
    if (dm_ref_build(a, w, h, w, &ref, &wk) != 0) {
        printf("verdict=noref dx=0 dy=0 dx_q=0 dy_q=0 conf=0 fine=0\n");
        return 3;
    }
    (void)dm_match(&ref, b, w, h, w, &wk, &r);
    printf("verdict=%s dx=%d dy=%d dx_q=%d dy_q=%d conf=%d fine=%d\n", dm_verdict_name(r.verdict), r.dx, r.dy, r.dx_q, r.dy_q, r.conf, r.fine);
    free(a);
    free(b);
    return r.verdict == DM_OK ? 0 : 3;
}
