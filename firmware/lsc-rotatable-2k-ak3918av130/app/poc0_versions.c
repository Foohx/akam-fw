/* PoC-0 of the Rotatable 2K own-app study: print the versions of the on-camera vendor libraries. It proves that a
 * binary built with the pinned uClibc toolchain loads and links the stock /appfs/lib *.so. No vendor header is
 * used: the only prototypes are the version getters below (const char *f(void), observed in the libraries'
 * exports and disassembled: each returns a constant string, signature runtime-UNVERIFIED until it runs on the
 * camera). Nothing here touches a device: no ak_sdk_init, no open(). */
#include <stdio.h>

extern const char *ak_vi_get_version(void);
extern const char *ak_vpss_get_version(void);
extern const char *ak_venc_get_version(void);
extern const char *ak_mem_get_version(void);
extern const char *ak_app_video_get_version(void);

#define SHOW(f) printf("%-28s %s\n", #f, f())

int main(void)
{
    SHOW(ak_vi_get_version);
    SHOW(ak_vpss_get_version);
    SHOW(ak_venc_get_version);
    SHOW(ak_mem_get_version);
    SHOW(ak_app_video_get_version);
    return 0;
}
