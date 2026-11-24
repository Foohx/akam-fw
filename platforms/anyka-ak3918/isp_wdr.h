#pragma once
/* isp.wdr of the 1080p: the strength of the backlight (WDR) shadow lift of the ISP. A pure composer (no ioctl, no float, host-testable)
 * and the sender (live GET, compose, SET of the WDR block, RAM only). Layout and evidence: specs/video-and-streaming.md 5.8.
 *
 * Block: 0x4004491b SET / 0x8004491c GET (the GET is the SET + 1 of the tunnel, as for every other block), 16936 = 0x4228 bytes
 * = u32 mode + 17 entries of 996 bytes; entry 0 manual, entry 1 + L = ISO level L (like NR2, 5.5.1). Per entry (RE of
 * _set_wdr_attr and of the stock's check_wdr_params):
 *   +0x004 u16  1 = the six curves below are programmed (0: the ISP skips them)
 *   +0x006.. u16 x5  thresholds th1..th5 (32 64 128 196 240 in the conf)
 *   +0x010 + 130 k (k = 0..5)  six tone curves of 65 u16, 10 bit, identity = min(1023, 16 i)
 *   +0x3dc u16  bit 0 = WDR switched on (<= 1); +0x3de, +0x3e0, +0x3e2 u16 slope parameters (+0x3e2 < 1024)
 * In the stock day subfile entries 1..4 and 10 (the bright levels) lift the shadows (curve 0: 8/64 of the range goes to 228/1024 instead
 * of 128) and the others are the identity; the night subfile has the switch off. The strength S blends every curve of an entry that is
 * switched on towards the identity: c' = id + (c - id) * S / ISP_WDR_STOCK, rounded, clamped 0..1023, kept non-decreasing.
 * S = ISP_WDR_STOCK is the camera's own table (a plain copy, nothing sent). Entry 0, the mode word, the switches and every other field
 * are never changed. */

#include <stddef.h>
#include <stdint.h>

#define ISP_WDR_BLOCK_LEN 16936u
#define ISP_WDR_STOCK 40 /* the internal strength of the 1080p's own conf */
#define ISP_WDR_MAX   50
/* The user scale of isp.wdr is the 2K's: 0..100, 90 = the stock. 0..90 maps linearly onto the strength 0..40 (strength = (w * 40 + 45) / 90),
 * 91..100 onto the extra lift 41..50 (strength = w - 50). */
#define ISP_WDR_USER_STOCK 90
#define ISP_WDR_USER_MAX   100
int isp_wdr_strength(int user); /* the strength of a user value, -1 outside 0..100 */

/* The day Y_GAMMA toe, driven by the same slider (below 90 only). Block: 0x40044975 SET / 0x80044976 GET, 302 = 0x12e bytes = 129 u16 (the
 * curve, input step 8 over 0..1023, 10 bit, identity = min(1023, 8 i)) + 22 bytes of fields that are never changed. The day conf lifts the
 * bottom (8 -> 17, 96 -> 146, back on the identity about 256); the night curve is the identity. At user value w (0..90) every point above the
 * identity is pulled toward it: c' = id + (c - id) * w / 90, rounded, clamped to 1023, kept non-decreasing; points on or below the identity
 * are never touched. w >= 90 is the camera's own curve (a plain copy, nothing is sent, nothing more is lifted). */
#define ISP_YGAMMA_BLOCK_LEN 302u
#define ISP_YGAMMA_POINTS    129
uint32_t isp_ygamma_get_cmd(void);
uint32_t isp_ygamma_set_cmd(void);
/* 0 when the block is as decoded (length 302, every curve point <= 1024, the curve non-decreasing), else ISP_NR_REFUSED (-2). */
int isp_ygamma_check(const uint8_t *base, size_t len);
/* out (len bytes, must not alias base) = base with the toe at user value 0..ISP_WDR_USER_MAX (>= 90: a copy). 0, ISP_NR_REFUSED, or -1. */
int isp_ygamma_compose(const uint8_t *base, size_t len, int user, uint8_t *out);

uint32_t isp_wdr_get_cmd(void);
uint32_t isp_wdr_set_cmd(void);

/* 0 when the block is as decoded (length, mode word not 0, per entry: +4 <= 1, +0x3dc <= 1, +0x3e2 < 1024, every curve value <= 1023
 * and every curve of a switched-on entry starting at 0), else ISP_NR_REFUSED (-2). */
int isp_wdr_check(const uint8_t *base, size_t len);

/* out (len bytes, must not alias base) = base with strength s (0..ISP_WDR_MAX, else -1). 0, ISP_NR_REFUSED, or -1. */
int isp_wdr_compose(const uint8_t *base, size_t len, int s, uint8_t *out);

/* Sender, called with the ISP configuration lock held (isp_cfg_lock). s: the USER value 0..ISP_WDR_USER_MAX. Returns 0 done (nothing is sent while the
 * strength is the stock one and the block was never changed, nor while the composed block is the live one), -1 the ISP refused or
 * could not be read (errno), ISP_NR_REFUSED the live block is not as decoded (nothing sent). The old strength is kept on a failure. */
int isp_wdr_set(int s);
int isp_wdr_get(void);      /* the user value in force (ISP_WDR_USER_STOCK until a first accepted set) */
void isp_wdr_reset(void);   /* after a pipeline reload: the vendor block is back, the cached baseline and the "changed" mark are stale */
/* The last [isp.wdr] diagnostic line (get/set results, mode word, switch / programmed / lifted entry masks, curve checksums before and after the
 * rescale, bytes changed, read-back after the SET): the text that was logged. Returns its length; "[isp.wdr] no event yet" before the first.
 * Every isp.wdr change and every ISP load logs one such line. */
int isp_wdr_dump(char *out, size_t cap);
/* isp.wdr also drives the Y_GAMMA toe (below 90): the sender reads the live block once as its pristine baseline, composes from it, SETs and
 * reads back, like the WDR block, and the same [isp.wdr] line carries `gamma=ok|errnoE|none glift=N gcsum_before=.. gcsum_after=..
 * gchanged=N gset=.. greadback=.. [gnote=..]`. The WDR and the gamma part are independent: one refused or failing leaves the other applied; the
 * call fails (old strength kept) only when neither could be applied. */
void isp_wdr_reapply(void);/* after isp_wdr_reset(): puts a non-stock strength back on the fresh block (logged on failure) */
