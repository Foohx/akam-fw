/* The vendor motion statistics reader of the Rotatable 2K (ak_vpss_md_get_stat, libplat_vpss): the stock yy_cam polls it with dev 0 and a zeroed 1536-byte buffer (docs/porting/lsc-rotatable-2k/ai-own-app.md
 * section 5). The only vendor call of the motion code, so the host tests never link it (motion.c takes the reader through a2k_motion_set_md_reader). */
#include <stdint.h>

#include "motion.h"

extern int ak_vpss_md_get_stat(int dev, void *stat);

int a2k_motion_md_read(void *buf, unsigned n)
{
    (void)n; /* the library fills 1536 bytes: the caller's buffer is that big (motion.c) */
    return ak_vpss_md_get_stat(0, buf);
}
