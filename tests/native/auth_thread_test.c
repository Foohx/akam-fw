/* Thread stress test of the shared authentication state: rtsp_auth.c (one db, limiter, per-connection state) and
 * cred.c (credential store, replay cache), driven from several threads at once the way the RTSP thread and the
 * ONVIF HTTP thread will. Built with ThreadSanitizer when the toolchain has it, else AddressSanitizer (see
 * tests/auth_thread_test.sh). Real files in a temporary directory, real clocks.
 * Run: auth_thread_test TEMPDIR */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "b64.h"
#include "cred.h"
#include "md5.h"
#include "rtsp_auth.h"
#include "sha1.h"

#define NTHREADS 4
#define PER_THREAD 30   /* 4 * 30 = 120 distinct nonces: below CRED_NONCE_CACHE, so every good token must pass */

static char dir[256], cpath[300], apath[300];
static rtsp_auth_db_t limiter;
static const char PASS[] = "Correct-Horse-9";
static int stop;
static int failures;
static pthread_mutex_t fm = PTHREAD_MUTEX_INITIALIZER;
static void bad(const char *m)
{
    pthread_mutex_lock(&fm);
    printf("  FAIL - %s\n", m);
    failures++;
    pthread_mutex_unlock(&fm);
}

static void put(const char *path, const char *content, mode_t mode)
{
    char tmp[320];
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, content, strlen(content)) < 0) exit(2);
    fchmod(fd, mode);
    close(fd);
    rename(tmp, path);
}

static void *worker(void *arg)
{
    long id = (long)arg;
    char created[40], nonce[32], dg[64];
    for (int i = 0; i < PER_THREAD; i++) {
        time_t now = time(NULL);
        struct tm t;
        gmtime_r(&now, &t);
        strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &t);
        uint8_t raw[16] = { (uint8_t)id, (uint8_t)i, 7 };
        b64_encode(raw, sizeof(raw), nonce, sizeof(nonce));
        sha1_ctx c;
        uint8_t d[20];
        sha1_init(&c);
        sha1_update(&c, raw, sizeof(raw));
        sha1_update(&c, created, strlen(created));
        sha1_update(&c, PASS, strlen(PASS));
        sha1_final(&c, d);
        b64_encode(d, 20, dg, sizeof(dg));
        unsigned delay;
        if (!cred_check_wsse("viewer", nonce, created, dg, &delay)) bad("a valid token was refused");
        if (cred_check_wsse("viewer", nonce, created, dg, &delay)) bad("a replayed token passed");
        if (cred_check_wsse("viewer", nonce, created, "AAAAAAAAAAAAAAAAAAAAAAAAAAA=", &delay)) bad("a bad digest passed");

        /* RTSP side: own connection state, shared db */
        rtsp_auth_conn_t conn;
        rtsp_auth_result_t r;
        rtsp_auth_conn_init(&conn);
        rtsp_auth_check(&limiter, &conn, "DESCRIBE", NULL, 0, 1000 + i, &r);
        if (r.verdict != RTSP_AUTH_DENY && r.verdict != RTSP_AUTH_ALLOW) bad("unexpected verdict");
        (void)rtsp_auth_session_ok(&limiter, &conn);
        (void)rtsp_auth_db_mode(&limiter);
        char u[64];
        (void)cred_get_user(u, sizeof(u));
        (void)cred_have_plain();
        (void)cred_auth_enabled();
        rtsp_auth_db_refresh(&limiter);
        rtsp_auth_digest_t dd;
        cred_check_http_digest("POST", "Digest username=\"viewer\"", 24, &dd, &delay);
        cred_note_failure(&delay);
        rtsp_auth_note_success(&limiter);
    }
    return NULL;
}

/* Rewrites the files while the workers run (same content, new inode/mtime: the reload path is exercised). */
static void *churn(void *arg)
{
    (void)arg;
    char ha1[33], line[128], cr[128];
    md5_hex_cat(ha1, "viewer", ":akam:", PASS, (char *)NULL);
    snprintf(line, sizeof(line), "viewer:akam:%s\n", ha1);
    snprintf(cr, sizeof(cr), "user=viewer\npass=%s\n", PASS);
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        put(apath, line, 0600);
        put(cpath, cr, 0600);
        cred_refresh(true);
        usleep(2000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    snprintf(cpath, sizeof(cpath), "%s/credentials", dir);
    snprintf(apath, sizeof(apath), "%s/rtsp_auth", dir);
    char ha1[33], line[128], cr[128];
    md5_hex_cat(ha1, "viewer", ":akam:", PASS, (char *)NULL);
    snprintf(line, sizeof(line), "viewer:akam:%s\n", ha1);
    snprintf(cr, sizeof(cr), "user=viewer\npass=%s\n", PASS);
    put(apath, line, 0600);
    put(cpath, cr, 0600);
    rtsp_auth_db_init(&limiter, apath);
    cred_init(cpath, &limiter);          /* once, before any thread: what main() does */
    pthread_t th[NTHREADS], ch;
    pthread_create(&ch, NULL, churn, NULL);
    for (long i = 0; i < NTHREADS; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    pthread_join(ch, NULL);
    printf("%s\n", failures ? "FAILED" : "  ok   - no race, every verdict as expected\nall ok");
    return failures ? 1 : 0;
}
