#pragma once
/* ISP parameter blocks: the table of the 30 blocks the vendor application writes, the loader of the
 * day / night subfiles of sensor_isp.conf, and a dump that reads the live blocks back from the ISP
 * driver and compares them with the file (specs/image-day-night-leds.md).
 *
 * Nothing here talks to the device by itself: the caller passes a "get" function (the ISP tunnel on
 * the camera, a fake in the host test), so the same code serves ak_rtsp (8091 key img.dump), the
 * isp_dump tool and tests/isp_blocks_test.sh. */

#include <stdint.h>
#include <stdio.h>

#define ISP_BLOCKS_MODULES 24

typedef struct isp_block {
    const char *name;
    uint32_t    set_cmd;       /* inner command the application writes the block with */
    uint32_t    get_cmd;       /* inner command that reads it back, 0 = none known */
    int         get_confirmed; /* 1 = seen in a stock capture or reversed, 0 = guessed (SET + 1) */
    int         module;        /* module (0..23) of sensor_isp.conf that carries the block */
    uint32_t    off;           /* offset of the block inside the module, 4-byte [id][size] header included */
    uint32_t    size;          /* payload bytes */
    uint32_t    skip_words;    /* leading u32 words that are runtime state, ignored when comparing */
} isp_block_t;

/* The whole table, in module order. *n receives the number of entries. */
const isp_block_t *isp_blocks_table(size_t *n);

/* One subfile of sensor_isp.conf (mode 0 = day, 1 = night): the 24 module blocks, header included. */
typedef struct isp_conf {
    uint8_t *data;                       /* the whole file, owned */
    const uint8_t *mod[ISP_BLOCKS_MODULES];
    uint32_t       len[ISP_BLOCKS_MODULES];
} isp_conf_t;

/* 0 on success. -1: unreadable file, no such subfile, or a layout that does not validate. */
int  isp_conf_load(const char *path, int mode, isp_conf_t *c);
void isp_conf_free(isp_conf_t *c);

/* Pointer to the block's payload inside the subfile, NULL when the module is too short for it. */
const uint8_t *isp_conf_payload(const isp_conf_t *c, const isp_block_t *b);

/* Number of differing u32 words between two payloads of `size` bytes (a trailing partial word counts
 * as one), the first `skip_words` words ignored. */
size_t isp_blocks_diff_words(const uint8_t *a, const uint8_t *b, size_t size, size_t skip_words);

/* 32-bit FNV-1a, to show a block's identity in a log line without printing its content. */
uint32_t isp_blocks_hash(const uint8_t *p, size_t n);

typedef int  (*isp_get_fn)(void *ctx, uint32_t get_cmd, void *buf);
typedef void (*isp_emit_fn)(void *ctx, const char *line);

typedef struct isp_dump_opts {
    const char *conf_path;  /* sensor_isp.conf to compare with (day and night subfiles); NULL = no comparison */
    int         all;        /* 1 = also try the getters that are only guessed */
    FILE       *bin;        /* if not NULL: every block read is appended in the isp_blocks.bin format of the stock capture */
    uint32_t    t_ms;       /* time stamp written in those records */
} isp_dump_opts_t;

/* Reads the blocks with `get` (a buffer of at least 64 KiB, zeroed before each call) and emits one line
 * per block plus a summary line. Returns the number of blocks read, -1 if none could be read. */
int isp_blocks_dump(isp_get_fn get, void *gctx, const isp_dump_opts_t *o, isp_emit_fn emit, void *ectx);
