/* See speaker.h. */
#include "speaker.h"

#include <stdio.h>
#include <string.h>

#include "audio_out.h"
#include "log.h"
#include "talk.h"

/* The playback node, handed to the shared engine, which opens it: never opened here. The line is exactly this one (the anti-brick audits of the overlay test exempt it). */
#define PCM_PLAYBACK "/dev/pcmC0D0p"

void a2k_speaker_init(void)
{
    audio_out_setup(PCM_PLAYBACK, A2K_SPK_CONF_DIR, 0);
    audio_out_set_persist(0);
    audio_out_set_guard_arm(0); /* a normal start writes nothing to flash; only a user turning the speaker on (a2k_speaker_set after boot) arms the guard for that one start */ /* the registry saves audio.* in /config/akam/settings, the engine's own file stays unwritten */
}

/* The singleton opens the device only here. A speaker that failed to start leaves the state error / disabled_crash, shown by audio.out_state. */
static int g_booted; /* a2k_speaker_boot ran: from now on an enable is a user action */

static int start_if_needed(int user_action)
{
    if (audio_out_running())
        return 0;
    audio_out_set_guard_arm(user_action); /* the marker is written only for this first start after the user's choice, never for a start at boot or after a restart */
    if (audio_out_start() != 0) {
        audio_out_set_guard_arm(0);
        a2k_log("speaker: not running (see audio.out_state)");
        return -1;
    }
    audio_out_set_guard_arm(0);
    a2k_log("speaker: running");
    return 0;
}

int a2k_speaker_boot(void)
{
    char v[32];
    g_booted = 1;
    if (audio_out_ctl_get("audio.out_enable", v, sizeof v) != 0 || strcmp(v, "audio.out_enable=1") != 0)
        return 0;
    return start_if_needed(0);
}

void a2k_speaker_stop(void) { audio_out_stop(); }

/* "audio.out_volume=80" -> "80" copied to out. Length, or -1. */
static int value_of(const char *line, char *out, size_t cap)
{
    const char *eq = strchr(line, '=');
    int n;
    if (!eq || strncmp(line, "ERR", 3) == 0)
        return -1;
    n = snprintf(out, cap, "%s", eq + 1);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int a2k_speaker_get(const char *key, char *out, size_t cap)
{
    char line[96];
    if (strcmp(key, "audio.talk_state") == 0) {
        talk_t *t = talk_global();
        return snprintf(out, cap, "%s", t->active ? "active" : "idle");
    }
    if (audio_out_ctl_get(key, line, sizeof line) != 0)
        return -1;
    return value_of(line, out, cap);
}

int a2k_speaker_set(const char *key, const char *val, st_apply_t *eff)
{
    char msg[160];
    (void)eff;
    /* The engine validates again (range, words): a value the registry let through always passes. */
    if (audio_out_ctl_set(key, val, msg, sizeof msg) != 0)
        return -1;
    if (strcmp(key, "audio.out_enable") == 0 && strcmp(val, "1") == 0)
        return start_if_needed(g_booted);
    return 0;
}

int a2k_speaker_beep(void)
{
    char msg[160];
    return audio_out_ctl_set("audio.out_test", "1", msg, sizeof msg) == 0 ? 0 : -1;
}
