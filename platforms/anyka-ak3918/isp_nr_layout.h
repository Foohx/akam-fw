#pragma once
/* Decoded layouts of the 1080p ISP noise-reduction and sharpening blocks (GC20C3, ak_isp.ko of the stock image) and of
 * the ISO-level selection that the kernel applies to them. Reference: specs/video-and-streaming.md section 5.5, which
 * gives the evidence level of every field (RE = read in the ak_isp.ko disassembly, conf = value pattern in
 * isp_gc20c3.conf, guess). Only the layout is fixed here; this header holds no code.
 *
 * Every linkage block has the same outer shape (RE, _isp_para_change_with_envi):
 *   u32 mode;               0 = manual (the kernel uses entry[0]), else linkage (all 3 subfiles of the conf: 1)
 *   entry[17];              entry[0] = manual, entry[1 + L] = ISO level L (0..15), one discrete table per level
 * The level L comes from _get_iso() (section 5.5.1), there is no interpolation between levels.
 * All fields are little-endian u16 unless noted. Offsets are relative to the start of one entry. */

#include <stddef.h>
#include <stdint.h>

#define ISP_LINK_ENTRIES 17 /* manual + 16 ISO levels */
#define ISP_LINK_LEVELS  16

/* 17-point Gaussian weight table used by NR1, NR2, UVNR, 3D_NR and SHARP:
 *   lut[i] = max(floor, round(1023 * exp(-256 * i * i / s))), i = 0..15, lut[16] = last (0 or floor, kept as is)
 * where s is the "strength" u16 stored next to the table (the kernel does not read s; the vendor tool generated the
 * table from it). Checked on all 354 tables with s > 0 of the 3 subfiles: within +-1 LSB. 10-bit values. */
#define ISP_GAUSS_LUT_N 17

/* ---------------------------------------------------------------- NR1 (0x4004490c, 3064 B) */
typedef struct isp_nr1_entry {
    uint16_t enable;           /* +0x00 bit 0                                   RE */
    uint16_t f02;              /* +0x02 4 bits (4..5)                            RE, meaning unknown */
    uint16_t f04;              /* +0x04 5 bits (31 down to 0 as the level rises) RE, meaning unknown */
    uint16_t strength[3];      /* +0x06 s of the 3 weight tables (equal)         conf; not read by the kernel */
    uint16_t intensity_lut[17];/* +0x0c 10 bits, 0..1023 rising                  RE (17 fields), meaning guess */
    uint16_t knots[16];        /* +0x2e not read by the kernel (tool data)       RE (absent from _set_nr1_attr) */
    uint16_t weight[3][17];    /* +0x4e 3 Gaussian range-weight tables, 10 bits  RE + conf (generator) */
} isp_nr1_entry_t;

typedef struct isp_nr1_block {
    uint32_t        mode;
    isp_nr1_entry_t e[ISP_LINK_ENTRIES];
} isp_nr1_block_t;

/* ---------------------------------------------------------------- NR2 (0x4004492b, 992 B) */
typedef struct isp_nr2_entry {
    uint16_t enable;           /* +0x00 bit 0                                   RE */
    uint16_t f02;              /* +0x02 bit 0                                   RE */
    uint16_t f04;              /* +0x04 4 bits                                  RE */
    uint16_t f06;              /* +0x06 5 bits                                  RE */
    uint16_t f08;              /* +0x08 4 bits (15)                             RE */
    uint16_t f0a;              /* +0x0a 4 bits (15)                             RE */
    uint16_t strength;         /* +0x0c s of the weight table                   conf; not read */
    uint16_t weight[17];       /* +0x0e Gaussian weights; [0] (1023) not read   RE + conf */
    uint16_t f30;              /* +0x30 bit 0                                   RE */
    uint16_t f32;              /* +0x32 10 bits (50/30/20)                      RE */
    uint16_t f34, f36, f38;    /* +0x34..+0x38 bit 0 each                       RE */
} isp_nr2_entry_t;

typedef struct isp_nr2_block {
    uint32_t        mode;
    isp_nr2_entry_t e[ISP_LINK_ENTRIES];
    uint16_t        pad;       /* 4 + 17 * 58 = 990, the ioctl copies 992 */
} isp_nr2_block_t;

/* ---------------------------------------------------------------- UVNR (0x4004497d, 720 B) */
typedef struct isp_uvnr_entry {
    uint16_t enable;           /* +0x00 bit 0                                   RE */
    uint16_t f02;              /* +0x02 bit 0                                   RE */
    uint16_t f04;              /* +0x04 4 bits (2..4)                           RE */
    uint16_t strength;         /* +0x06 s of the weight table                   conf; not read */
    uint16_t weight[17];       /* +0x08 Gaussian weights; [0] (1023) not read   RE + conf */
} isp_uvnr_entry_t;

typedef struct isp_uvnr_block {
    uint32_t         mode;
    isp_uvnr_entry_t e[ISP_LINK_ENTRIES];
    uint16_t         pad;      /* 4 + 17 * 42 = 718, the ioctl copies 720 */
} isp_uvnr_block_t;

/* ---------------------------------------------------------------- 3D NR (0x4004492f, the ioctl copies 7180 B) */
typedef struct isp_3dnr_entry {
    uint16_t enable;           /* +0x000 bit 0                                  RE */
    uint16_t f002;             /* +0x002 bit 0                                  RE */
    uint16_t f004;             /* +0x004 2 bits                                 RE */
    uint16_t motion_lo;        /* +0x006 13 bits: motion ramp start             RE (lo/hi + 65536/(hi-lo) slope), meaning guess */
    uint16_t motion_hi;        /* +0x008 13 bits: motion ramp end (> lo)        RE; the kernel bumps hi when hi < lo */
    uint16_t f00a;             /* +0x00a 0                                      conf */
    uint16_t luma_tbl[128];    /* +0x00c 4 bits each (3..8), 16 regs of 8       RE, meaning guess (per-luma noise level) */
    uint16_t f10c;             /* +0x10c                                        unknown */
    uint16_t curve[16];        /* +0x10e rising 0..128 curve                    conf, use unknown */
    uint16_t f12e;             /* +0x12e 4 bits (2..10)                         RE */
    uint16_t strength;         /* +0x130 s of the weight table                  conf; not read */
    uint16_t weight[17];       /* +0x132 Gaussian weights; [0] not read, 10 bit RE + conf */
    uint16_t tail[38];         /* +0x154..+0x19f two parallel parameter groups  RE (widths in spec 5.5.5), meaning unknown */
} isp_3dnr_entry_t;

/* 6-byte companion entry that the kernel reads right after the 17 3D NR entries (_set_3d_nr_rc_attr), selected with
 * the 3D NR mode word. Stock zero-fills it (anyka_ipc isp_set_attr: memset 0x1c0c then copies the 7076 conf bytes). */
typedef struct isp_3dnr_rc_entry {
    uint16_t enable;           /* +0 bit 0 -> 3D NR register 0x20 bit 19         RE */
    uint8_t  v0;               /* +2 -> register 0x7c bits 0..7                  RE */
    uint8_t  pad;
    uint8_t  v1;               /* +4 -> register 0x7c bits 8..15                 RE */
    uint8_t  pad2;
} isp_3dnr_rc_entry_t;

typedef struct isp_3dnr_block {
    uint32_t            mode;
    isp_3dnr_entry_t    e[ISP_LINK_ENTRIES];
    isp_3dnr_rc_entry_t rc[ISP_LINK_ENTRIES];
    uint16_t            pad;
} isp_3dnr_block_t;

#define ISP_3DNR_CONF_BYTES  7076u  /* what sensor_isp.conf carries (module 4 minus its header) */
#define ISP_3DNR_IOCTL_BYTES 7180u  /* what ak_isp.ko copy_from_user()s for SET and copy_to_user()s for GET */

/* ---------------------------------------------------------------- SHARP (0x40044925, 46584 B) */
/* Only the fields that are proven are named; the rest are opaque byte ranges. Offsets relative to the entry. */
#define SHARP_E_ENABLE        0x000 /* u16, 1 in all linkage entries                  conf */
#define SHARP_E_DN_STRENGTH   0x03a /* u16 s of the pre-sharpen Gaussian table        conf */
#define SHARP_E_DN_LUT        0x018 /* 17 x u16 Gaussian table (floor 50 or 1)         conf (generator) */
#define SHARP_E_LUT_A         0x674 /* 256 x s16 detail transfer curve                RE (stock slider scales them) + conf shape */
#define SHARP_E_LUT_B         0x874 /* 256 x s16 second transfer curve                same */
#define SHARP_E_LUT_N         256

/* The 8 coring thresholds: the stock slider moves them inversely to the sharpness (inv_gain_q8, clamp 255) and the
 * vendor raises them with the ISO level (4 -> 10 and 8 -> 15): "sharpen only above this detail amplitude". */
#define SHARP_E_CORING_OFFS_INIT { 0x644, 0x648, 0x652, 0x656, 0x65c, 0x660, 0x666, 0x66a }
/* The 8 gains that go with them (gain_q8, clamp 255). */
#define SHARP_E_GAIN_OFFS_INIT   { 0x646, 0x64a, 0x654, 0x658, 0x65e, 0x662, 0x668, 0x66c }
/* The 4 q4 gains (clamp 15). */
#define SHARP_E_GAIN_Q4_OFFS_INIT { 0x63e, 0x64c, 0x65a, 0x664 }

#define SHARP_ENTRY_SIZE   0xab4u
#define SHARP_BLOCK_SIZE   (4u + ISP_LINK_ENTRIES * SHARP_ENTRY_SIZE) /* 46584 */
#define SHARP_ENTRY_OFF(k) (4u + (unsigned)(k) * SHARP_ENTRY_SIZE)   /* k = 0 manual, 1 + level */

/* ---------------------------------------------------------------- ISO level thresholds (SET_EXPOSURE, 1824 B) */
#define EXPO_ISO_PAIRS_OFF 0x98 /* 16 x {u32 lo, u32 hi} in units of the gain index G / 256 (RE + conf) */
#define EXPO_ISO_FLAG_OFF  0x08 /* u32: when != 0 and run_info[+16] < EXPO_ISO_MIN_OFF word, G = 0 (RE) */
#define EXPO_ISO_MIN_OFF   0x3c

_Static_assert(sizeof(isp_nr1_entry_t) == 180, "NR1 entry");
_Static_assert(offsetof(isp_nr1_entry_t, intensity_lut) == 0x0c, "NR1 lut");
_Static_assert(offsetof(isp_nr1_entry_t, weight) == 0x4e, "NR1 weights");
_Static_assert(sizeof(isp_nr1_block_t) == 3064, "NR1 block");
_Static_assert(sizeof(isp_nr2_entry_t) == 58, "NR2 entry");
_Static_assert(offsetof(isp_nr2_entry_t, weight) == 0x0e, "NR2 weights");
_Static_assert(offsetof(isp_nr2_entry_t, f30) == 0x30, "NR2 tail");
_Static_assert(sizeof(isp_nr2_block_t) == 992, "NR2 block");
_Static_assert(sizeof(isp_uvnr_entry_t) == 42, "UVNR entry");
_Static_assert(sizeof(isp_uvnr_block_t) == 720, "UVNR block");
_Static_assert(sizeof(isp_3dnr_entry_t) == 416, "3DNR entry");
_Static_assert(offsetof(isp_3dnr_entry_t, luma_tbl) == 0x0c, "3DNR luma table");
_Static_assert(offsetof(isp_3dnr_entry_t, f12e) == 0x12e, "3DNR f12e");
_Static_assert(offsetof(isp_3dnr_entry_t, weight) == 0x132, "3DNR weights");
_Static_assert(offsetof(isp_3dnr_entry_t, tail) == 0x154, "3DNR tail");
_Static_assert(sizeof(isp_3dnr_rc_entry_t) == 6, "3DNR rc entry");
_Static_assert(offsetof(isp_3dnr_block_t, rc) == ISP_3DNR_CONF_BYTES, "3DNR rc after the conf bytes");
_Static_assert(sizeof(isp_3dnr_block_t) == ISP_3DNR_IOCTL_BYTES, "3DNR ioctl size");
_Static_assert(SHARP_BLOCK_SIZE == 46584, "SHARP block");
_Static_assert(SHARP_E_LUT_B + 2 * SHARP_E_LUT_N <= SHARP_ENTRY_SIZE, "SHARP LUT B inside the entry");
