#!/usr/bin/env python3
"""calc_partition_sign.py -- host-side only, recomputes the stock YYOS "*_sign" nvram value for a candidate
appfs/rootfs/kernel image, given the recipe reverse-engineered in
docs/porting/lsc-rotatable-2k/flash-signature-findings.md.

Runs on the PC, never on the camera. Python standard library only (pure-Python AES-128-ECB single-block encrypt,
no pycryptodome/cryptography dependency). Never writes nvram, never writes flash, never touches a device.

Algorithm (yy_pack_calc_sign, yy_upgrade 0x3718c):
    sign = hex( AES-128-ECB-encrypt(key, md5(data)) )   # single 16-byte block, no IV, no chaining
    key  = b"yiyiot0518@_YYOS"                           # hardcoded, same on every device (yy_upgrade 0x429a8)

Per-partition "data" (validated against the real nvram *_sign values and the 2026-11-14 flash backup):
  - appfs/rootfs: squashfs image, data = file[0 : round_up(superblock.bytes_used, granularity)]
        appfs granularity  = 0x4000 (16 KiB)
        rootfs granularity = 0x1000 (4 KiB)
  - kernel: the *whole* "kernel" MTD partition dump (env + DTB + uImage), starting at offset 0, up to
        round_up(0x10000 + 64 + uimage_header.size, 128), i.e. env+DTB+uImage header+payload rounded to 128 bytes.
        Pass the full partition dump (or at least its first ~1.5 MiB) as the --kernel-partition file; this script
        locates the uImage header itself (expects it at the fixed offset 0x10000 used on this SoC/layout).

Usage:
    calc_partition_sign.py appfs  path/to/appfs.squashfs
    calc_partition_sign.py rootfs path/to/rootfs.squashfs
    calc_partition_sign.py kernel path/to/kernel-mtd-partition-dump.bin
    calc_partition_sign.py raw    path/to/file N      # hash exactly the first N bytes, for experimentation

Exit codes: 0 ok (prints the 32-hex sign), 1 usage/parse error, 2 file/structure error.
"""

import struct
import sys
import hashlib

KEY = bytes.fromhex("796979696f7430353138405f59594f53")  # "yiyiot0518@_YYOS"

APPFS_GRANULARITY = 0x4000
ROOTFS_GRANULARITY = 0x1000
KERNEL_UIMAGE_OFFSET = 0x10000
KERNEL_ROUND = 128

# --- Minimal pure-Python AES-128 (verified against the FIPS-197 all-zero test vector: see test_aes() below) ---

_SBOX = [
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
]
_RCON = [0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36]


def _xtime(a):
    a <<= 1
    if a & 0x100:
        a ^= 0x11b
    return a & 0xff


def _key_expansion(key):
    nk, nr = 4, 10
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    for i in range(nk, 4 * (nr + 1)):
        temp = list(w[i - 1])
        if i % nk == 0:
            temp = temp[1:] + temp[:1]
            temp = [_SBOX[b] for b in temp]
            temp[0] ^= _RCON[i // nk - 1]
        w.append([w[i - nk][j] ^ temp[j] for j in range(4)])
    return w


def _add_round_key(state, w, rnd):
    for c in range(4):
        for r in range(4):
            state[r][c] ^= w[rnd * 4 + c][r]


def _sub_bytes(state):
    for r in range(4):
        for c in range(4):
            state[r][c] = _SBOX[state[r][c]]


def _shift_rows(state):
    for r in range(1, 4):
        state[r] = state[r][r:] + state[r][:r]


def _mix_single_column(a):
    t = a[0] ^ a[1] ^ a[2] ^ a[3]
    u = a[0]
    a[0] ^= t ^ _xtime(a[0] ^ a[1])
    a[1] ^= t ^ _xtime(a[1] ^ a[2])
    a[2] ^= t ^ _xtime(a[2] ^ a[3])
    a[3] ^= t ^ _xtime(a[3] ^ u)


def _mix_columns(state):
    for c in range(4):
        col = [state[r][c] for r in range(4)]
        _mix_single_column(col)
        for r in range(4):
            state[r][c] = col[r]


def aes128_ecb_encrypt_block(block16: bytes, key16: bytes) -> bytes:
    if len(block16) != 16 or len(key16) != 16:
        raise ValueError("AES-128 needs exactly 16-byte block and key")
    w = _key_expansion(key16)
    state = [[block16[r + 4 * c] for c in range(4)] for r in range(4)]
    _add_round_key(state, w, 0)
    for rnd in range(1, 10):
        _sub_bytes(state)
        _shift_rows(state)
        _mix_columns(state)
        _add_round_key(state, w, rnd)
    _sub_bytes(state)
    _shift_rows(state)
    _add_round_key(state, w, 10)
    out = bytearray(16)
    for c in range(4):
        for r in range(4):
            out[r + 4 * c] = state[r][c]
    return bytes(out)


def test_aes():
    ct = aes128_ecb_encrypt_block(bytes(16), bytes(16))
    assert ct.hex() == "66e94bd4ef8a2c3b884cfa59ca342b2e", "AES self-test failed"


def calc_sign(data: bytes) -> str:
    digest = hashlib.md5(data).digest()
    enc = aes128_ecb_encrypt_block(digest, KEY)
    return enc.hex()


def round_up(n: int, granularity: int) -> int:
    return ((n + granularity - 1) // granularity) * granularity


def squashfs_bytes_used(data: bytes) -> int:
    magic, = struct.unpack_from("<I", data, 0)
    if magic != 0x73717368:
        raise ValueError("not a squashfs image (bad magic %#x)" % magic)
    _root_inode, bytes_used = struct.unpack_from("<QQ", data, 32)
    return bytes_used


def kernel_sign_length(partition_dump: bytes) -> int:
    off = KERNEL_UIMAGE_OFFSET
    magic, = struct.unpack_from(">I", partition_dump, off)
    if magic != 0x27051956:
        raise ValueError("no legacy uImage header at offset %#x (bad magic %#x)" % (off, magic))
    size, = struct.unpack_from(">I", partition_dump, off + 12)
    end = off + 64 + size
    return round_up(end, KERNEL_ROUND)


def main(argv):
    test_aes()
    if len(argv) < 3:
        print(__doc__)
        return 1
    kind = argv[1]
    path = argv[2]
    with open(path, "rb") as f:
        data = f.read()

    if kind == "appfs":
        n = round_up(squashfs_bytes_used(data), APPFS_GRANULARITY)
    elif kind == "rootfs":
        n = round_up(squashfs_bytes_used(data), ROOTFS_GRANULARITY)
    elif kind == "kernel":
        n = kernel_sign_length(data)
    elif kind == "raw":
        if len(argv) < 4:
            print("raw mode needs an explicit byte count N", file=sys.stderr)
            return 1
        n = int(argv[3], 0)
    else:
        print("unknown kind %r (expected appfs|rootfs|kernel|raw)" % kind, file=sys.stderr)
        return 1

    if n > len(data):
        print("computed length 0x%x exceeds file size 0x%x" % (n, len(data)), file=sys.stderr)
        return 2

    print("length=0x%x (%d bytes)  sign=%s" % (n, n, calc_sign(data[:n])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
