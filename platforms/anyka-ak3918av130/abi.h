/* Vendor ABI of the Rotatable 2K own app, as transcribed in poc2_frame.c (which keeps its own copy: it is the
 * hardware-validated PoC-2 and stays untouched). Sizes and offsets come from the stock yy_cam disassembly and were
 * confirmed on the camera by PoC-2 (both VI channels, H.264 Main/Baseline). No vendor header is used or committed:
 * everything below is OUR transcription, with _Static_assert on the sizes. */
#ifndef AKAM2K_ABI_H
#define AKAM2K_ABI_H
#include <stddef.h>
#include <stdint.h>

/* ---- struct transcriptions (sizes and offsets from the stock code) ----------------------------------------- */

/* 0x29d060: 16 zero bytes, only the word at +12 is ever set (1 enables the ISP tuning server: we never do). HIGH */
struct sdk_param { uint32_t w[4]; };

/* ak_vi_set_dev_attr, 56 bytes at ctx+900 (memset 56 at 0x29d368), copied from the config at 0x29d4b8..0x29d744.
 * Offsets HIGH; names of the four zero words (+4..+16: interface mode, fd?, data path, data type) are LOW, the stock
 * leaves them 0 with the default config. */
struct vi_dev_attr {
    uint32_t dev_id;                                   /* +0  sensor1_dev_id (0) */
    uint32_t zero4, zero8, zero12, zero16;             /* +4..+16 */
    uint32_t crop_left, crop_top, crop_w, crop_h;      /* +20..+32 defaults 0 0 1280 720 (kept as stock) */
    uint32_t max_w, max_h;                             /* +36,+40 2304 x 1296 */
    uint32_t fps;                                      /* +44 15 */
    uint32_t sub_max_w, sub_max_h;                     /* +48,+52 640 x 360 */
};

/* ak_vi_set_chn_attr, 20 bytes per channel at ctx+956 (memset 80 = 4 channels, 0x29d7e8). HIGH on offsets from the
 * stores at 0x29d8cc..0x29da18, names MEDIUM. */
struct vi_chn_attr {
    uint32_t chn;      /* +0  video_chnN_chn_id: 0 main, 1 sub */
    uint32_t fps;      /* +4  video_chnN_frame_rate (15) */
    uint32_t width;    /* +8 */
    uint32_t height;   /* +12 */
    uint32_t depth;    /* +16 frame depth, MUST be 1..4 (lib check, hardware run 1). yy_cam: video_chnN_depth, and when
                        * that is 0 (the default) 3 if the DMA reserved pool is > 50 MiB else 2 (0x29d294..0x29d2ac,
                        * used at 0x29d9cc..0x29da4c). HIGH */
};

/* ak_venc_open_ex first argument, 64 bytes (fp-104 at 0x29e6d4), filled at 0x29df50..0x29e684. HIGH on offsets. */
struct venc_param {
    uint16_t width, height;          /* +0,+2 */
    uint16_t fps, gop;               /* +4,+6 */
    uint16_t target_kbps, max_kbps;  /* +8,+10 */
    uint32_t profile;                /* +12 Main 0, High 1, Baseline 2 (strcmp table 0x29e0d0..0x29e2ac) */
    uint32_t rc_mode;                /* +16 CBR 0, VBR 1, CONST_QP 2, LOW_LATENCY 3, AVBR 4 (names MEDIUM) */
    uint16_t init_qp, min_qp, max_qp, pad26; /* +20,+22,+24 (+26 never written) */
    uint32_t jpeg_quality;           /* +28 video_chnN_snap_jpeg_quality (1) */
    uint32_t chroma_mode;            /* +32 (1) */
    uint32_t enc_type;               /* +36 H264 0 (MJPEG 1, H265 2: MEDIUM) */
    uint32_t buf_size;               /* +40 enc_buf_size (0) */
    uint32_t quality_lvl;            /* +44 (30) */
    uint32_t smart_mode;             /* +48 (0) */
    uint16_t smart_goplen, smart_q, smart_static, pad58; /* +52,+54 (not loaded: 0),+56 */
    uint32_t gdr_mode;               /* +60 (0) */
};

/* second argument of ak_venc_open_ex: 28 zero bytes, the byte at +24 set to 1 (meaning LOW), copied from stock. */
struct venc_ext { uint8_t b[28]; };

/* ak_vi_get_frame out struct: 40 bytes zeroed by the stock (0x29f96c). Only these are used by yy_cam: word 0 and
 * word 1 go to ak_venc_encode_frame as (data, len) (MEDIUM: consistent with the lib's encode_frame(handle, data, len,
 * mdinfo, stream)), the u64 at +8 is the timestamp in ms (HIGH, stock multiplies by 1000), word 8 (+32) is passed
 * through as the third encoder argument (probably the motion/ROI info pointer, LOW). */
struct vi_frame {
    uint32_t data;      /* +0  */
    uint32_t len;       /* +4  */
    uint64_t ts_ms;     /* +8  */
    uint32_t w16, w20, w24, w28; /* +16..+28 unknown (the stock reads +16 into its own record) */
    uint32_t mdinfo;    /* +32 */
    uint32_t w36;
};

/* ak_venc_encode_frame out struct, 24 bytes zeroed by the stock (0x29f980): data ptr +0, len +4 (HIGH), timestamp u64
 * +8 (HIGH), sequence +16 (MEDIUM), type +20 (MEDIUM: the stock maps ==1 to its own 'not I' flag). */
struct venc_stream {
    uint8_t *data;
    uint32_t len;
    uint64_t ts_ms;
    uint32_t seq;
    uint32_t type;
};

/* ak_vpss_set_soft_ps_param, 92 bytes: the vendor's automatic day/night parameters ("ak_vpss_set_auto_day_night_param" in
 * the lib's own messages). Offsets and names from the stock call (0x2a123c) and from the lib's validation of the struct
 * (libplat_vpss 0xa628: its error strings name the fields). The values are the ones the stock passes. Names HIGH where the
 * lib validates the field, LOW (w..) for the words it never checks. */
struct vpss_dn_param {
    uint32_t day_to_night_lum;       /* +0  15410000 (0xeb2350), must be non-zero */
    uint32_t night_to_day_lum;       /* +4  2359000 (0x2402c0), must be non-zero */
    uint32_t w8;                     /* +8  0 (not checked) */
    uint32_t awb_night_dis_max;      /* +12 4000, non-zero */
    uint32_t day_check_frame_num;    /* +16 10, non-zero */
    uint32_t night_check_frame_num;  /* +20 10, non-zero */
    uint32_t exp_stable_check_frame_num; /* +24 10, non-zero */
    uint32_t w28;                    /* +28 10000 */
    uint32_t gain_r;                 /* +32 1280, non-zero */
    uint32_t w36;                    /* +36 2048 */
    uint32_t gain_b;                 /* +40 1280, non-zero */
    uint32_t w44;                    /* +44 2048 */
    uint32_t gain_rb;                /* +48 0 -> the lib uses 1024 */
    uint32_t w52;                    /* +52 0 */
    uint32_t blk_check_flag;         /* +56 1 (0..1) */
    uint32_t low_lumi_th;            /* +60 5 (0..255) */
    uint32_t high_lumi_th;           /* +64 245 (1..255) */
    uint32_t non_infrared_ratio_th;  /* +68 40 (1..99) */
    uint32_t highlight_lumi_th;      /* +72 170 (1..254) */
    uint32_t highlight_blk_cnt_max;  /* +76 45 (1..768) */
    uint32_t lock_time;              /* +80 0: the lib's own lock thread is off, the stock locks itself (LOW on the name) */
    uint32_t w84;                    /* +84 0 */
    uint32_t revised_dis_max_ratio;  /* +88 0 -> the lib uses 5 (0..1024) */
};

#define SZCHK(t, n) _Static_assert(sizeof(t) == (n), #t " size")
SZCHK(struct sdk_param, 16);
SZCHK(struct vi_dev_attr, 56);
SZCHK(struct vi_chn_attr, 20);
SZCHK(struct venc_param, 64);
SZCHK(struct venc_ext, 28);
SZCHK(struct vi_frame, 40);
SZCHK(struct vpss_dn_param, 92);
_Static_assert(offsetof(struct vpss_dn_param, gain_rb) == 48, "dn param gain_rb");
_Static_assert(offsetof(struct vpss_dn_param, non_infrared_ratio_th) == 68, "dn param non IR ratio");
_Static_assert(offsetof(struct vpss_dn_param, lock_time) == 80, "dn param lock");
_Static_assert(offsetof(struct vi_dev_attr, crop_left) == 20, "dev attr crop");
_Static_assert(offsetof(struct vi_dev_attr, sub_max_w) == 48, "dev attr sub");
_Static_assert(offsetof(struct venc_param, profile) == 12, "venc profile");
_Static_assert(offsetof(struct venc_param, init_qp) == 20, "venc qp");
_Static_assert(offsetof(struct venc_param, enc_type) == 36, "venc type");
_Static_assert(offsetof(struct venc_param, smart_goplen) == 52, "venc smart");
_Static_assert(offsetof(struct venc_param, gdr_mode) == 60, "venc gdr");
_Static_assert(offsetof(struct vi_frame, ts_ms) == 8, "frame ts");
_Static_assert(offsetof(struct vi_frame, mdinfo) == 32, "frame mdinfo");
#if __SIZEOF_POINTER__ == 4 /* the camera ABI; a 64-bit host unit test only needs the layout of venc_param */
SZCHK(struct venc_stream, 24);
_Static_assert(offsetof(struct venc_stream, ts_ms) == 8, "stream ts");
_Static_assert(offsetof(struct venc_stream, type) == 20, "stream type");
#endif

/* ---- NPU detection (same transcription as poc4_detect.c, which keeps its own copy: it is the hardware-validated PoC-4) */

/* ak_svp2_create_chn attr: model_num then one 20-byte entry per model (the stock uses 1: env 0, type 10, classify 700,
 * IoU 3, target 0). */
struct svp2_model { uint32_t env_mode, model_type, classify_thr, iou_thr, target_type; };
struct svp2_attr { uint32_t model_num; struct svp2_model m[1]; };
/* conf_info: +0 model path, +4 conf_mode (0 = file path), +8 read function (NULL = default); 16 zeroed bytes passed. */
struct svp2_conf { const char *path; uint32_t mode; void *read_fn; uint32_t pad; };
/* ak_svp2_process input, 40 bytes: +4 img_type (0 = YUV420SP), +8 w, +12 h, +16..+28 region of interest, +32 physical
 * address, +36 virtual address. */
struct svp2_input { uint32_t zero0, img_type, width, height, roi_left, roi_top, roi_w, roi_h, phys, virt; };
/* ak_svp2_process out, 16 bytes: +0 count, +4 results (valid until ak_svp2_release). */
struct svp2_output { uint32_t count; const void *res; uint32_t type, w12; };
/* One result, stride 24: class (0 person, 1 head), box as corners x1,y1,x2,y2 in input pixels, score 0..1000. */
struct svp2_result { int32_t cls, x1, y1, x2, y2, score; };
SZCHK(struct svp2_attr, 24);
SZCHK(struct svp2_input, 40);
SZCHK(struct svp2_result, 24);
_Static_assert(offsetof(struct svp2_input, phys) == 32, "svp2 input phys");
_Static_assert(offsetof(struct svp2_input, virt) == 36, "svp2 input virt");
_Static_assert(offsetof(struct svp2_input, img_type) == 4, "svp2 input img_type");
#if __SIZEOF_POINTER__ == 4
SZCHK(struct svp2_conf, 16);
SZCHK(struct svp2_output, 16);
_Static_assert(offsetof(struct svp2_conf, read_fn) == 8, "svp2 conf read_fn");
#endif

/* ---- vendor prototypes (all return 0 on success; HIGH from the call sites) ------------------------------- */
extern int ak_sdk_init(struct sdk_param *p);
extern int ak_sdk_exit(void);
extern int ak_mem_get_total_size(uint32_t *size);          /* 0x29d24c, out word, rc ignored by stock (MEDIUM) */
extern int ak_mem_dma_get_reserved_size(uint32_t *size);   /* 0x29d258 */
extern int ak_mem_dma_vaddr2paddr(const void *vaddr, uint32_t *paddr);
extern int ak_npu_init(void);
extern int ak_npu_deinit(void);
extern void ak_npu_reset_hw(void);
extern int ak_svp2_create_chn(int handle, const struct svp2_attr *attr, const struct svp2_conf *conf);
extern int ak_svp2_destroy_chn(int handle);
extern int ak_svp2_process(int handle, const struct svp2_input *in, struct svp2_output *out);
extern int ak_svp2_release(struct svp2_output *out);
extern int ak_vi_open(int dev);
extern int ak_vi_close(int dev);
extern int ak_vi_load_sensor_cfg(int dev, const char *cfg_path);
extern int ak_vi_get_sensor_resolution(int dev, int res[2]); /* 8-byte out struct, 0x27c1c8 (MEDIUM) */
extern int ak_vi_set_dev_attr(int dev, const struct vi_dev_attr *attr);
extern int ak_vi_enable_dev(int dev);
extern int ak_vi_disable_dev(int dev);
extern int ak_vi_set_chn_attr(int chn, const struct vi_chn_attr *attr);
extern int ak_vi_enable_chn(int chn);
extern int ak_vi_disable_chn(int chn);
extern int ak_vi_get_frame(int chn, struct vi_frame *f);
extern int ak_vi_release_frame(int chn, struct vi_frame *f);
/* Image and day/night (RE of yy_cam and of libplat_vpss / libplat_vi, see stock-firmware-reference.md "Day/night and image";
 * the effect and mode semantics are HIGH from the lib code, none of it ran on the camera yet). dev is the VI/ISP device 0.
 * effect types 0..5 take -50..50 (the stock maps its 0..100 setting v to v - 50): 1 brightness, 3 contrast, 4 sharpness
 * are the ones yy_cam uses; 7 = mains frequency, 50 or 60 only; (0, 2, 5 exist and are never used by the stock; 6 = ISP
 * style, 8 = flip/mirror bits: not used by us, see ak_vpss_switch_flip_mirror). ak_vi_switch_mode: 0 day, 1 night (loads the ISP conf day / night set).
 * ak_vpss_get_soft_ps_level: cur 0 day / 1 night, *want 0 day / 1 night = the lib's own decision, with its hysteresis and
 * its non-infrared check (which does NOT keep the camera's own IR light out: hardware 2026-11-15, see night_ctl.h);
 * it fails until ak_vpss_set_soft_ps_param ran and while the AE is not stable. */
extern int ak_vpss_effect_set(int dev, int type, int value);
extern int ak_vpss_set_soft_ps_param(int dev, const struct vpss_dn_param *p);
extern int ak_vpss_get_soft_ps_level(int dev, int cur, int *want);
extern int ak_vi_switch_mode(int dev, int mode);
/* Flip / mirror of the picture (libplat_vpss, RE of the lib and of yy_cam, see isp-controls.md "Flip and mirror"): dev 0..3, flip_en and mirror_en 0 or 1 (the lib's own error
 * text names them "flip_en" and "mirror_en"; anything else is refused). The lib XORs each with the orientation its sensor config declared: 0 = the sensor's native picture
 * (what the app has always shown), 1 = reversed. 0 = done, live (ioctl 0x40044979 of the ISP), no stream restart. */
extern int ak_vpss_switch_flip_mirror(int dev, int flip_en, int mirror_en);
/* ISP attribute accessors used by the RAM-only test hook (isptest.h; layouts and confidence in docs/porting/lsc-rotatable-2k/isp-controls.md). All take the
 * device 0 and return 0 on success, 0x100 for a NULL / bad device, or the validation / ioctl failure. Every setter validates the whole struct before it writes, so a
 * get -> change one field -> set of the same buffer is the safe way to use them. The buffers the hook passes are 16 KiB (the real structs are smaller). */
extern int ak_vpss_effect_get(int dev, int type, int *value);
extern int ak_vpss_get_yuv_effect_attr(int dev, void *attr);
extern int ak_vpss_set_yuv_effect_attr(int dev, const void *attr);
extern int ak_vpss_get_wb_type(int dev, int *type);
extern int ak_vpss_set_wb_type(int dev, const int *type);
extern int ak_vpss_get_mwb_attr(int dev, void *attr);
extern int ak_vpss_set_mwb_attr(int dev, const void *attr);
extern int ak_vpss_get_ccm_attr(int dev, void *attr);
extern int ak_vpss_set_ccm_attr(int dev, const void *attr);
extern int ak_vpss_get_nr1_attr(int dev, void *attr);
extern int ak_vpss_set_nr1_attr(int dev, const void *attr);
extern int ak_vpss_get_nr2_attr(int dev, void *attr);
extern int ak_vpss_set_nr2_attr(int dev, const void *attr);
extern int ak_vpss_get_uvnr_attr(int dev, void *attr);
extern int ak_vpss_set_uvnr_attr(int dev, const void *attr);
extern int ak_vpss_get_3d_nr_attr(int dev, void *attr);
extern int ak_vpss_set_3d_nr_attr(int dev, const void *attr);
extern int ak_vpss_get_awb_stat_info(int dev, void *buf); /* 9376 bytes of statistics + 38 bytes of run info */
extern int ak_vpss_get_rgb_average(int dev, int *r, int *g, int *b);
extern int ak_venc_open_ex(const struct venc_param *p, const struct venc_ext *ext, int *handle);
extern int ak_venc_close(int handle);
extern int ak_venc_strictlylimit_frameSize(int handle, int limit);
extern int ak_venc_set_stream_buff(int handle, unsigned size);
extern int ak_venc_request_idr(int handle);
/* Live setters (libmpi_venc, RE: docs/porting/lsc-rotatable-2k/venc-live-params.md; HIGH on the signatures, the driver's acceptance is checked on
 * hardware). 0 = ok, -1 ioctl failed, 0x0d000003 bad parameter, 0x0d000011 frame needs a bigger stream buffer, 0x0d000012 set_attr ioctl failed.
 * get_cur_qp / get_rc_stat: the out struct size is not proven (ioctl 4 / 8 bytes): pass at least 64 zeroed bytes. rc_type: 0 or 1 (CBR / VBR, MEDIUM). */
extern int ak_venc_set_fps(int handle, int fps);
extern int ak_venc_set_qp(int handle, int min_qp, int max_qp);
extern int ak_venc_get_attr(int handle, struct venc_param *out);
extern int ak_venc_set_attr(int handle, const struct venc_param *p);
extern int ak_venc_set_rc_type(int handle, int type);
extern int ak_venc_get_cur_qp(int handle, void *out);
extern int ak_venc_get_rc_stat(int handle, void *out);
extern int ak_venc_encode_frame(int handle, const void *data, unsigned len, const void *mdinfo,
                                struct venc_stream *out);
extern int ak_venc_release_stream(int handle, struct venc_stream *s);

#define SENSOR_CFG "/appfs/conf/isp_sc1346_mipi_1lane_av130.bin"

#endif
