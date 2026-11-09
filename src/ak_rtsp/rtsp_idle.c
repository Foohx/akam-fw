#include "rtsp_idle.h"

void rtsp_idle_init(rtsp_idle_t *s, int64_t now_us)
{
    s->start_us = now_us;
    s->activity_us = now_us;
}

void rtsp_idle_request(rtsp_idle_t *s, int64_t now_us)
{
    s->activity_us = now_us;
}

const char *rtsp_idle_check(const rtsp_idle_t *s, int is_playing, int64_t now_us)
{
    if (is_playing) return 0;
    if (now_us - s->start_us >= (int64_t)RTSP_IDLE_SETUP_MAX_MS * 1000)
        return "too long to reach PLAY";
    if (now_us - s->activity_us >= (int64_t)RTSP_IDLE_PRE_PLAY_MS * 1000)
        return "idle before PLAY";
    return 0;
}

int rtsp_idle_wait_ms(const rtsp_idle_t *s, int is_playing, int64_t now_us, int max_ms)
{
    if (is_playing) return max_ms;
    int64_t a = s->activity_us + (int64_t)RTSP_IDLE_PRE_PLAY_MS * 1000 - now_us;
    int64_t b = s->start_us + (int64_t)RTSP_IDLE_SETUP_MAX_MS * 1000 - now_us;
    int64_t us = a < b ? a : b;
    int64_t ms = us <= 0 ? 1 : (us + 999) / 1000;
    return ms > max_ms ? max_ms : (int)ms;
}
