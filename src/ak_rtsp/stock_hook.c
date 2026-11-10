/* stock_hook.c - LD_PRELOAD capture of what the STOCK vendor app anyka_ipc does to
 * the speaker, the GPIOs and the ISP, so ak_rtsp can mirror it. Read-only on the
 * camera: it only observes and writes its own log files. Superset of isp_hook.c
 * (the ISP/VENC/AKPCM decoders are the very same code, #included below).
 *
 * What is logged (one text line per event, key=value, parsed by
 * tools/hw_tests/parse_stock_capture.py):
 *   - open/close of the PCM devices (/dev/pcm..., /dev/akpcm..., /dev/snd/...), of the
 *     GPIO sysfs tree (/sys/class/gpio/...), /dev/mem, of audio files (.mp3, .wav, files
 *     under /usr/share/), of ISP and config files
 *   - EVERY ioctl on those fds (throttled when identical): request, decoded
 *     dir/type/nr/size, the argument bytes before and after the call (bounded hex),
 *     return value and errno
 *   - write()/read()/writev() sizes on those fds; for audio the first bytes and a
 *     2 s statistics window (rate, gaps, peak, zero and L==R counts) from which the
 *     format can be inferred; the first 64 KiB of playback data go to stock_audio.raw
 *   - mmap() of those fds (e.g. /dev/mem register windows)
 *   - ISP parameter blocks (BLC, CCM, gamma, sharpness, ...): bounded hex dump of the
 *     payload of each ISP ioctl, plus everything isp_hook.c already captured (AE
 *     windows, sensor registers, VENC CREATE_ENC ...) in isp_hook.log
 * Not captured: fopen()/fwrite() (stdio calls the libc-internal open, not ours) and
 * system()/popen() commands. The stock binary imports open/write/ioctl directly.
 *
 * Log placement (first directory that works): /tmp/stock_capture/out (symlink made by
 * stock_capture.sh to the SD card run folder), /tmp/sd/stock_capture (only when that folder
 * already exists: never created, so it cannot silently become a RAM directory), then /tmp
 * (RAM, 1 MiB cap per file). Nothing is ever created on the root filesystem or in flash.
 * Size caps per file; a missing or removed card never crashes anything (events are dropped,
 * the real syscall result is always returned).
 *
 * Threads: no lock and no allocation (a lock taken in a hooked call could deadlock against a
 * signal handler or the stock code). Every event is built on the caller's stack and written
 * with ONE write() on an O_APPEND descriptor, so lines of different threads never mix; the
 * shared counters and tables are racy on purpose and can only blur a statistic, never the
 * result of the call that is being observed.
 *
 * The hook only activates in a process whose /proc/self/exe is called anyka_ipc
 * (STOCK_HOOK_COMM), so children spawned by system()/popen() inherit LD_PRELOAD but
 * stay silent. errno is set like libc does (the raw syscalls bypass libc).
 *
 * No libc dependency, no division by a variable, no compiler-rt: raw syscalls
 * (ARM EABI on the camera, x86_64 only for the host test).
 *
 * Build: make stock_hook.so       Host test build: see tests/stock_hook_test.sh
 */
#include <stdint.h>
#include <stdarg.h>

#ifndef STOCK_HOOK_COMM
#define STOCK_HOOK_COMM "anyka_ipc"
#endif

/* ---- raw syscalls -------------------------------------------------------- */
#if defined(__arm__)
static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r3 asm("r3") = d;
    register long r4 asm("r4") = e;
    register long r5 asm("r5") = f;
    register long r7 asm("r7") = n;
    asm volatile ("svc #0" : "+r"(r0)
                  : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r7)
                  : "memory", "cc");
    return r0;
}
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_LSEEK      19
#define SYS_DUP        41
#define SYS_IOCTL      54
#define SYS_FCNTL      55
#define SYS_DUP2       63
#define SYS_READLINK   85
#define SYS_WRITEV    146
#define SYS_MMAP      192   /* mmap2: offset in 4096-byte pages */
#define SYS_FSYNC     118
#define SYS_GETPPID    64
#define SYS_MINCORE   219
#define SYS_GETTID    224
#define SYS_CLOCK_GETTIME 263
#define SYS_PIPE2     359
#define MMAP_OFF(x)  ((long)(((unsigned long)(x)) >> 12))
#elif defined(__x86_64__)
/* Host test only (tests/stock_hook_test.sh). */
static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    register long r10 asm("r10") = d;
    register long r8  asm("r8")  = e;
    register long r9  asm("r9")  = f;
    long ret;
    asm volatile ("syscall" : "=a"(ret)
                  : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                  : "rcx", "r11", "memory");
    return ret;
}
#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_LSEEK       8
#define SYS_MMAP        9
#define SYS_IOCTL      16
#define SYS_WRITEV     20
#define SYS_DUP        32
#define SYS_DUP2       33
#define SYS_FCNTL      72
#define SYS_READLINK   89
#define SYS_FSYNC      74
#define SYS_GETPPID   110
#define SYS_MINCORE    27
#define SYS_GETTID    186
#define SYS_CLOCK_GETTIME 228
#define SYS_PIPE2     293
#define MMAP_OFF(x)  ((long)(x))
#else
#error "stock_hook.c supports ARM EABI (camera) and x86_64 (host test) only"
#endif

/* ARM EABI division helpers, private (hidden) so the hook needs no compiler-rt/libgcc and
 * cannot interpose the ones anyka_ipc uses. isp_hook.c's w_frac divides by a variable. */
#if defined(__arm__)
__attribute__((visibility("hidden"), used))
unsigned long long __aeabi_uidivmod(unsigned n, unsigned d)
{
    unsigned q = 0, r = 0;
    if (!d) return 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return ((unsigned long long)r << 32) | q;
}
__attribute__((visibility("hidden"), used))
unsigned __aeabi_uidiv(unsigned n, unsigned d) { return (unsigned)__aeabi_uidivmod(n, d); }
#endif

#define _sc3(n, a, b, c) sc6((n), (a), (b), (c), 0, 0, 0)
#define _sc2(n, a, b)    sc6((n), (a), (b), 0, 0, 0, 0)

#define O_WRONLY        1
#define O_CREAT      0100
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_CLOEXEC 02000000
#define F_DUPFD_CLOEXEC 1030
#define EBADF           9

/* ---- legacy ISP/VENC/AKPCM decoders, same source as isp_hook.so ------------ */
static int isp_line_open(void);
static int isp_catchall_ok(uint32_t cmd, const void *payload);
#define ISP_HOOK_EMBED 1
#define ISP_LOG_OPEN()   isp_line_open()      /* opens the sink and writes "T=sss.mmm " */
#define ISP_LOG_CLOSE(f) ((void)(f))
#define ISP_CATCHALL_OK(c, p) isp_catchall_ok(c, p)  /* ISP_BLOCK/isp_blocks.bin carry the rest */
#include "isp_hook.c"
#undef open
#undef read
#undef write
#undef ioctl

#define HOOK_EXPORT __attribute__((visibility("default")))

/* Like libc: negative errno from the raw syscall becomes -1 + errno. */
extern int *__errno_location(void) __attribute__((weak));
static long fixret(long r)
{
    if ((unsigned long)r > (unsigned long)-4096L) {
        if (__errno_location) *__errno_location() = (int)-r;
        return -1;
    }
    return r;
}

/* ---- state -------------------------------------------------------------- */
#define MAXFD        512
#define LOG_FD_BASE  900            /* our own descriptors live far above stock's */
#define LB_MAX       512            /* line buffer on the CALLER's stack: keep it small, a vendor
                                     * thread may have a small stack (pthread_attr_setstacksize) */
/* Size caps (bytes). The *_TEST overrides exist for tests/stock_hook_test.sh only. */
#ifndef CAP_MAIN_SD
#define CAP_MAIN_SD (12u << 20)
#endif
#ifndef CAP_ISP_SD
#define CAP_ISP_SD  (12u << 20)
#endif
#ifndef CAP_TMP
#define CAP_TMP     (1u << 20)
#endif
#define RAW_CAP     65536u
#ifndef STATS_WIN_MS
#define STATS_WIN_MS 2000u
#endif
#ifndef ALIVE_MS
#define ALIVE_MS    10000u          /* ALIVE line and ISP_STAT counters every 10 s */
#endif
#define MAX_HEX      32u            /* bytes of ioctl argument shown */
/* ISP parameter blocks: full payload to isp_blocks.bin (binary), once per distinct content. */
#ifndef CAP_BIN_SD
#define CAP_BIN_SD  (24u << 20)         /* isp_blocks.bin on the card */
#endif
#ifndef CAP_BIN_TMP
#define CAP_BIN_TMP (1u << 20)          /* ... when the log fell back to RAM */
#endif
#ifndef CAP_BIN_CMD
#define CAP_BIN_CMD (2u << 20)          /* per ISP command */
#endif
#define ISP_BLOCK_MAX 65536u            /* sanity cap of one payload (the biggest known is 46584) */
#define ISP_PROBE      256u             /* bytes taken when the struct size of a command is unknown */
#define ISP_GET_MIN_MS 1000u            /* polled (read) blocks: at most one dump per second ... */
#define ISP_SET_MIN_MS  100u            /* ... written blocks: 10 per second */

enum { C_NONE = 0, C_PCM, C_GPIO, C_MEM, C_AUDIOFILE, C_OTHER };

struct fdi {
    uint8_t  cls;
    char     last;                  /* last character of the path: c/p/l on /dev/pcm* */
    uint8_t  raw_on;
    uint8_t  quiet;                 /* the OPEN of this sysfs file was not logged: neither is its CLOSE */
    uint32_t logged_wr, logged_rd;
    uint32_t rd_calls, rd_bytes, wr_calls, wr_bytes, errs;
    char     dev[48];
    uint32_t w_start, w_last, w_calls, w_bytes, w_gmin, w_gmax;
    char     w_op;
    uint32_t a_bytes, a_samples, a_peak, a_sumabs, a_zero, a_pairs, a_lreq, a_b00, a_bff, a_baw;
};
static struct fdi g_fdi[MAXFD];

static volatile int g_state;        /* 0 unknown, 1 active, 2 off (not anyka_ipc) */
static unsigned long g_t0;          /* monotonic ms at activation */
static int g_logfd = -1, g_ispfd = -1, g_rawfd = -1, g_pipe_r = -1, g_pipe_w = -1, g_binfd = -1;
static int g_dir_idx = -1;
static uint32_t g_main_bytes, g_cap_main, g_cap_isp, g_cap_bin, g_raw_bytes, g_bin_bytes;
static uint32_t g_isp_calls, g_isp_dumps, g_isp_skip, g_gpio_skip;
static int g_full;
static uint32_t g_drop, g_events, g_alive_ms, g_log_fail;

#ifdef STOCK_HOOK_DIRS_TEST
static const char *const k_dirs[] = { STOCK_HOOK_DIRS_TEST };   /* host test: 3 or more entries */
#else
static const char *const k_dirs[] = {
    "/tmp/stock_capture/out", "/tmp/sd/stock_capture", "/tmp"
};
#endif
#define NDIRS ((int)(sizeof k_dirs / sizeof k_dirs[0]))

/* ---- tiny string helpers ------------------------------------------------- */
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int has(const char *s, const char *needle) { return str_has(s, needle); }
static int ends_with(const char *s, const char *suf)
{
    unsigned a = slen(s), b = slen(suf);
    if (b > a) return 0;
    s += a - b;
    while (*suf) { if (*s++ != *suf++) return 0; }
    return 1;
}

/* ---- line builder -------------------------------------------------------- */
struct lb { char b[LB_MAX]; unsigned n; };
static void lb_c(struct lb *l, char c) { if (l->n < LB_MAX - 2) l->b[l->n++] = c; }
static void lb_s(struct lb *l, const char *s) { while (*s) lb_c(l, *s++); }
static void lb_u(struct lb *l, unsigned long v)
{
    char t[12]; int i = 11; t[i] = 0;
    if (!v) t[--i] = '0';
    while (v) { t[--i] = (char)('0' + v % 10); v /= 10; }
    lb_s(l, t + i);
}
static void lb_i(struct lb *l, long v)
{
    if (v < 0) { lb_c(l, '-'); lb_u(l, (unsigned long)(-v)); }
    else lb_u(l, (unsigned long)v);
}
static void lb_x(struct lb *l, unsigned long v)
{
    char t[12]; int i = 11; t[i] = 0;
    if (!v) t[--i] = '0';
    while (v) { int d = (int)(v & 15); t[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    lb_s(l, "0x"); lb_s(l, t + i);
}
static void lb_hexbytes(struct lb *l, const uint8_t *p, unsigned n)
{
    static const char hx[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; i++) { lb_c(l, hx[p[i] >> 4]); lb_c(l, hx[p[i] & 15]); }
}
static void lb_text(struct lb *l, const uint8_t *p, unsigned n)
{
    lb_c(l, '"');
    for (unsigned i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') lb_c(l, (char)c);
        else { lb_s(l, "\\x"); lb_hexbytes(l, &c, 1); }
    }
    lb_c(l, '"');
}

/* ---- time and thread id --------------------------------------------------- */
static unsigned long mono_ms(void)
{
    long ts[2] = { 0, 0 };
    _sc2(SYS_CLOCK_GETTIME, 1 /* CLOCK_MONOTONIC */, (long)ts);
    return (unsigned long)ts[0] * 1000ul + (unsigned long)ts[1] / 1000000ul;
}
static unsigned long now_ms(void) { return mono_ms() - g_t0; }

/* ---- log sink ------------------------------------------------------------- */
static int sink_open(int idx, const char *name, int fdbase)
{
    char p[160];
    unsigned n = 0;
    const char *d = k_dirs[idx];
    while (*d && n < sizeof p - 24) p[n++] = *d++;
    p[n++] = '/';
    const char *s = name;
    while (*s && n < sizeof p - 1) p[n++] = *s++;
    p[n] = 0;
    /* O_CLOEXEC: if the move above 900 fails (low RLIMIT_NOFILE) the descriptor must still not
     * leak into children. The directory is never created here: it is either the card folder
     * made by stock_capture.sh or /tmp. */
    long fd = _sc3(SYS_OPEN, (long)p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    long hi = sc6(SYS_FCNTL, fd, F_DUPFD_CLOEXEC, fdbase, 0, 0, 0);
    if (hi >= 0) { _sc2(SYS_CLOSE, fd, 0); fd = hi; }
    return (int)fd;
}

static int log_pick(void)
{
    if ((g_log_fail++ & 0xff) != 0 && g_log_fail > 8) return 0;   /* back off when hopeless */
    for (int i = 0; i < NDIRS; i++) {
        int fd = sink_open(i, "stock_hook.log", LOG_FD_BASE);
        if (fd >= 0) {
            if (g_logfd >= 0 && g_logfd != fd) _sc2(SYS_CLOSE, g_logfd, 0);
            g_logfd = fd;
            if (g_dir_idx != i) {
                g_dir_idx = i;
                g_main_bytes = 0;
                g_cap_main = (i == NDIRS - 1) ? CAP_TMP : CAP_MAIN_SD;
                g_cap_isp  = (i == NDIRS - 1) ? CAP_TMP : CAP_ISP_SD;
                g_cap_bin  = (i == NDIRS - 1) ? CAP_BIN_TMP : CAP_BIN_SD;
                g_bin_bytes = 0;
                if (g_ispfd >= 0) { _sc2(SYS_CLOSE, g_ispfd, 0); g_ispfd = -1; }
                if (g_binfd >= 0) { _sc2(SYS_CLOSE, g_binfd, 0); g_binfd = -1; }
            }
            g_log_fail = 0;
            return 1;
        }
    }
    return 0;
}

static void emit(struct lb *l)
{
    if (g_full) return;
    l->b[l->n++] = '\n';
    g_events++;
    if (g_logfd < 0 && !log_pick()) { g_drop++; return; }
    long w = _sc3(SYS_WRITE, g_logfd, (long)l->b, l->n);
    if (w < 0) {                                      /* stock closed it, card removed ... */
        if (w != -EBADF) _sc2(SYS_CLOSE, g_logfd, 0);   /* EIO of a pulled card: do not leak it */
        g_logfd = -1;
        if (log_pick()) w = _sc3(SYS_WRITE, g_logfd, (long)l->b, l->n);
    }
    if (w > 0) {
        g_main_bytes += (uint32_t)w;
        if (g_main_bytes > g_cap_main) {
            static const char m[] = "LOG_CAP reached, further stock_hook.log events dropped\n";
            _sc3(SYS_WRITE, g_logfd, (long)m, sizeof m - 1);
            g_full = 1;
        }
    } else g_drop++;
}

static int isp_sink_open(void)
{
    if (g_state != 1) return -1;
    if (g_dir_idx < 0 && !log_pick()) return -1;
    if (g_ispfd >= 0 && _sc3(SYS_LSEEK, g_ispfd, 0, 1) < 0) g_ispfd = -1;   /* closed by stock */
    if (g_ispfd < 0) {
        g_ispfd = sink_open(g_dir_idx, "isp_hook.log", LOG_FD_BASE + 1);
        if (g_ispfd < 0) return -1;
    }
    long pos = _sc3(SYS_LSEEK, g_ispfd, 0, 1);
    if (pos < 0 || (unsigned long)pos > g_cap_isp) return -1;
    return g_ispfd;
}

static void raw_audio(const uint8_t *p, unsigned n)
{
    if (g_raw_bytes >= RAW_CAP || g_dir_idx < 0) return;
    if (g_rawfd < 0) {
        g_rawfd = sink_open(g_dir_idx, "stock_audio.raw", LOG_FD_BASE + 2);
        if (g_rawfd < 0) return;
    }
    if (n > RAW_CAP - g_raw_bytes) n = RAW_CAP - g_raw_bytes;
    long w = _sc3(SYS_WRITE, g_rawfd, (long)p, n);
    if (w > 0) g_raw_bytes += (uint32_t)w; else g_rawfd = -1;
}

/* ---- safe copy of user memory (EFAULT instead of a crash) ---------------- */
static unsigned safe_copy(void *dst, const void *src, unsigned n)
{
    if (!src || !n) return 0;
    if (g_pipe_w < 0) {
        int p[2] = { -1, -1 };
        if (sc6(SYS_PIPE2, (long)p, O_NONBLOCK | O_CLOEXEC, 0, 0, 0, 0) < 0) return 0;
        long r = sc6(SYS_FCNTL, p[0], F_DUPFD_CLOEXEC, LOG_FD_BASE + 4, 0, 0, 0);
        long w = sc6(SYS_FCNTL, p[1], F_DUPFD_CLOEXEC, LOG_FD_BASE + 5, 0, 0, 0);
        _sc2(SYS_CLOSE, p[0], 0); _sc2(SYS_CLOSE, p[1], 0);
        if (r < 0 || w < 0) return 0;
        g_pipe_r = (int)r; g_pipe_w = (int)w;
    }
    uint8_t drain[8];
    while (_sc3(SYS_READ, g_pipe_r, (long)drain, sizeof drain) > 0) { }
    long w = _sc3(SYS_WRITE, g_pipe_w, (long)src, n);
    /* a copy crossing into an unmapped page may fail as a whole: retry shorter */
    while (w <= 0 && n > 4) { n >>= 1; w = _sc3(SYS_WRITE, g_pipe_w, (long)src, n); }
    if (w <= 0) return 0;
    long r = _sc3(SYS_READ, g_pipe_r, (long)dst, w);
    return r > 0 ? (unsigned)r : 0;
}

static uint32_t fnv(const uint8_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* ---- activation ----------------------------------------------------------- */
static void head(struct lb *l, const char *ev)
{
    lb_s(l, "T="); unsigned long t = now_ms();
    lb_u(l, t / 1000); lb_c(l, '.');
    unsigned ms = (unsigned)(t % 1000);
    lb_c(l, (char)('0' + ms / 100)); lb_c(l, (char)('0' + (ms / 10) % 10)); lb_c(l, (char)('0' + ms % 10));
    lb_s(l, " tid="); lb_u(l, (unsigned long)_sc2(SYS_GETTID, 0, 0));
    lb_c(l, ' '); lb_s(l, ev);
}

/* isp_hook.log: the legacy decoders write their lines piece by piece; every line they start
 * gets a "T=sss.mmm " stamp first (same clock as stock_hook.log) so the AE / WB / sensor
 * lines can be lined up with the GPIO and ISP block events. */
static int isp_line_open(void)
{
    int f = isp_sink_open();
    if (f < 0) return f;
    char b[24];
    unsigned n = 0;
    unsigned long t = now_ms();
    unsigned long sec = t / 1000;
    unsigned ms = (unsigned)(t % 1000);
    char d[12];
    int i = 11;
    d[i] = 0;
    if (!sec) d[--i] = '0';
    while (sec) { d[--i] = (char)('0' + sec % 10); sec /= 10; }
    b[n++] = 'T'; b[n++] = '=';
    while (d[i]) b[n++] = d[i++];
    b[n++] = '.';
    b[n++] = (char)('0' + ms / 100);
    b[n++] = (char)('0' + (ms / 10) % 10);
    b[n++] = (char)('0' + ms % 10);
    b[n++] = ' ';
    _sc3(SYS_WRITE, f, (long)b, n);
    return f;
}

/* The legacy catch-all prints every other ISP command with 4 words; ISP_BLOCK has the full
 * block, so isp_hook.log keeps only the first lines per command (names stay greppable). */
static unsigned readable_len(const void *p, unsigned n);
static int isp_catchall_ok(uint32_t cmd, const void *payload)
{
    static uint8_t seen[256];
    if (readable_len(payload, 16) < 16) return 0;      /* the catch-all reads 4 words */
    unsigned i = (cmd ^ (cmd >> 8) ^ (cmd >> 16)) & 255u;
    if (seen[i] >= 3) return 0;
    seen[i]++;
    return 1;
}

/* Does this command line (NUL separated or not) look like one that restarts or resets the board? */
static int cmd_is_fatal(const char *c)
{
    return has(c, "reboot") || has(c, "recover") || has(c, "poweroff") || has(c, "halt") ||
           has(c, "reset") || has(c, "killall") || has(c, "kill ");
}

/* A process started by anyka_ipc (system(), popen()) inherits LD_PRELOAD, loads this library
 * and lands here: one CHILD line with its command line, nothing else, then it stays silent.
 * This is how a "reboot -f" or "sh recover_cfg.sh" launched by the stock app shows up (the
 * stock binary imports system() and popen(); libc does not call an interposable execve).
 * T of a CHILD line is the absolute uptime (the child does not know the start of its parent):
 * abs_ms= is given, the parser subtracts mono_ms of HOOK_START. */
__attribute__((noinline)) static void child_note(const char *exe, unsigned exelen)
{
    uint8_t cl[129];
    long n = 0;
    long fd = _sc3(SYS_OPEN, (long)"/proc/self/cmdline", 0 /* O_RDONLY */, 0);
    if (fd >= 0) {
        n = _sc3(SYS_READ, fd, (long)cl, sizeof cl);
        _sc2(SYS_CLOSE, fd, 0);
    }
    if (n < 0) n = 0;
    for (long i = 0; i < n; i++) if (cl[i] == 0) cl[i] = ' ';
    while (n > 0 && cl[n - 1] == ' ') n--;
    cl[n] = 0;
    struct lb l; l.n = 0;
    head(&l, "CHILD");
    lb_s(&l, " abs_ms="); lb_u(&l, now_ms());
    lb_s(&l, " ppid="); lb_u(&l, (unsigned long)_sc2(SYS_GETPPID, 0, 0));
    lb_s(&l, " exe="); lb_text(&l, (const uint8_t *)exe, exelen > 48 ? 48 : exelen);
    lb_s(&l, " cmd="); lb_text(&l, cl, (unsigned)n);
    emit(&l);
    /* A reboot / reset command is the last thing this system does: its line must not stay in the
     * page cache. Only these (rare, short-lived) children fsync; nothing else in the hook does. */
    if (g_logfd >= 0 && cmd_is_fatal((const char *)cl)) _sc2(SYS_FSYNC, g_logfd, 0);
}

/* The stock app starts its helpers with system() and popen(): "reboot -f", "sh recover_cfg.sh &",
 * "echo N > /tmp/camera_start_times". On the camera /bin/sh is a STATIC BusyBox (checked in the
 * vendor rootfs), which ignores LD_PRELOAD, so the CHILD path above never sees those. They are
 * therefore logged from inside anyka_ipc, by the exported system() and popen() below: one CHILD
 * line with via=system|popen and the command, written BEFORE the real call runs (it forwards
 * everything to libc's function, found with dlsym(RTLD_NEXT); the arguments, the return value and
 * errno are the real ones). A command that may reboot the board is fsync'd first (the only fsync
 * of the hook; such a command is rare and nothing is flushed after a reboot). */
#define CMD_MAX 200u
__attribute__((noinline)) static void cmd_note(const char *via, const char *cmd)
{
    struct lb l; l.n = 0;
    head(&l, "CHILD");
    lb_s(&l, " abs_ms="); lb_u(&l, g_t0 + now_ms());
    lb_s(&l, " via="); lb_s(&l, via);
    lb_s(&l, " exe=\"/bin/sh\" cmd=");
    unsigned n = 0;
    if (cmd) {
        while (n < CMD_MAX && cmd[n]) n++;
        lb_text(&l, (const uint8_t *)cmd, n);
        if (cmd[n]) lb_s(&l, "...");
    } else {
        lb_s(&l, "\"(null)\"");
    }
    emit(&l);
    if (cmd && g_logfd >= 0 && cmd_is_fatal(cmd)) _sc2(SYS_FSYNC, g_logfd, 0);
}

__attribute__((noinline)) static void hook_init(void)
{
    char exe[160];
    long n = _sc3(SYS_READLINK, (long)"/proc/self/exe", (long)exe, sizeof exe - 1);
    int match = 0;
    if (n > 0) {
        exe[n] = 0;
        unsigned i = 0, base = 0;
        while (exe[i]) { if (exe[i] == '/') base = i + 1; i++; }
        const char *c = STOCK_HOOK_COMM;
        const char *e = exe + base;
        while (*c && *e == *c) { c++; e++; }
        match = (*c == 0);                       /* prefix: tolerates " (deleted)" */
    }
    if (!match) {
        g_state = 2;
        child_note(exe, n > 0 ? (unsigned)n : 0);
        return;
    }
    g_t0 = mono_ms();
    g_alive_ms = 0;
    g_state = 1;
    long ts[2] = { 0, 0 };
    _sc2(SYS_CLOCK_GETTIME, 0 /* CLOCK_REALTIME */, (long)ts);
    struct lb l; l.n = 0;
    head(&l, "HOOK_START");
    lb_s(&l, " version=2 arch="
#if defined(__arm__)
             "arm"
#else
             "x86_64"
#endif
             " comm=" STOCK_HOOK_COMM " pid=");
    lb_u(&l, (unsigned long)_sc2(SYS_GETTID, 0, 0));
    lb_s(&l, " wall="); lb_u(&l, (unsigned long)ts[0]);
    lb_s(&l, " mono_ms="); lb_u(&l, g_t0);          /* CLOCK_MONOTONIC = /proc/uptime at T=0 */
    emit(&l);
    if (g_dir_idx >= 0) {
        l.n = 0; head(&l, "HOOK_SINK"); lb_s(&l, " dir="); lb_s(&l, k_dirs[g_dir_idx]);
        lb_s(&l, " cap="); lb_u(&l, g_cap_main);
        emit(&l);
    }
}
static int hook_on(void)
{
    int s = g_state;
    if (!s) { hook_init(); s = g_state; }
    return s == 1;
}

static void isp_stats_flush(void);
/* Every function that owns a 512-byte line buffer is a leaf of the call chain (noinline, and
 * the caller keeps no buffer of its own while it runs): the hook's stack use is the frame of
 * the hooked function plus ONE buffer, never two. */
__attribute__((noinline)) static void alive_line(void)
{
    struct lb l; l.n = 0;
    head(&l, "ALIVE"); lb_s(&l, " events="); lb_u(&l, g_events);
    lb_s(&l, " dropped="); lb_u(&l, g_drop);
    lb_s(&l, " isp_calls="); lb_u(&l, g_isp_calls);
    lb_s(&l, " isp_dumps="); lb_u(&l, g_isp_dumps);
    lb_s(&l, " isp_skipped="); lb_u(&l, g_isp_skip);
    lb_s(&l, " gpio_skipped="); lb_u(&l, g_gpio_skip);
    lb_s(&l, " bin_bytes="); lb_u(&l, g_bin_bytes);
    lb_s(&l, " log_bytes="); lb_u(&l, g_main_bytes);
    emit(&l);
}
static void alive(void)
{
    unsigned long t = now_ms();
    if (t - g_alive_ms < ALIVE_MS) return;
    g_alive_ms = (uint32_t)t;
    alive_line();
    isp_stats_flush();
}

static struct fdi *fdi_get(int fd)
{
    if (g_state != 1 || fd < 0 || fd >= MAXFD || !g_fdi[fd].cls) return 0;
    return &g_fdi[fd];
}

/* ---- path classification --------------------------------------------------- */
static int classify(const char *p)
{
    if (!p) return C_NONE;
    if (has(p, "/dev/pcm") || has(p, "/dev/akpcm") || has(p, "/dev/snd/")) return C_PCM;
    if (has(p, "/sys/class/gpio") || has(p, "/dev/gpio") || has(p, "/dev/ak_gpio")) return C_GPIO;
    /* day/night: light sensor (ADC), IR LED (PWM or LED class) */
    if (has(p, "/sys/bus/iio/") || has(p, "/sys/class/pwm") || has(p, "/sys/class/leds") ||
        has(p, "/sys/devices/platform/leds")) return C_GPIO;
    if (has(p, "/dev/mem")) return C_MEM;
    if (ends_with(p, ".mp3") || ends_with(p, ".wav") || ends_with(p, ".pcm") ||
        ends_with(p, ".aac") || ends_with(p, ".amr") || ends_with(p, ".opus") ||
        ends_with(p, ".g711") || ends_with(p, ".ulaw") || ends_with(p, ".alaw") ||
        has(p, "/usr/share/")) return C_AUDIOFILE;
    if (has(p, "_ht_") || has(p, "isp_") || has(p, "_isp") || has(p, "/dev/isp") ||
        has(p, "/etc/config/") || has(p, "/usr/local/")) return C_OTHER;
    return C_NONE;
}
static const char *cls_name(int c)
{
    switch (c) {
    case C_PCM: return "pcm"; case C_GPIO: return "gpio"; case C_MEM: return "mem";
    case C_AUDIOFILE: return "audiofile"; case C_OTHER: return "other";
    default: return "none";
    }
}

static void fdi_copy(struct fdi *d, const struct fdi *s)
{
    unsigned char *a = (unsigned char *)d;
    const unsigned char *b = (const unsigned char *)s;
    for (unsigned i = 0; i < sizeof *d; i++) a[i] = b[i];
}

static void track(int fd, int cls, const char *path)
{
    if (fd < 0 || fd >= MAXFD) return;
    struct fdi *f = &g_fdi[fd];
    unsigned char *z = (unsigned char *)f;
    for (unsigned i = 0; i < sizeof *f; i++) z[i] = 0;
    f->cls = (uint8_t)cls;
    unsigned n = slen(path);
    f->last = n ? path[n - 1] : 0;
    unsigned k = 0;
    if (n > sizeof f->dev - 1) path += n - (sizeof f->dev - 1);   /* keep the tail */
    while (path[k] && k < sizeof f->dev - 1) { f->dev[k] = path[k]; k++; }
    f->dev[k] = 0;
}

/* ---- ioctl throttle table ---------------------------------------------------- */
#define NTHR 96
static struct { int fd; unsigned req; uint32_t n, hash; } g_thr[NTHR];
static uint32_t thr_hit(int fd, unsigned req, uint32_t hash, int *changed)
{
    int free_i = -1;
    for (int i = 0; i < NTHR; i++) {
        if (g_thr[i].n && g_thr[i].fd == fd && g_thr[i].req == req) {
            g_thr[i].n++;
            *changed = (g_thr[i].hash != hash);
            g_thr[i].hash = hash;
            return g_thr[i].n;
        }
        if (!g_thr[i].n && free_i < 0) free_i = i;
    }
    if (free_i < 0) free_i = (int)((unsigned)(fd * 31) + req) % NTHR;
    g_thr[free_i].fd = fd; g_thr[free_i].req = req; g_thr[free_i].n = 1; g_thr[free_i].hash = hash;
    *changed = 1;
    return 1;
}
static void thr_clear(int fd)
{
    for (int i = 0; i < NTHR; i++) if (g_thr[i].n && g_thr[i].fd == fd) g_thr[i].n = 0;
}

/* ---- sysfs text I/O throttle (GPIO, ADC light sensor, PWM, LED) ------------------------------
 * The stock app opens, reads or writes and closes these files over and over (a light sensor
 * poll, an IR LED pulse). Per file and operation: the first few events are always logged,
 * a write whose text differs from the previous one is always logged, identical ones every
 * 50th time, reads at most once per second; failures are always logged. */
#define NTX 48
static struct { uint32_t key, h, n, last_ms; } g_tx[NTX];
static uint32_t g_tx_seen;
static int tx_should_log(const char *dev, char op, const uint8_t *buf, long r)
{
    if (r < 0) return 1;
    uint32_t key = fnv((const uint8_t *)dev, slen(dev)) + (uint32_t)(uint8_t)op;
    int slot = -1;
    for (int i = 0; i < NTX; i++) {
        if (g_tx[i].n && g_tx[i].key == key) { slot = i; break; }
        if (!g_tx[i].n && slot < 0) slot = i;
    }
    if (slot < 0) slot = (int)(key % NTX);                     /* table full: recycle a slot */
    if (g_tx[slot].key != key || !g_tx[slot].n) { g_tx[slot].key = key; g_tx[slot].n = 0; g_tx[slot].h = 0; g_tx[slot].last_ms = 0; }
    uint32_t n = ++g_tx[slot].n;
    g_tx_seen = n;
    uint32_t h = (buf && r > 0) ? fnv(buf, r > 32 ? 32u : (unsigned)r) : 0;
    unsigned long t = now_ms();
    int log;
    if (op == 'w') log = n <= 8 || h != g_tx[slot].h || (n % 50) == 0;
    else if (op == 'r') log = n <= 4 || t - g_tx[slot].last_ms >= 1000ul;
    else log = n <= 8 || (n % 50) == 0;
    if (log) { g_tx[slot].h = h; g_tx[slot].last_ms = (uint32_t)t; } else g_gpio_skip++;
    return log;
}

/* ---- audio statistics ---------------------------------------------------- */
static void analyze(struct fdi *f, const uint8_t *b, unsigned n)
{
    if (n > 2048) n = 2048;
    f->a_bytes += n;
    for (unsigned i = 0; i < n; i++) {
        if (b[i] == 0) f->a_b00++;
        else if (b[i] == 0xff || b[i] == 0x7f) f->a_bff++;
        else if (b[i] == 0xd5 || b[i] == 0x55) f->a_baw++;
    }
    for (unsigned i = 0; i + 1 < n; i += 2) {
        int16_t s = (int16_t)(b[i] | (b[i + 1] << 8));
        uint32_t m = (uint32_t)(s < 0 ? -(int32_t)s : s);
        if (m > f->a_peak) f->a_peak = m;
        f->a_sumabs += m;
        if (!s) f->a_zero++;
        f->a_samples++;
    }
    for (unsigned i = 0; i + 3 < n; i += 4) {
        f->a_pairs++;
        if (b[i] == b[i + 2] && b[i + 1] == b[i + 3]) f->a_lreq++;
    }
}

__attribute__((noinline)) static void stats_flush(int fd, struct fdi *f, unsigned long t, const char *why)
{
    if (!f->w_calls) return;
    struct lb l; l.n = 0;
    head(&l, "AUDIO_STATS");
    lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
    lb_s(&l, " dev="); lb_s(&l, f->dev);
    lb_s(&l, " op="); lb_c(&l, f->w_op);
    lb_s(&l, " why="); lb_s(&l, why);
    lb_s(&l, " win_ms="); lb_u(&l, t - f->w_start);
    lb_s(&l, " calls="); lb_u(&l, f->w_calls);
    lb_s(&l, " bytes="); lb_u(&l, f->w_bytes);
    lb_s(&l, " gap_ms_min="); lb_u(&l, f->w_gmin == 0xffffffffu ? 0 : f->w_gmin);
    lb_s(&l, " gap_ms_max="); lb_u(&l, f->w_gmax);
    lb_s(&l, " an_bytes="); lb_u(&l, f->a_bytes);
    lb_s(&l, " samples="); lb_u(&l, f->a_samples);
    lb_s(&l, " peak="); lb_u(&l, f->a_peak);
    lb_s(&l, " sumabs="); lb_u(&l, f->a_sumabs);
    lb_s(&l, " zero16="); lb_u(&l, f->a_zero);
    lb_s(&l, " pairs="); lb_u(&l, f->a_pairs);
    lb_s(&l, " lr_eq="); lb_u(&l, f->a_lreq);
    lb_s(&l, " b00="); lb_u(&l, f->a_b00);
    lb_s(&l, " bff="); lb_u(&l, f->a_bff);
    lb_s(&l, " baw="); lb_u(&l, f->a_baw);
    emit(&l);
    f->w_calls = 0; f->w_bytes = 0; f->w_gmin = 0xffffffffu; f->w_gmax = 0;
    f->a_bytes = f->a_samples = f->a_peak = f->a_sumabs = f->a_zero = 0;
    f->a_pairs = f->a_lreq = f->a_b00 = f->a_bff = f->a_baw = 0;
}

static void win_update(int fd, struct fdi *f, char op, unsigned bytes, const uint8_t *data)
{
    unsigned long t = now_ms();
    if (!f->w_calls) {
        f->w_start = (uint32_t)t; f->w_gmin = 0xffffffffu; f->w_gmax = 0; f->w_last = 0;
    }
    f->w_op = op;
    if (f->w_last) {
        uint32_t gap = (uint32_t)t - f->w_last;
        if (gap < f->w_gmin) f->w_gmin = gap;
        if (gap > f->w_gmax) f->w_gmax = gap;
    }
    f->w_last = (uint32_t)t ? (uint32_t)t : 1;
    f->w_calls++; f->w_bytes += bytes;
    if (data && bytes) analyze(f, data, bytes);
    if (t - f->w_start >= STATS_WIN_MS) stats_flush(fd, f, t, "window");
}

/* ---- events: open / close ----------------------------------------------------- */
__attribute__((noinline)) static void on_open(const char *path, int flags, int mode, long r)
{
    if (!hook_on()) return;
    int cls = classify(path);
    if (!cls) return;
    if (r >= 0) track((int)r, cls, path);
    if (cls == C_GPIO && !tx_should_log(path, 'o', 0, r)) {
        if (r >= 0 && r < MAXFD) g_fdi[r].quiet = 1;
        return;
    }
    struct lb l; l.n = 0;
    head(&l, "OPEN");
    lb_s(&l, " path="); lb_text(&l, (const uint8_t *)path, slen(path) > 96 ? 96 : slen(path));
    lb_s(&l, " flags="); lb_x(&l, (unsigned long)flags);
    lb_s(&l, " mode="); lb_x(&l, (unsigned long)mode);
    lb_s(&l, " cls="); lb_s(&l, cls_name(cls));
    if (r < 0) { lb_s(&l, " ret=-1 errno="); lb_u(&l, (unsigned long)-r); }
    else { lb_s(&l, " fd="); lb_u(&l, (unsigned long)r); }
    emit(&l);
    alive();
}

static void close_line(int fd, struct fdi *f, long r);
static void on_close(int fd, long r)
{
    struct fdi *f = fdi_get(fd);
    if (!f) return;
    if (f->quiet) { f->cls = 0; thr_clear(fd); return; }
    unsigned long t = now_ms();
    stats_flush(fd, f, t, "close");
    close_line(fd, f, r);
    f->cls = 0;
    thr_clear(fd);
}
__attribute__((noinline)) static void close_line(int fd, struct fdi *f, long r)
{
    struct lb l; l.n = 0;
    head(&l, "CLOSE");
    lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
    lb_s(&l, " dev="); lb_s(&l, f->dev);
    lb_s(&l, " ret="); lb_i(&l, r < 0 ? -1 : r);
    lb_s(&l, " wr_calls="); lb_u(&l, f->wr_calls);
    lb_s(&l, " wr_bytes="); lb_u(&l, f->wr_bytes);
    lb_s(&l, " rd_calls="); lb_u(&l, f->rd_calls);
    lb_s(&l, " rd_bytes="); lb_u(&l, f->rd_bytes);
    lb_s(&l, " errs="); lb_u(&l, f->errs);
    emit(&l);
}

/* ---- events: read / write ---------------------------------------------------------- */
__attribute__((noinline)) static void io_event(const char *ev, int fd, struct fdi *f, const uint8_t *buf,
                     unsigned long count, long r, int full)
{
    struct lb l; l.n = 0;
    head(&l, ev);
    lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
    lb_s(&l, " dev="); lb_s(&l, f->dev);
    lb_s(&l, " count="); lb_u(&l, count);
    if (r < 0) { lb_s(&l, " ret=-1 errno="); lb_u(&l, (unsigned long)-r); }
    else { lb_s(&l, " ret="); lb_u(&l, (unsigned long)r); }
    if (r > 0 && buf) {
        unsigned n = (unsigned)r;
        if (f->cls == C_GPIO) {
            lb_s(&l, " text="); lb_text(&l, buf, n > 32 ? 32 : n);
            if (g_tx_seen > 8) { lb_s(&l, " seen="); lb_u(&l, g_tx_seen); }
        } else if (full) {
            lb_s(&l, " head="); lb_hexbytes(&l, buf, n > 32 ? 32 : n);
        }
    }
    emit(&l);
}

__attribute__((noinline)) static void audio_dump_line(int fd)
{
    struct lb l; l.n = 0;
    head(&l, "AUDIO_DUMP"); lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
    lb_s(&l, " file=stock_audio.raw raw_offset="); lb_u(&l, g_raw_bytes);
    emit(&l);
}

static void on_write(int fd, const void *buf, unsigned long count, long r)
{
    struct fdi *f = fdi_get(fd);
    if (!f) return;
    f->wr_calls++;
    if (r > 0) f->wr_bytes += (uint32_t)r; else f->errs++;
    int bad = (r < 0 || (unsigned long)r != count);
    switch (f->cls) {
    case C_PCM:
        if (r > 0) {
            win_update(fd, f, 'w', (unsigned)r, (const uint8_t *)buf);
            if (f->last == 'p') {
                if (!f->raw_on && g_raw_bytes < RAW_CAP) {
                    f->raw_on = 1;
                    audio_dump_line(fd);
                }
                if (f->raw_on) raw_audio((const uint8_t *)buf, (unsigned)r);
            }
        }
        if (f->logged_wr < 16 || (bad && f->errs <= 50)) {
            f->logged_wr++;
            io_event("WRITE", fd, f, (const uint8_t *)buf, count, r, 1);
        }
        break;
    case C_GPIO:
        if (tx_should_log(f->dev, 'w', (const uint8_t *)buf, r))
            io_event("WRITE", fd, f, (const uint8_t *)buf, count, r, 0);
        break;
    default:
        break;                                    /* counted, reported at close */
    }
    alive();
}

static void on_read(int fd, const void *buf, unsigned long count, long r)
{
    struct fdi *f = fdi_get(fd);
    if (!f) return;
    f->rd_calls++;
    if (r > 0) f->rd_bytes += (uint32_t)r; else f->errs++;
    int bad = (r < 0);
    switch (f->cls) {
    case C_PCM:
        if (r > 0) win_update(fd, f, 'r', (unsigned)r, (const uint8_t *)buf);
        if (f->logged_rd < 6 || (bad && f->errs <= 50)) {
            f->logged_rd++;
            io_event("READ", fd, f, (const uint8_t *)buf, count, r, 1);
        }
        break;
    case C_GPIO:
        if (tx_should_log(f->dev, 'r', (const uint8_t *)buf, r))
            io_event("READ", fd, f, (const uint8_t *)buf, count, r, 0);
        break;
    default:
        break;
    }
    alive();
}

/* ---- events: ioctl --------------------------------------------------------------------- */
__attribute__((noinline)) static void on_ioctl_tracked(int fd, struct fdi *f, unsigned req, void *arg,
                             const uint8_t *pre, unsigned npre, long r)
{
    unsigned sz = (req >> 16) & 0x3fffu;
    uint8_t post[MAX_HEX];
    unsigned npost = 0;
    if (sz && arg) npost = safe_copy(post, arg, sz > MAX_HEX ? MAX_HEX : sz);
    /* "Value changed" detection only for calls that leave the argument untouched (pure
     * inputs such as SET_GAIN); outputs (timestamps...) would always differ. */
    unsigned same = (npost == npre);
    for (unsigned i = 0; same && i < npost; i++) if (post[i] != pre[i]) same = 0;
    uint32_t key = (sz && arg) ? (same ? fnv(pre, npre) : 0u) : (uint32_t)(unsigned long)arg;
    int changed = 0;
    uint32_t n = thr_hit(fd, req, key, &changed);
    if (!(n <= 24 || changed || (n % 200) == 0 || (r < 0 && n <= 32))) return;
    struct lb l; l.n = 0;
    head(&l, "IOCTL");
    lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
    lb_s(&l, " dev="); lb_s(&l, f->dev);
    lb_s(&l, " req="); lb_x(&l, req);
    lb_s(&l, " dir="); lb_u(&l, (req >> 30) & 3u);
    lb_s(&l, " type="); lb_x(&l, (req >> 8) & 0xffu);
    lb_s(&l, " nr="); lb_x(&l, req & 0xffu);
    lb_s(&l, " size="); lb_u(&l, sz);
    if (sz && arg) {
        lb_s(&l, " in="); lb_hexbytes(&l, pre, npre);
        lb_s(&l, " out=");
        if (same) lb_s(&l, "same"); else lb_hexbytes(&l, post, npost);
    } else {
        lb_s(&l, " arg="); lb_x(&l, (unsigned long)arg);
    }
    if (r < 0) { lb_s(&l, " ret=-1 errno="); lb_u(&l, (unsigned long)-r); }
    else { lb_s(&l, " ret="); lb_u(&l, (unsigned long)r); }
    if (n > 24) { lb_s(&l, " seen="); lb_u(&l, n); }
    emit(&l);
}

/* ---- ISP parameter blocks ------------------------------------------------------------
 * Every ISP tunnel call (ioctl 0xc0cc5616) carries {flag, inner_cmd, payload pointer}; the
 * inner command number does not say how long the payload is. The sizes below are the real
 * struct sizes: the SET blocks are the module blocks of the stock sensor ISP file (the same
 * sizes ak_rtsp/isp.c sends), the GET ones come from ak_ioctls.h. Unknown commands are probed
 * with ISP_PROBE bytes. The payload is hashed and, when its content is new (and not more often
 * than ISP_*_MIN_MS per command), written in FULL to isp_blocks.bin with ONE writev() that
 * takes the bytes straight from the application's memory: no copy, no big stack frame, and a
 * record never interleaves with another thread's. Record layout, little endian:
 *   "ISPB" t_ms cmd len hash tid   (6 x 4 bytes)   followed by len payload bytes
 * Hot polled commands are counted, not dumped (ISP_STAT lines every 10 s with the counters).
 * Reading the payload directly is guarded by mincore(): an unmapped range is never touched. */
struct isp_sz { uint32_t cmd; uint32_t size; };
static const struct isp_sz k_isp_sz[] = {
    { 0x40044902u, 312 },   { 0x40044904u, 208 },   { 0x4004490au, 884 },   /* BLC LSC RAW_LUT */
    { 0x4004490cu, 3064 },  { 0x4004492bu, 992 },   { 0x4004497du, 720 },   /* NR1 NR2 UVNR */
    { 0x4004492fu, 7076 },  { 0x40044906u, 140 },   { 0x4004490eu, 18 },    /* 3D_NR GB DEMO */
    { 0x4004491au, 1746 },  { 0x40044912u, 324 },   { 0x40044936u, 176 },   /* RGB_GAMMA CCM FCS */
    { 0x4004491bu, 16936 }, { 0x40044925u, 46584 }, { 0x40044927u, 20 },    /* WDR SHARP SHARP_EX */
    { 0x4004493bu, 344 },   { 0x40044939u, 104 },   { 0x4004493fu, 4 },     /* SATURATION CONTRAST RGB2YUV */
    { 0x40044941u, 10 },    { 0x40044910u, 208 },   { 0x8004498eu, 1500 },  /* EFFECT DPC LCE */
    { 0x40044960u, 42 },    { 0x80044986u, 2016 },  { 0x80044987u, 2016 },  /* AF SET_WB GET_WB */
    { 0x40044973u, 5124 },  { 0x80044988u, 64 },    { 0x8004498au, 1824 },  /* AWB_EX AWB_CALIB SET_EXPO */
    { 0x8004498bu, 1824 },  { 0x4004494eu, 24 },    { 0x8004494fu, 24 },    /* GET_EXPO SET/GET_FRAME_RATE */
    { 0x4004496eu, 38 },    { 0x40044975u, 302 },   { 0x40044977u, 2204 },  /* MISC Y_GAMMA HUE */
    { 0x80044952u, 36 },    { 0x80044991u, 192 },   { 0x40044981u, 16 },    /* AE_RUN_INFO AE_STAT SENSOR_AE */
    { 0x80044984u, 4 },     { 0x40044967u, 8 },
};
static unsigned isp_size_of(uint32_t cmd)
{
    for (unsigned i = 0; i < sizeof k_isp_sz / sizeof k_isp_sz[0]; i++)
        if (k_isp_sz[i].cmd == cmd) return k_isp_sz[i].size;
    return ISP_PROBE;
}

/* How many bytes starting at p can be read without a fault (page granularity, mincore). */
static unsigned readable_len(const void *p, unsigned n)
{
    unsigned long a = (unsigned long)p;
    unsigned long base = a & ~4095ul;
    unsigned long end = a + n;
    uint8_t vec[24];
    if ((end - base + 4095ul) / 4096ul > sizeof vec) return 0;
    if (_sc3(SYS_MINCORE, (long)base, (long)(end - base), (long)vec) == 0) return n;
    /* the tail is not mapped: keep what is left of the first page, if that is mapped */
    unsigned first = (unsigned)(4096ul - (a - base));
    if (first >= n) return 0;
    if (_sc3(SYS_MINCORE, (long)base, 4096, (long)vec) == 0) return first;
    return 0;
}

static int bin_open(void)
{
    if (g_binfd >= 0) return g_binfd;
    if (g_dir_idx < 0 && !log_pick()) return -1;
    g_binfd = sink_open(g_dir_idx, "isp_blocks.bin", LOG_FD_BASE + 3);
    return g_binfd;
}

#define NISP 64
static struct { uint32_t cmd, calls, changes, dumps, last_seen, last_dumped, last_ms, bytes, reported; } g_isp[NISP];

__attribute__((noinline)) static void isp_stats_flush(void)       /* called from alive(): counters of the commands that moved */
{
    for (int i = 0; i < NISP; i++) {
        if (!g_isp[i].calls || g_isp[i].calls == g_isp[i].reported) continue;
        g_isp[i].reported = g_isp[i].calls;
        const char *name = isp_cmd_name(g_isp[i].cmd);
        struct lb l; l.n = 0;
        head(&l, "ISP_STAT");
        lb_s(&l, " name="); lb_s(&l, name ? name : "UNKNOWN");
        lb_s(&l, " cmd="); lb_x(&l, g_isp[i].cmd);
        lb_s(&l, " size="); lb_u(&l, isp_size_of(g_isp[i].cmd));
        lb_s(&l, " calls="); lb_u(&l, g_isp[i].calls);
        lb_s(&l, " changes="); lb_u(&l, g_isp[i].changes);
        lb_s(&l, " dumps="); lb_u(&l, g_isp[i].dumps);
        emit(&l);
    }
}

__attribute__((noinline)) static void isp_block_log(void *arg, long r)
{
    const struct isp_wrapper *w = (const struct isp_wrapper *)arg;
    uint32_t cmd = w->inner_cmd;
    const uint8_t *pl = (const uint8_t *)w->payload;
    if (!pl) return;
    if (cmd >= 0x100 && cmd <= 0x10f) return;                       /* VI commands: scalar, not a pointer */
    if (cmd == GET_3D_NR_STAT || cmd == SENSOR_SETREG || cmd == SENSOR_GETREG) return;   /* hot, isp_hook.log */
    unsigned want = isp_size_of(cmd);
    if (want > ISP_BLOCK_MAX) want = ISP_BLOCK_MAX;
    unsigned n = readable_len(pl, want);
    if (!n) return;
    uint32_t h = fnv(pl, n);
    unsigned long t = now_ms();
    int slot = -1, i;
    for (i = 0; i < NISP; i++) {
        if (g_isp[i].calls && g_isp[i].cmd == cmd) { slot = i; break; }
        if (!g_isp[i].calls && slot < 0) slot = i;
    }
    g_isp_calls++;
    if (slot < 0) return;
    int first = (g_isp[slot].calls == 0);
    g_isp[slot].cmd = cmd;
    g_isp[slot].calls++;
    if (first) { g_isp[slot].last_seen = h; g_isp[slot].last_dumped = h ^ 1u; }
    if (g_isp[slot].last_seen != h) g_isp[slot].changes++;
    g_isp[slot].last_seen = h;
    if (h == g_isp[slot].last_dumped) return;                       /* same content as the last dump */
    unsigned long gap = t - g_isp[slot].last_ms;
    unsigned min_ms = ((cmd >> 30) == 2u) ? ISP_GET_MIN_MS : ISP_SET_MIN_MS;
    if (!first && gap < min_ms) { g_isp_skip++; return; }
    if (g_isp[slot].bytes + n > CAP_BIN_CMD) { g_isp_skip++; return; }
    int fd = bin_open();
    if (fd < 0 || g_bin_bytes + 24u + n > g_cap_bin) { g_isp_skip++; return; }
    uint32_t hdr[6];
    hdr[0] = 0x42505349u;                                           /* "ISPB" little endian */
    hdr[1] = (uint32_t)t; hdr[2] = cmd; hdr[3] = n; hdr[4] = h;
    hdr[5] = (uint32_t)_sc2(SYS_GETTID, 0, 0);
    struct { const void *base; unsigned long len; } iov[2] = { { hdr, sizeof hdr }, { pl, n } };
    long wr = _sc3(SYS_WRITEV, fd, (long)iov, 2);
    int ok = (wr == (long)(sizeof hdr + n));
    if (wr < 0 && wr != -EBADF) _sc2(SYS_CLOSE, fd, 0);
    if (wr < 0) g_binfd = -1;
    if (wr > 0) { g_bin_bytes += (uint32_t)wr; g_isp[slot].bytes += (uint32_t)wr; }
    if (!ok) g_isp_skip++;
    g_isp[slot].last_dumped = h;
    g_isp[slot].last_ms = (uint32_t)t;
    g_isp[slot].dumps++;
    g_isp_dumps++;
    const char *name = isp_cmd_name(cmd);
    struct lb l; l.n = 0;
    head(&l, "ISP_BLOCK");
    lb_s(&l, " name="); lb_s(&l, name ? name : "UNKNOWN");
    lb_s(&l, " cmd="); lb_x(&l, cmd);
    lb_s(&l, " flag="); lb_u(&l, w->flag);
    lb_s(&l, " ret="); lb_i(&l, r < 0 ? -1 : r);
    lb_s(&l, " size="); lb_u(&l, n);
    lb_s(&l, " hash="); lb_x(&l, h);
    lb_s(&l, " calls="); lb_u(&l, g_isp[slot].calls);
    lb_s(&l, " changes="); lb_u(&l, g_isp[slot].changes);
    lb_s(&l, " dump="); lb_u(&l, g_isp[slot].dumps);
    lb_s(&l, " stored="); lb_s(&l, ok ? "bin" : "no");
    lb_s(&l, " head="); lb_hexbytes(&l, pl, n > 16 ? 16 : n);
    emit(&l);
}

/* ---- exported hooks -------------------------------------------------------------------- */
HOOK_EXPORT int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    long r = _sc3(SYS_OPEN, (long)path, (long)flags, (long)mode);
    on_open(path, flags, mode, r);
    return (int)fixret(r);
}

HOOK_EXPORT int close(int fd)
{
    long r = _sc2(SYS_CLOSE, fd, 0);
    if (g_state == 1) on_close(fd, r);
    return (int)fixret(r);
}

HOOK_EXPORT long read(int fd, void *buf, unsigned long count)
{
    long r = _sc3(SYS_READ, fd, (long)buf, (long)count);
    if (g_state == 1) on_read(fd, buf, count, r);
    return fixret(r);
}

HOOK_EXPORT long write(int fd, const void *buf, unsigned long count)
{
    long r = _sc3(SYS_WRITE, fd, (long)buf, (long)count);
    if (g_state == 1) on_write(fd, buf, count, r);
    return fixret(r);
}

HOOK_EXPORT long writev(int fd, const void *iov, int iovcnt)
{
    long r = _sc3(SYS_WRITEV, fd, (long)iov, (long)iovcnt);
    struct fdi *f = fdi_get(fd);
    if (f && (f->cls == C_PCM || f->cls == C_GPIO)) {
        f->wr_calls++;
        if (r > 0) f->wr_bytes += (uint32_t)r; else f->errs++;
        struct lb l; l.n = 0;
        head(&l, "WRITEV");
        lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
        lb_s(&l, " dev="); lb_s(&l, f->dev);
        lb_s(&l, " iovcnt="); lb_i(&l, iovcnt);
        if (r < 0) { lb_s(&l, " ret=-1 errno="); lb_u(&l, (unsigned long)-r); }
        else { lb_s(&l, " ret="); lb_u(&l, (unsigned long)r); }
        if (f->wr_calls <= 16 || r < 0) emit(&l);
    }
    return fixret(r);
}

HOOK_EXPORT int dup(int fd)
{
    long r = _sc2(SYS_DUP, fd, 0);
    struct fdi *f = fdi_get(fd);
    if (f && r >= 0 && r < MAXFD) {
        fdi_copy(&g_fdi[r], f);
        struct lb l; l.n = 0;
        head(&l, "DUP"); lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
        lb_s(&l, " newfd="); lb_u(&l, (unsigned long)r);
        lb_s(&l, " dev="); lb_s(&l, f->dev);
        emit(&l);
    }
    return (int)fixret(r);
}

HOOK_EXPORT int dup2(int oldfd, int newfd)
{
    long r = _sc2(SYS_DUP2, oldfd, newfd);
    if (g_state == 1 && r >= 0 && r < MAXFD) {
        struct fdi *f = fdi_get(oldfd);
        if (f && oldfd != newfd) {
            fdi_copy(&g_fdi[r], f);
            struct lb l; l.n = 0;
            head(&l, "DUP"); lb_s(&l, " fd="); lb_u(&l, (unsigned long)oldfd);
            lb_s(&l, " newfd="); lb_u(&l, (unsigned long)r);
            lb_s(&l, " dev="); lb_s(&l, f->dev);
            emit(&l);
        } else if (!f) {
            g_fdi[r].cls = 0;
        }
    }
    return (int)fixret(r);
}

HOOK_EXPORT void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off)
{
#if defined(__arm__)
    if ((unsigned long)off & 4095ul) {          /* what libc's mmap() checks before mmap2 */
        if (__errno_location) *__errno_location() = 22;
        return (void *)-1;
    }
#endif
    long r = sc6(SYS_MMAP, (long)addr, (long)len, prot, flags, fd, MMAP_OFF(off));
    struct fdi *f = fdi_get(fd);
    if (f && (f->cls == C_PCM || f->cls == C_MEM || f->cls == C_GPIO)) {
        struct lb l; l.n = 0;
        head(&l, "MMAP");
        lb_s(&l, " fd="); lb_u(&l, (unsigned long)fd);
        lb_s(&l, " dev="); lb_s(&l, f->dev);
        lb_s(&l, " len="); lb_x(&l, len);
        lb_s(&l, " prot="); lb_x(&l, (unsigned long)prot);
        lb_s(&l, " flags="); lb_x(&l, (unsigned long)flags);
        lb_s(&l, " off="); lb_x(&l, (unsigned long)off);
        if (r < 0 && r > -4096) { lb_s(&l, " ret=-1 errno="); lb_u(&l, (unsigned long)-r); }
        else { lb_s(&l, " ret="); lb_x(&l, (unsigned long)r); }
        emit(&l);
    }
    return (void *)fixret(r);
}

#define RTLD_NEXT ((void *)-1L)
extern void *dlsym(void *, const char *) __attribute__((weak));   /* in libc.so.0 of the camera */

HOOK_EXPORT int system(const char *cmd)
{
    static int (*real)(const char *);
    if (hook_on()) cmd_note("system", cmd);
    if (!real && dlsym) real = (int (*)(const char *))dlsym(RTLD_NEXT, "system");
    if (!real) { if (__errno_location) *__errno_location() = 38 /* ENOSYS */; return -1; }
    return real(cmd);
}

HOOK_EXPORT void *popen(const char *cmd, const char *mode)       /* returns a FILE * */
{
    static void *(*real)(const char *, const char *);
    if (hook_on()) cmd_note("popen", cmd);
    if (!real && dlsym) real = (void *(*)(const char *, const char *))dlsym(RTLD_NEXT, "popen");
    if (!real) { if (__errno_location) *__errno_location() = 38 /* ENOSYS */; return (void *)0; }
    return real(cmd, mode);
}

HOOK_EXPORT int ioctl(int fd, unsigned int req, void *arg)
{
    if (!hook_on()) {                       /* child process or hook idle: pure pass-through */
        long r0 = _sc3(SYS_IOCTL, fd, (long)req, (long)arg);
        return (int)fixret(r0);
    }
    struct fdi *f = fdi_get(fd);
    long r;
    if (f && (f->cls == C_PCM || f->cls == C_GPIO || f->cls == C_MEM)) {
        uint8_t pre[MAX_HEX];
        unsigned sz = (req >> 16) & 0x3fffu, npre = 0;
        if (sz && arg) npre = safe_copy(pre, arg, sz > MAX_HEX ? MAX_HEX : sz);
        r = _sc3(SYS_IOCTL, fd, (long)req, (long)arg);
        on_ioctl_tracked(fd, f, req, arg, pre, npre, r);
        alive();
        return (int)fixret(r);
    }
    r = isp_hook_ioctl(fd, req, arg);        /* legacy decoders: ISP, VENC, AKPCM, FD6-10 */
    if (req == ISP_IOCTL_CMD && arg) { isp_block_log(arg, r); alive(); }
    return (int)fixret(r);
}
