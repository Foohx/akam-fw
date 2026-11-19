#pragma once
/* Picture orientation for a camera mounted upside down or seen in a mirror: vertical flip and horizontal mirror,
 * done by the ISP (the sensor reads out in the other direction), so the stream, the snapshots and the motion
 * detection all see the turned picture. Both together are a rotation by 180 degrees.
 *
 * Saved as flip=0|1 and mirror=0|1 in /etc/config/akam/video (video_cfg.h); 8091 keys video.flip and video.mirror
 * (video_ctl.h). Unlike the other video settings they apply at once. The ISP forgets them when it reloads its
 * configuration for a day/night switch, so they are sent again after each one. */

/* Remembers the wanted orientation and sends it to the ISP. Returns 0, or -1 when the ISP refused (the values are
 * remembered all the same and sent again at the next day/night switch). Before the ISP is up (orient_ready()),
 * it only remembers. */
int orient_set(int flip, int mirror);

/* main.c: the ISP is initialised and streaming; sends the remembered orientation if it is not the default. */
void orient_ready(void);

/* night.c, at the end of every day/night switch (ISP configuration lock held). */
void orient_after_mode_switch(void);
