# LSC Rotatable 2K — appfs/rootfs/kernel signature scheme (offline reverse engineering, 2026-11-15)

Goal: fully determine the YYOS firmware signature scheme (§7bis gate) to decide whether a self-built image could
ever be flashed, and whether `yy_upgrade` itself could be used as a card-less install path. **Pure reverse
engineering, no camera write.** Confidence: **HIGH** = read directly from disassembly/data and, where stated,
cross-checked against the real flash backup and the real `nvram` signs; **MEDIUM** = inferred from code shape and
naming, not independently cross-checked; **UNVERIFIED** = not resolved (explicitly flagged, with what was tried).

Binary analysed: `appfs/bin/yy_upgrade` (ARM, stripped of local symbols but keeps a dynamic symbol table with
mangled C++ names — addresses below are its own virtual addresses). U-Boot: `mtd0.bin` (raw Anyka boot image, no
symbol table). Flash backups: `~/akam-backup-lsc-rotatable-2k-20261114_170301/mtd{0..5}.bin`. Everything in this
document was obtained by disassembling these files and by one read-only `nvram show`/`cat /proc/mtd` over SSH; no
`nvram set`, no `fw_setenv`, no `yy_upgrade` execution (not even under `qemu-arm` on the dev host — the task
explicitly rules that out), nothing was written to the camera.

**No `yy_upgrade` was ever run (on the camera or anywhere else) to produce this document; no nvram variable was
ever written. The per-partition "destination device path" field offset (§3.3) is reverse-engineered from
disassembly only, not confirmed by execution.**

## 1. The signature primitive: `yy_pack_calc_sign` (HIGH)

`yy_pack_calc_sign(data, len, out_hex33)` at `0x3718c`:
1. `md5(data, len)` → 16 bytes (uses the bundled `md5`/`md5_wd` at `0x3b25c`/`0x3b2b0`, not system libc).
2. AES-128-ECB **encrypts that single 16-byte digest** with a **hardcoded key**, no IV, no chaining (`aes_expand_key`
   `0x39d0c` + `aes_encrypt` `0x39f50`).
3. `sprintf("%02x"*16, ...)` the 16 ciphertext bytes → 32 lowercase hex chars.

**Key = the 16 ASCII bytes `yiyiot0518@_YYOS`** (hex `796979696f7430353138405f59594f53`), loaded PC-relative at
vaddr `0x429a8`. It is the same for every device (not derived from nvram, MAC, serial, or anything per-unit) —
confirmed by it successfully reproducing the real `appfs_sign`/`rootfs_sign` below.

`yy_pack_calc_file_sign(path, &len_out, out_hex33)` at `0x37364` is a thin wrapper: `fopen`, `fseek(END)`/`ftell`
for the file size, `malloc`+`fread` the **whole file**, then `yy_pack_calc_sign(buf, filesize, out)`. So whatever
file is handed to it gets hashed **in full, by file size** — there is no partial/aligned read inside this function;
any truncation/rounding has to happen before the file is written to disk at build time.

## 2. Validated recipe per partition (HIGH — matches the real `nvram` signs)

AES implementation re-derived in Python, verified first against the standard AES-128 all-zero test vector
(`66e94bd4ef8a2c3b884cfa59ca342b2e`), then used to brute-force the exact byte range hashed for each partition
against the flash backup. `appfs_sign`/`rootfs_sign`/`kernel_sign` were read **once**, read-only, from the live
device's `nvram show` for comparison; they are **not reproduced in this document** (only "match found" / byte
range is recorded) per the project's secrets-handling note.

- **appfs (`mtd3`)**: squashfs superblock `bytes_used` = `0x44d21a` (4 510 234). Sign = `yy_pack_calc_sign` over
  `mtd3.bin[0 : round_up(bytes_used, 0x4000)]` = the first **4 521 984** bytes (`0x44fc00`). **Exact match** against
  the live `appfs_sign`.
- **rootfs (`mtd2`)**: squashfs `bytes_used` = `0x134818` (1 263 640). Sign = over `mtd2.bin[0 :
  round_up(bytes_used, 0x1000)]` = the first **1 265 664** bytes (`0x134000`). **Exact match** against the live
  `rootfs_sign`.
- **kernel (`mtd1`)**: this MTD partition is *not* a bare kernel image — it starts with a duplicated U-Boot env
  (text `uboot_flags=...`, `sf_hz=...`, `mtdparts=...`, `bootcmd=...`), then the DTB at partition offset `0x2000`
  (size `0xE000`, from `dtb_offset`/`dtb_size` in that same env text), then a legacy U-Boot `uImage` at partition
  offset `0x10000` (magic `0x27051956`, header declares `size=1419016` (`0x15a708`), name `Linux-4.4.302-cip94`).
  Sign = over `mtd1.bin[0 : round_up(0x10000 + 64 + 1419016, 128)]` = the first **1 484 672** bytes (`0x16a780`),
  i.e. env + DTB + uImage header + uImage payload, rounded up to a 128-byte boundary. **Exact match** against the
  live `kernel_sign`. (Found by an incremental-MD5 + batched-OpenSSL-AES brute force over the plausible length
  range — not a guess.)

So the per-partition rounding granularity differs (16 KiB for appfs, 4 KiB for rootfs, 128 B for kernel) — almost
certainly an artefact of three different build-time packaging/padding conventions in the vendor's build system, not
something meaningful to replicate conceptually; what matters is that **all three are now reproducible exactly**
from first principles (squashfs `bytes_used` / uImage header `size`, rounded to a partition-specific, now-known
granularity).

**Verdict on forgeability: YES.** Key, algorithm and the exact per-partition length rule are all known and
cross-checked against the three live signs. Anyone who can build a squashfs/uImage can compute a matching
`*_sign` for it offline with the Python helper below — no asymmetric crypto, no per-device secret, no vendor-only
step is involved anywhere in this scheme.

Helper script (host-only, **not installed on the camera**, never writes anything):
`firmware/lsc-rotatable-2k-ak3918av130/tools/calc_partition_sign.py` — `calc_partition_sign.py
<appfs|rootfs|kernel|raw> <file> [N]` prints the hex sign for a candidate image using the recipe above (`kernel`
expects the raw "kernel" MTD partition dump — env + DTB + uImage — and locates the uImage header itself; `raw`
hashes exactly the first `N` bytes, for experimentation). Re-run against the 2026-11-14 backup to confirm it still
reproduces the three live signs: `calc_partition_sign.py appfs mtd3.bin`, `rootfs mtd2.bin`, `kernel mtd1.bin` each
print the length used and the matching 32-hex sign.

## 3. OTA package format (`yy_pack_create_ota_package` `0x3770c`, `yy_pack_check_ota_package` `0x37c44`)

### 3.1 Header (HIGH)

A fixed 1380-byte (`0x564`) header, read whole by `fread`, structured as:

- `+0x00` (16 bytes): MD5 of `header[0x10 : 0x564]` (plain MD5, **no AES, no key** — this is a self-consistency
  checksum, not a signature; `yy_pack_check_ota_package` recomputes it and `memcmp`s). This is the `"md5:.."` the
  OTA tool prints line-by-line (`"md5 : %02x..."` at string `0x32a2c`).
- `+0x10` (4 bytes): magic constant `0x59594f53` (bytes `53 4f 59 59`, a "YYOS" tag) — checked exactly
  (`"magic : 0x%x"` at `0x32aa0`).
- `+0x14` (4 bytes): declared `flash_size`; checked as `flash_size + 0x564 <= actual_file_size` (`"flash_size:0x%x"`
  implied by the same printf block).
- `+0x98` (4 bytes, i.e. offset 152): `ota_partitions_count`, hard-checked `<= 6` (`"ota_partitions_count : %d"` at
  `0x32b18`) — exactly the number of MTD partitions on this SoC (uboot/kernel/rootfs/appfs/config/factory), but
  **nothing in the check function restricts which of the 6 *names* may appear or how many times**.
- `+0x9c` (offset 156) onward: `count` entries of 204 bytes (`0xcc`) each:
  - entry `+0`: partition **name** (C string) — used for `strcmp`-based lookups and for the `nvram_set("%s_sign", ...)`
    / `nvram_set("%s_version", ...)` keys (strings `%s_sign` `0x2b950`, `%s_version` `0x2b924`,
    `"set %s version and sign"` `0x2ba88`).
  - entry `+0xc8` (offset 200, the last 4 bytes of the 204-byte entry): **payload length** for this partition inside
    the package (malloc'd and `fread`'d this many bytes when the data section is read).
  - a **version** string field and a **sign** string field (32 hex chars) also live inside the entry — exact
    sub-offsets are MEDIUM confidence (two different call sites in `yy_pack_create_ota_package` /
    `yy_pack_check_ota_package` compute them via slightly different but arithmetically-equivalent address chains;
    not independently confirmed by execution, see the no-run constraint above). What is HIGH confidence: the
    **sign** stored per entry is exactly `yy_pack_calc_sign(partition_payload_bytes, length, ...)` — i.e. the same
    primitive and the same hardcoded key as §1/§2, computed over the bytes **as stored in the package**, and
    `yy_pack_check_ota_package` enforces `strcmp(stored_sign, recomputed_sign) == 0` for every entry before
    accepting the package.
- Partition **data** follows the header, concatenated in entry order, each slice exactly `length` bytes (confirmed:
  looking up an entry by name `fseek`s forward over every non-matching entry's `length` bytes).

### 3.2 Package validation = self-consistency only (HIGH)

`yy_pack_check_ota_package` only checks: header self-MD5, magic, `flash_size` vs. real file size, partition count
`<= 6`, and per-entry `sign == AES(key, md5(payload))`. **There is no asymmetric signature, no certificate, no
per-device secret, no check against anything already on the camera** (no comparison with the device's own
`nvram` signs, serial, or installed version at this stage). Anyone who knows the key (§1, now public in this
document) can therefore build a package that passes this check for arbitrary partition content — **the package
format itself is fully host-buildable offline**, using only the primitive in §1 and the layout above.

### 3.3 Which partitions can be erased+written (HIGH shape, MEDIUM exact field offset — important safety finding)

Tracing the main upgrade loop (an unnamed static function at `0x178b8`, called from `_Z13yy_upgrade_thPv` /
`0x18178`, itself started by `yy_upgrade()` `0x18484` via `pthread_create`):

1. Calls `yy_pack_check_ota_package` on the package file (§3.2). Any failure here reboots **without** touching any
   MTD device (`"upgrade check failed!"`, `yy_msg_send_event`, `sync`, `sleep(2)`, `reboot`).
2. Otherwise, for `i = 0 .. ota_partitions_count-1`: reads the current `%s_version`/`%s_sign` from `nvram` for that
   entry's **name**, compares to the entry's version/sign; if they already match, that partition is skipped
   (`"%s need not upgrade"`-style path, jumps to the next `i`). Otherwise it `nvram_unset`s the two vars,
   `yy_pack_read_partition_data_by_name(package_path, name, &buf)` to get the payload bytes again, then calls
   `yy_mtd_erase(X)` and `yy_mtd_write(X, buf, len)`, then `nvram_set("%s_version", ...)` / `nvram_set("%s_sign",
   ...)` + `nvram_commit()` (`"set %s version and sign"`, exactly the log line named in the task).
3. `X` (the argument given to `yy_mtd_erase`/`yy_mtd_write`) is read from a field inside the SAME 204-byte entry,
   **not** derived from `/proc/mtd` at this call site (`yy_mtd_erase`/`yy_mtd_write`/`yy_mtd_read_size` all just
   `open()` whatever path string they are handed — none of them parses `/proc/mtd`; that string is referenced
   elsewhere, in a small helper reading a *different* config file, not in this loop). The field's approximate
   offset (entry `+0xb8`/184, MEDIUM confidence, not execution-confirmed) places it in the same entry as the name
   and length, i.e. **it almost certainly travels inside the package too, author-controlled, independently of the
   partition's declared name.**

**Critical safety consequence**: in the code paths traced, **nothing cross-checks the destination path/device
against the partition's own name, against a fixed allow-list, or against `/proc/mtd`.** `yy_upgrade` is a generic
"erase+write whatever this package's entries say, wherever they say" engine gated only by the self-consistency
check in §3.2 — which we can satisfy for *any* content since we know the key. **A package that declared an entry
named e.g. `"uboot"` with a destination pointing at `mtd0`, and a self-consistent sign for whatever bytes we chose,
would — on the evidence gathered here — pass `yy_pack_check_ota_package` and reach `yy_mtd_erase`/`yy_mtd_write`
with no further gate.** This was **not tested** (never run, per the task constraint) — it is a reverse-engineered
reading of the only code path found, not a proven boundary. Until the exact entry layout is nailed down with more
certainty than "medium confidence from static analysis", **any self-built package must be built with exactly one
entry, name `"appfs"`, destination explicitly set to the appfs device, and triple-checked by inspection before it
is ever placed near the trigger file** (§3.5).

### 3.4 No working backup/rollback path (HIGH)

`yy_upgrade_th` first calls a small static function (`0x176f8`) that opens a config file and `sscanf`/`strcmp`s a
line against a fixed key (string `UG_MTD` at `0x2b6a0`) to decide whether a "backup partition" record exists.

- If **no** such record exists (the normal/only case seen on this device): falls straight into the
  `"no backup, upgrading"` path, which is exactly the loop in §3.3 — direct erase+write, no net.
- If a record **does** exist, the code calls a second static function (`0x1789c`) that is a **stub**:
  `push {fp}; mvn r3, #0; pop {fp}; bx lr` — it unconditionally returns `-1`. The caller treats any non-zero result
  as failure and takes the `"backup partition upgrade failed!"` branch: logs, sends the failure event, `sync`,
  `sleep(2)`, `reboot` — **without writing anything**.

So in this exact build, the "has a backup, verify/restore it" branch is permanently broken (always logs failure
and reboots harmlessly), and the only branch that can ever actually write flash is the no-backup, no-rollback one.
**There is no A/B partition, no automatic rollback, no "verify after write and revert if bad" anywhere in this
binary.** A bad write (wrong partition, corrupt image, power loss mid-write) has no software recovery — the only
recovery path is external (CH341A + SOIC8 clip on the SPI NOR), exactly as already planned in archive/rot2k-porting-plan.md §8bis step 5.

### 3.5 Trigger path (HIGH — read directly from the script, not executed)

`appfs/script/init.sh`, in its main polling loop (after `mode`/`start.sh` dispatch):

```sh
if [ -f "/tmp/rmem_blk/yy_upgrade.bin" ]; then
    yyos_upgrade=$(nvram get YYOS_UPGRADE)
    if [ "${yyos_upgrade}" == "on" ]; then
        cp /appfs/bin/yy_upgrade /tmp/rmem_blk/yy_upgrade
        /tmp/rmem_blk/yy_upgrade &
        break
    fi
    sleep 1; continue
fi
```

So triggering a local, card-less, no-cloud upgrade needs exactly two things: (a) a package file at
`/tmp/rmem_blk/yy_upgrade.bin` — `/tmp` is tmpfs (RAM), **placing it there is not a flash write**, and (b) nvram
`YYOS_UPGRADE=on`. **(b) is an nvram write, which is explicitly out of scope for this task and was not done.**
This confirms the path is real and simple (no cloud/Tuya DP involved, no network needed), but also that exercising
it at all — even just to observe `yy_pack_check_ota_package`'s verdict on a self-built package — requires an
nvram write that these instructions forbid; it was not attempted.

## 4. U-Boot (`mtd0`) — reader side (UNVERIFIED enforcement, HIGH evidence it exists)

`strings` on `mtd0.bin` show the **same** vocabulary as `yy_upgrade`: `%s_sign` (`0x2d445`), `"[%s:%d %s] md5 = ..."`
(`0x2d295`), `"[%s:%d %s] sign = %s"` (`0x2d3eb`), `"[%s:%d %s] set %s version and sign"` (`0x2d4c4`,
**byte-identical wording** to `yy_upgrade`'s string, strong evidence the same `yy_pack`/sign C code is statically
linked into both binaries), `"[%s:%d] %s sign error!"` (`0x2d17c`), `"Signature FAILED!"` (`0x2d906`), `"CRC
FAILED!"` (`0x2d937`), and a `bootsecure` command (`0x2f54e`, `"## Error: Secure boot command not specified"`).
This is strong (HIGH) evidence that **U-Boot carries the same md5+AES sign-check code as `yy_upgrade`**, i.e. it
can very plausibly recompute `sign = AES(key, md5(partition_bytes))` itself at boot time and compare it against the
`nvram` `*_sign` values, using the **same recipe validated in §2**.

What could **not** be confirmed in the time available: whether this check actually **runs unconditionally at every
boot**, for **which** partitions (kernel only, or also rootfs/appfs), and whether a mismatch **halts/refuses to
boot** or merely **logs** ("Signature FAILED!" is the string that would print, but nothing was found tying it
definitively to a hard stop vs. a soft warning). The blocker: `mtd0.bin` is a raw, un-relocated Anyka boot image
with **no symbol table and no reliable load-address recovery** in the time budgeted — the same
"ldr r3,[pc,#N]; add r3,pc,r3" PC-relative-literal pattern used throughout `yy_upgrade`'s PIC code was searched for
near every sign-related string (both as plain absolute literals and as the two-instruction relative-offset idiom)
and found **no cross-reference**, meaning either the real pattern differs from `yy_upgrade`'s, or the load base
could not be recovered from static heuristics alone (histogram of `value - file_offset` showed no dominant
candidate). **Marked UNVERIFIED.** A proper answer needs either a UART/JTAG session on U-Boot itself (interactive,
out of scope here — read-only SSH is to a *running Linux* userspace, not a U-Boot prompt) or a correctly-based
disassembly (e.g. from the Anyka SDK's U-Boot source, if ever obtained, or from locating the `AKH322LS` header
format to recover the true link address).

## 5. Verdict for §7bis

- **Signature is forgeable**: YES, with full confidence. Key `yiyiot0518@_YYOS` (AES-128-ECB, no IV), algorithm
  `hex(AES(key, md5(data)))`, and the exact per-partition length rule are all known and independently validated
  against the three live `*_sign` nvram values and the real flash backup (§2). Nothing asymmetric, nothing
  per-device, nothing vendor-exclusive is involved.
- **A card-less, no-cloud install path through `yy_upgrade` itself is plausible and well understood in shape**
  (§3): build a package, `cp` it to `/tmp/rmem_blk/yy_upgrade.bin` (RAM only), set `nvram YYOS_UPGRADE=on`, let
  `init.sh` launch `/appfs/bin/yy_upgrade`. This sidesteps the "does U-Boot enforce the sign" question entirely —
  it's the *vendor's own* writer, using the *vendor's own* recipe, so whatever U-Boot expects afterwards should by
  construction already be satisfied, **provided the package is built correctly**.
- **But `yy_upgrade` has no partition-name allow-list in the code paths traced** (§3.3): the self-consistency check
  only verifies the package is internally coherent, not that its declared destinations are sane. This is the single
  biggest risk in this whole path — a mistake in a self-built package's destination field could, on this reading of
  the code, direct a write at `mtd0` (uboot) or any other partition, with **no backup/rollback** to recover from it
  (§3.4).
- **U-Boot's own enforcement at boot remains UNVERIFIED** (§4): strong textual evidence the same check exists, no
  confirmation of what it gates or whether it would even matter (since §3's path writes with the vendor's own
  signer, it should pass that check "for free" if correctly built — but this has never been exercised).

**Gate stays RED for any real flash write.** This investigation substantially *de-risks the cryptographic side*
(the signature is no longer a mystery and is not the blocker it looked like after the 2026-11-14 pass) but
*surfaces a new, more concrete risk on the mechanism side* (no destination allow-list, no rollback). Before any
real attempt, in order:
1. Pin the exact per-entry struct (name/destination/version/sign/length byte offsets) with certainty — ideally by
   finding the vendor's own package-building tool/script rather than only reverse-engineering the reader, since a
   byte-offset mistake here is exactly the failure mode that would misdirect a write.
2. Have the CH341A + SOIC8 clip recovery path proven (full read-back dump compared against today's backup) —
   archive/rot2k-porting-plan.md §8bis step 5 prerequisite, independent of all of the above.
3. Only then consider a real, single-partition (`appfs` only), full-backup, explicit-user-go attempt — and still
   never touch `mtd0`.

This is a feasibility read, not a go — flashing stays gated on hardware recovery and an explicit user "go"
regardless of how well understood the signature scheme now is.

## Addendum (2026-11-15): U-Boot boot path does not enforce the signature

Static reading of the backup `mtd0.bin` (strings + default environment; no disassembly of `bootm`):

- Default env boot flow: `bootcmd=run boot_normal`; `boot_normal` = set bootargs, `run read_kernel`,
  `run read_dtb`, `bootm ${loadaddr} - ${fdtcontroladdr}`. `read_kernel`/`read_dtb` are plain
  `sf probe 0:0; sf read`. **There is no verify / sign / bootsecure step in the boot command.**
- The kernel load uses the standard U-Boot legacy-uImage path (`## Booting kernel from Legacy Image`,
  `Verifying Checksum`, `Bad Data CRC`, `Bad Magic Number`) — a **CRC** check only, not a signature.
- The signature strings (`Signature FAILED!`, `sign = %s`, `set %s version and sign`, key
  `yiyiot0518@_YYOS`) are clustered with the source path
  `.../uboot/common/yy_upgrade/yy_upgrade.c` and the NVRAM strings: they belong to the **yy_upgrade
  code compiled into U-Boot** (update-time integrity), not to the boot path.
- `bootsecure` is a standalone command whose only companion string is "Secure boot command not
  specified"; `bootcmd` never invokes it.

Conclusion (confidence MEDIUM-HIGH, static only): **the normal boot does not gate on
appfs/rootfs/appfs_sign**, like the 1080p. A correctly-placed modified appfs is expected to boot. Not
yet proven against a disassembly of `bootm` or a possible secure-boot fuse; a UART session (see
[uart-recovery.md](uart-recovery.md) step 0) would confirm it directly and safely. The §7bis gate
therefore rests on the *mechanical* risks of writing flash (never touch mtd0; yy_upgrade has no
destination allow-list and no rollback), not on the signature, which is both forgeable and apparently
unenforced at boot.
