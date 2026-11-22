/* akam2k_mic -- standalone microphone proof of concept for the Rotatable 2K (audio-findings.md, route A, plan step 3).
 *
 *   akam2k_mic [-s SECONDS] [-r RATE] [-f] OUTFILE.wav
 *
 *   -s SECONDS  1..30, default 5
 *   -r RATE     8000 or 16000, default 8000 (16000 is UNVERIFIED on the hardware: the 1080p code only ever ran 8000)
 *   -f          allow an OUTFILE outside /tmp (default: only /tmp/NAME)
 *   (no -g: the only gain ioctl known from the 1080p tree, nr 0x30, is the PLAYBACK gain; on the capture fd nr 0x30 is
 *    the periods call, so a microphone gain is not settable here until it is traced.)
 *
 * It opens the capture device read-only and replays, step by step, the capture sequence of platforms/anyka-ak3918/audio.c
 * (audio_init_capture, ground truth of the stock anyka_ipc): SET_STATE(2), GET_PERIODS, GET_PARS (= commit of
 * struct akpcm_pars: format 0, rate, 1 channel, 16 bit, 512-byte periods, 16 periods), RESET_BUF(-1), SET_MODE(0),
 * SET_RATE(rate), SET_STATE(2), SET_PERIODS(2). Then it read()s 512-byte periods into one fixed buffer, writes them to
 * OUTFILE, fixes the WAV header up at the end and prints the statistics on stderr.
 *
 * Safety: writes only OUTFILE; never opens the playback or loopback device, never touches a GPIO; no allocation; a
 * flock() on the capture fd refuses a second akam2k_mic (it cannot see another program, such as the stock yy_cam or
 * akam2k_rtsp with audio: stop those first, they own the capture); at most 5 consecutive read errors (200 ms apart),
 * then it stops; SIGINT/SIGTERM and an alarm stop the loop and the file is still finalised; exit 0 only when at
 * least one period was captured. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "akpcm.h"
#include "mic_core.h"

#define PCM_CAPTURE "/dev/pcmC0D0c"
#define PERIOD_BYTES 512u
#define PERIOD_COUNT 16u
#define MAX_CONSEC_FAILS 5


static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#define STEP(name, expr) do { if ((expr) < 0) { fprintf(stderr, "akam2k_mic: step '%s' failed: %s (errno=%d)\n", name, strerror(errno), errno); goto fail; } } while (0)

static int usage(void)
{
    fputs("usage: akam2k_mic [-s SECONDS 1..30] [-r 8000|16000] [-f] OUTFILE.wav\n", stderr);
    return 2;
}

int main(int argc, char **argv)
{
    int secs = 5, force = 0;
    uint32_t rate = 8000;
    int opt;
    while ((opt = getopt(argc, argv, "s:r:f")) != -1) {
        char *end;
        long v;
        switch (opt) {
        case 's':
            v = strtol(optarg, &end, 10);
            if (*optarg == 0 || *end || v < 1 || v > 30) return usage();
            secs = (int)v;
            break;
        case 'r':
            v = strtol(optarg, &end, 10);
            if (*optarg == 0 || *end || (v != 8000 && v != 16000)) return usage();
            rate = (uint32_t)v;
            break;
        case 'f': force = 1; break;
        default: return usage();
        }
    }
    if (optind != argc - 1) return usage();
    const char *outpath = argv[optind];
    if (!mic_path_ok(outpath, force)) {
        fprintf(stderr, "akam2k_mic: refusing OUTFILE '%s' (only /tmp/NAME unless -f)\n", outpath);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig; /* no SA_RESTART: a blocked read() returns EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    /* No O_TRUNC at open: a FIFO/device/hard-linked file left in /tmp must not be truncated or block the open. Only a
     * regular file with one link is accepted, then truncated. O_NOFOLLOW covers the last component (the only one: the
     * default rule allows no sub-directory). */
    int out = open(outpath, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0644);
    if (out < 0) { fprintf(stderr, "akam2k_mic: open %s: %s\n", outpath, strerror(errno)); return 1; }
    struct stat sb;
    if (fstat(out, &sb) < 0 || !S_ISREG(sb.st_mode) || sb.st_nlink != 1 || ftruncate(out, 0) < 0) {
        fprintf(stderr, "akam2k_mic: %s is not a plain single-link regular file, refusing\n", outpath);
        close(out);
        return 1;
    }
    uint8_t hdr[MIC_WAV_HDR_LEN];
    mic_wav_header(hdr, rate, 0);
    if (write(out, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) { fprintf(stderr, "akam2k_mic: write header: %s\n", strerror(errno)); close(out); return 1; }

    int rc = 1;
    int fd = open(PCM_CAPTURE, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "akam2k_mic: open %s: %s\n", PCM_CAPTURE, strerror(errno)); goto done; }
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { fprintf(stderr, "akam2k_mic: %s is locked by another akam2k_mic\n", PCM_CAPTURE); goto done; }

    {
        int32_t v = 2;
        struct akpcm_pars pars;
        STEP("SET_STATE(2)", ioctl(fd, AKPCM_IOC_SET_STATE, &v));
        v = 0;
        if (ioctl(fd, AKPCM_IOC_GET_PERIODS, &v) < 0) fprintf(stderr, "akam2k_mic: GET_PERIODS (non-fatal): %s\n", strerror(errno));
        memset(&pars, 0, sizeof(pars));
        pars.rate = rate;
        pars.channels = 1;
        pars.bits = 16;
        pars.period_size = PERIOD_BYTES;
        pars.period_count = PERIOD_COUNT;
        STEP("GET_PARS (commit params)", ioctl(fd, AKPCM_IOC_GET_PARS, &pars));
        STEP("RESET_BUF(-1)", ioctl(fd, AKPCM_IOC_RESET_BUF, (void *)(long)-1));
        STEP("SET_MODE(0)", ioctl(fd, AKPCM_IOC_SET_MODE, 0));
        v = (int32_t)rate;
        STEP("SET_RATE", ioctl(fd, AKPCM_IOC_SET_RATE, &v));
        v = 2;
        STEP("SET_STATE(2)#2", ioctl(fd, AKPCM_IOC_SET_STATE, &v));
        v = 2;
        STEP("SET_PERIODS(2)", ioctl(fd, AKPCM_IOC_SET_PERIODS, &v));
        fprintf(stderr, "akam2k_mic: configured %u Hz mono 16 bit, requested period %u B x %u (struct after the commit call: period_size %u, period_count %u)\n",
                (unsigned)rate, PERIOD_BYTES, PERIOD_COUNT, (unsigned)pars.period_size, (unsigned)pars.period_count);
    }

    static int16_t buf[PERIOD_BYTES / 2];
    struct mic_stats st;
    mic_stats_init(&st);
    uint64_t bytes = 0, first_bytes = 0, reads = 0, errors = 0, shorts = 0;
    size_t min_read = 0, max_read = 0;
    const uint64_t want = (uint64_t)rate * 2u * (uint64_t)secs;
    int consec = 0;
    int64_t t0 = 0, t1 = 0;
    alarm((unsigned)secs + 10u); /* a hung read() cannot run forever */
    while (!g_stop && bytes < want) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            errors++;
            fprintf(stderr, "akam2k_mic: read: %s (errno=%d)\n", strerror(errno), errno);
            if (++consec >= MAX_CONSEC_FAILS) { fputs("akam2k_mic: too many consecutive read errors, stopping\n", stderr); break; }
            usleep(200000);
            continue;
        }
        consec = 0;
        if (n == 0) { usleep(10000); continue; }
        if (n & 1) n--; /* whole samples only */
        if (n == 0) continue;
        t1 = mono_us();
        if ((uint64_t)n > want - bytes) n = (ssize_t)(want - bytes);
        if (write(out, buf, (size_t)n) != n) { fprintf(stderr, "akam2k_mic: write: %s\n", strerror(errno)); errors++; break; }
        mic_stats_add(&st, buf, (size_t)n / 2);
        bytes += (uint64_t)n;
        if (reads == 0) { t0 = t1; first_bytes = (uint64_t)n; min_read = max_read = (size_t)n; }
        reads++;
        if ((size_t)n < PERIOD_BYTES) shorts++;
        if ((size_t)n < min_read) min_read = (size_t)n;
        if ((size_t)n > max_read) max_read = (size_t)n;
    }
    alarm(0);

    mic_wav_header(hdr, rate, (uint32_t)bytes);
    if (pwrite(out, hdr, sizeof(hdr), 0) != (ssize_t)sizeof(hdr)) fprintf(stderr, "akam2k_mic: header fix-up: %s\n", strerror(errno));

    /* The clock starts when the first read() returns, so its data is not counted: the rate is the rest over the span. */
    double span = (t1 > t0) ? (double)(t1 - t0) / 1e6 : 0.0;
    double eff = span > 0 ? (double)(bytes - first_bytes) / 2.0 / span : 0.0;
    fprintf(stderr, "akam2k_mic: bytes %llu, frames %llu, reads %llu (size min %zu max %zu, short %llu)\n",
            (unsigned long long)bytes, (unsigned long long)st.n, (unsigned long long)reads, min_read, max_read, (unsigned long long)shorts);
    fprintf(stderr, "akam2k_mic: effective rate %.1f Hz over %.3f s (nominal %u)\n", eff, span, (unsigned)rate);
    if (st.n)
        fprintf(stderr, "akam2k_mic: min %d (%.1f dBFS), max %d (%.1f dBFS), peak %.1f dBFS, RMS %.1f dBFS\n",
                st.min, mic_sample_dbfs(st.min), st.max, mic_sample_dbfs(st.max), mic_peak_dbfs(&st), mic_rms_dbfs(&st));
    fprintf(stderr, "akam2k_mic: read errors/overruns %llu, accepted period %u B (observed read size %zu..%zu B)\n",
            (unsigned long long)errors, PERIOD_BYTES, min_read, max_read);
    if (g_stop) fputs("akam2k_mic: stopped by signal (file finalised)\n", stderr);
    rc = bytes > 0 ? 0 : 1;
    goto done;

fail:
    rc = 1;
done:
    if (fd >= 0) close(fd); /* closing releases the flock */
    close(out);
    return rc;
}
