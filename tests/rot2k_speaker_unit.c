/* Host unit test of the speaker glue of the Rotatable 2K (firmware/lsc-rotatable-2k-ak3918av130/speaker.c) on the REAL shared engine
 * (src/features/audio/audio_out.c, the 1080p's) and the real talk session: registry keys to engine keys and back, ranges, the opt-in default,
 * the enable -> start path with no playback device (host), the amplifier route untouched, the engine's own file never written,
 * and the beep action. A directory of its own stands in for /config/akam: nothing outside it is touched. Prints "rot2k speaker unit: N failed". */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_out.h"
#include "speaker.h"
#include "talk.h"

static int fails;
#define CHECK(c, msg) do { if (c) printf("  ok   - %s\n", msg); else { printf("  FAIL - %s\n", msg); fails++; } } while (0)

void a2k_log(const char *fmt, ...)
{
    (void)fmt;
}

static char g_dir[256];

static int count_files(void)
{
    int n = 0;
    struct dirent *e;
    DIR *d = opendir(g_dir);
    if (!d)
        return -1;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static int get(const char *k, char *out, size_t cap) { return a2k_speaker_get(k, out, cap); }

int main(void)
{
    char v[64];
    st_apply_t eff;
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/spk_unit_XXXXXX", tmp && *tmp ? tmp : ".");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 2;
    }
    /* what a2k_speaker_init() does, with the directory of this test */
    audio_out_setup("/nonexistent/pcm_playback", g_dir, 0); /* no node: a host never plays */
    audio_out_set_persist(0);
    audio_out_set_guard_arm(0); /* what a2k_speaker_init() does: a normal start writes nothing */

    printf("rot2k speaker unit\n");
    /* defaults: the 1080p's names and values, the speaker opt-in */
    CHECK(get("audio.out_enable", v, sizeof v) > 0 && !strcmp(v, "0"), "audio.out_enable defaults to 0 on the 2K (opt-in)");
    CHECK(get("audio.out_volume", v, sizeof v) > 0 && !strcmp(v, "100"), "audio.out_volume defaults to 100, as on the 1080p");
    CHECK(get("audio.out_gain", v, sizeof v) > 0 && !strcmp(v, "2"), "audio.out_gain defaults to 2");
    CHECK(get("audio.out_amp", v, sizeof v) > 0 && !strcmp(v, "auto"), "audio.out_amp defaults to auto");
    CHECK(get("audio.talk_enable", v, sizeof v) > 0 && !strcmp(v, "1"), "audio.talk_enable defaults to 1");
    CHECK(get("audio.talk_duplex", v, sizeof v) > 0 && !strcmp(v, "half"), "audio.talk_duplex defaults to half");
    CHECK(get("audio.out_state", v, sizeof v) > 0 && !strcmp(v, "off"), "audio.out_state reads off while disabled");
    CHECK(get("audio.talk_state", v, sizeof v) > 0 && !strcmp(v, "idle"), "audio.talk_state reads idle");
    CHECK(get("audio.nothing", v, sizeof v) < 0, "an unknown key is refused");
    CHECK(get("audio.out_volume", v, 2) < 0, "a value that does not fit the buffer is an error, not a truncation");

    /* volume, gain, amp, duplex: applied live, read back, range checked by the engine as well */
    CHECK(a2k_speaker_set("audio.out_volume", "35", &eff) == 0 && get("audio.out_volume", v, sizeof v) > 0 && !strcmp(v, "35"), "volume 35 is applied and read back");
    CHECK(a2k_speaker_set("audio.out_volume", "101", &eff) != 0, "volume 101 is refused");
    CHECK(a2k_speaker_set("audio.out_volume", "-1", &eff) != 0, "volume -1 is refused");
    CHECK(a2k_speaker_set("audio.out_volume", "abc", &eff) != 0, "volume abc is refused");
    CHECK(get("audio.out_volume", v, sizeof v) > 0 && !strcmp(v, "35"), "a refused volume leaves the old one");
    CHECK(a2k_speaker_set("audio.out_gain", "5", &eff) == 0 && get("audio.out_gain", v, sizeof v) > 0 && !strcmp(v, "5"), "gain 5 is applied");
    CHECK(a2k_speaker_set("audio.out_gain", "6", &eff) != 0, "gain 6 is refused");
    CHECK(a2k_speaker_set("audio.out_amp", "on", &eff) == 0 && get("audio.out_amp", v, sizeof v) > 0 && !strcmp(v, "on"), "amp on is applied");
    CHECK(a2k_speaker_set("audio.out_amp", "loud", &eff) != 0, "amp loud is refused");
    CHECK(a2k_speaker_set("audio.out_amp", "auto", &eff) == 0, "amp back to auto");
    CHECK(a2k_speaker_set("audio.talk_duplex", "full", &eff) == 0 && get("audio.talk_duplex", v, sizeof v) > 0 && !strcmp(v, "full"), "duplex full is applied");
    CHECK(a2k_speaker_set("audio.talk_duplex", "both", &eff) != 0, "duplex both is refused");
    CHECK(audio_out_talk_half_duplex() == 0, "full duplex reaches the talk gate");
    CHECK(a2k_speaker_set("audio.talk_duplex", "half", &eff) == 0 && audio_out_talk_half_duplex() == 1, "half duplex reaches the talk gate");
    CHECK(a2k_speaker_set("audio.talk_enable", "0", &eff) == 0 && get("audio.talk_enable", v, sizeof v) > 0 && !strcmp(v, "0"), "talk_enable 0 is applied");
    CHECK(a2k_speaker_set("audio.talk_enable", "1", &eff) == 0, "talk_enable 1 is applied");
    CHECK(a2k_speaker_set("audio.out_state", "idle", &eff) != 0, "the state is read-only (refused)");

    /* nothing of the above wrote the engine's own file (the registry owns persistence) */
    CHECK(count_files() == 0, "no engine file was written by the settings (audio_out_set_persist(0))");

    /* the speaker is not available while it is not running: the backchannel is not offered */
    CHECK(audio_out_talk_available() == 0, "talk is not available while the speaker is off");
    CHECK(a2k_speaker_beep() != 0, "the beep action refuses while the speaker is not running");
    CHECK(a2k_speaker_boot() == 0 && !audio_out_running(), "boot with the speaker off starts nothing");

    /* enable on a host without the playback device: the start fails, is reported, leaves no crash guard behind (a clean failure is not a crash) */
    CHECK(a2k_speaker_set("audio.out_enable", "1", &eff) != 0, "enabling without a playback device fails (host)");
    CHECK(!audio_out_running(), "no playback thread runs after the failed start");
    CHECK(get("audio.out_state", v, sizeof v) > 0 && !strcmp(v, "error"), "the state says error");
    CHECK(count_files() == 0, "nothing was written under the config dir by a start (the guard is armed only by the user's own enable)");
    CHECK(get("audio.out_enable", v, sizeof v) > 0 && !strcmp(v, "1"), "the setting itself stays on (the user asked for it)");
    CHECK(a2k_speaker_set("audio.out_enable", "0", &eff) == 0 && get("audio.out_enable", v, sizeof v) > 0 && !strcmp(v, "0"), "disabling is always possible");

    /* a crash guard left by a previous run blocks the start, with its own state, until it is removed by hand */
    {
        char gp[300];
        FILE *f;
        snprintf(gp, sizeof gp, "%s/%s", g_dir, AO_GUARD_FILE);
        f = fopen(gp, "w");
        if (f) {
            fputs("1\n", f);
            fclose(f);
        }
        CHECK(a2k_speaker_set("audio.out_enable", "1", &eff) != 0, "a start is refused while the crash guard exists");
        CHECK(get("audio.out_state", v, sizeof v) > 0 && !strcmp(v, "disabled_crash"), "the state says disabled_crash");
        CHECK(access(gp, F_OK) == 0, "the marker of a previous crash is kept (only the user removes it)");
        unlink(gp);
        a2k_speaker_set("audio.out_enable", "0", &eff);
    }

    a2k_speaker_stop();
    {
        char cmd[320];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
        if (system(cmd) != 0) {
        }
    }
    printf("rot2k speaker unit: %d failed\n", fails);
    return fails ? 1 : 0;
}
