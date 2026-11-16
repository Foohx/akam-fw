/* The ISP side of the Rotatable 2K own app as the other modules see it: image effects and the day/night primitives.
 * Implemented in akam2k_cap.c (every vendor call of the app lives there); the host unit tests link their own fakes.
 * All return 0 on success, non-zero on failure (the vendor code, or -1). */
#ifndef AKAM2K_ISP_H
#define AKAM2K_ISP_H

/* ak_vpss_effect_set(0, type, value). Types: A2K_FX_*. */
#define A2K_FX_BRIGHTNESS 1
#define A2K_FX_CONTRAST 3
#define A2K_FX_SHARPNESS 4
#define A2K_FX_WDR 5 /* the stock "WDR" effect (wdr_type 1): 40 = on, 0 = off, see a2k_isp_wdr */
#define A2K_FX_MAINS_HZ 7
int a2k_isp_effect(int type, int value);

/* The stock yy_media_av130_isp_set_wdr(onoff, 1) with wdr_type 1 (its [dsp] default): ak_vpss_effect_set(0, 5, 40) for on, (0, 5, 0)
 * for off. The ISP mode switch resets it, so the stock replays it after every switch. */
int a2k_isp_wdr(int on);

/* Registers the stock automatic day/night parameters with the vendor lib (once, before the first a2k_isp_dn_level). */
int a2k_isp_dn_init(void);
/* The lib's decision: cur_night 0 day / 1 night, *want_night 0 day / 1 night. Fails while the AE is not stable. */
int a2k_isp_dn_level(int cur_night, int *want_night);
/* ak_vi_switch_mode(0, night) then one keyframe request on every stream (the picture jumps), like the stock. */
int a2k_isp_mode(int night);

#endif
