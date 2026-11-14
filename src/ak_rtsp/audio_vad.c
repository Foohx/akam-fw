#include "audio_vad.h"

#include <string.h>

void audio_vad_init(audio_vad_t *v, unsigned hold_ms)
{
    memset(v, 0, sizeof(*v));
    v->hold_us = (int64_t)hold_ms * 1000;
}

void audio_vad_reset(audio_vad_t *v)
{
    v->sumsq = 0;
    v->cnt = 0;
    v->mute_until_us = 0;
}

void audio_vad_feed(audio_vad_t *v, const int16_t *pcm, size_t n, int64_t now_us,
                    unsigned latency_ms)
{
    for (size_t i = 0; i < n; i++) {
        int32_t s = pcm[i];
        v->sumsq += (uint64_t)((int64_t)s * s);
        if (++v->cnt < AVAD_WINDOW) continue;
        if (v->sumsq / AVAD_WINDOW > AVAD_THRESHOLD_MS) {
            int64_t until = now_us + v->hold_us + (int64_t)latency_ms * 1000;
            if (until > v->mute_until_us) v->mute_until_us = until;
            v->speech_windows++;
        }
        v->sumsq = 0;
        v->cnt = 0;
    }
}

int audio_vad_speech(const audio_vad_t *v, int64_t now_us)
{
    return now_us < v->mute_until_us;
}
