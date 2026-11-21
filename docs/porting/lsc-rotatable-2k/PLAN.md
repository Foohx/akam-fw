# Porting plan — LSC Smart Rotatable Camera 2K (Dual Band)

> Historical document: the file paths below predate the repository reorganisation (`src/common`, `src/ak_rtsp`, `firmware/<model>/app`); the old to new table is `tools/dev/path-map.txt`.

> Hand-off document. It is self-contained: a fresh conversation should be able to read **only this file**
> (plus the files it references) and start executing. Written 2026-11-14.
>
> **Model id (proposed):** `lsc-rotatable-2k-ak3918av130`
> **Camera:** LSC Smart Rotatable Camera 2K, "Dual Band", sold at Action, article **`3225722`**, label code `SZ G26312`.
> **Chosen strategy:** phased — **(1) a fully-local camera first** (keep the stock app, cut the cloud, enable local
> RTSP/ONVIF/PTZ, add SSH + an SD rescue card), **(2) then move toward a native Akam port** once everything works.
> Go **progressively**, with **no regression** on the validated 1080p firmware and a **repo where it is obvious which
> code belongs to which model**.

---

## 0. TL;DR for the executor

1. This is **not** the same hardware stack as the 1080p. Same SoC (AK3918AV130) but a **newer video userspace**
   (Anyka **MPI SDK**: `libplat_vi/vpss/isp_sdk`, `libmpi_venc`, Allegro encoder), and the OS is **YYOS / TuyaOS v1.6**,
   not the old Anyka SDK. `ak_rtsp`'s capture/encode layer (`/dev/video-0-0`, `/dev/venc-chn0` ioctls) **does not apply**
   here. See [§2](#2-hardware--firmware-findings-analysis-a).
2. We already have **root + a verified full flash backup** and a **working SD rescue card**. See [§3](#3-what-we-already-have).
3. Phase 1 lives **entirely on the SD card** (YYOS `mode=debug` → our launcher → hands off to stock `yy_cam`): **no
   flash writes, fully reversible**. This card is also the rescue card. See [§5](#5-phase-1--fully-local-camera).
4. Keep `src/ak_rtsp/` (the 1080p firmware) **untouched**. All rotatable code goes under a directory named
   `lsc-rotatable-2k-ak3918av130`. See [§4](#4-repo-architecture-clean-per-model-separation).
5. **Golden rule (both cameras):** never write `mtd0` (U-Boot: secure-boot, unrecoverable brick without an SPI
   programmer). Phase 1 writes **nothing** to flash.

---

## 1. Goal and constraints

- **Product goal:** a camera that works **fully local** — live video (RTSP), discovery/control (ONVIF), motor (PTZ),
  **no Tuya cloud** — with **SSH** access and an **SD rescue card** (telnet) as a safety net. Matches the Akam vision:
  light, performant, integrates into any home-automation setup with standards (RTSP, ONVIF, MQTT/HA).
- **Hard constraint 1 — no regression on the 1080p.** The validated firmware (`lsc-indoor-1080p-ak3918`,
  `src/ak_rtsp`) must keep building byte-for-byte and keep passing CI. Rotatable work must not modify its sources.
- **Hard constraint 2 — clean, legible repo.** From a path alone, one must know which model a file serves. Everything
  model-specific lives under a directory named exactly after its model id; shared code stays in the shared dirs.
- **Working style:** progressive, reversible steps; worktrees for repo changes; Opus coordinates, Sonnet does
  spec/dev/review in worktrees, merge local, no push without the user. Per-camera flash writes always need an explicit
  user "go". (See memory `dev-workflow-agents`, `akam-vision`, `camyn-project-state`.)

---

## 2. Hardware & firmware findings (analysis A)

Backed by the offline analysis of the flash backup (rootfs `mtd2` and appfs `mtd3`, both squashfs-4.0/xz/128 KiB).

### 2.1 Platform

| Item | Value |
|---|---|
| SoC | Anyka **AK3918AV130** (ARM926EJ-S, ARMv5TEJ, single core, no FPU) — **same family as the 1080p** |
| OS | **YYOS / TuyaOS v1.6** (build path `…/yyos_v1.6/…`), **not** the old Anyka SDK |
| Kernel | Linux `4.4.302-cip94 #1 Tue Jun 9 17:48:59 CST 2026`, `armv5tejl` |
| RAM | ~38 MiB usable (`MemTotal 38112 kB`) |
| Flash | 8 MiB SPI NOR, erase size 4 KiB |
| Sensor | **SmartSens SC1346** (driver `sensor_sc1346.ko`, insmod with `check_id=1`); ISP tuning blob `/appfs/conf/isp_sc1346_mipi_1lane_av130.bin` (MIPI 1-lane) |
| Video encoder | **Allegro** (`/dev/allegro`, `ak_venc_adapter.ko` + `ak_venc_bridge.ko`) |
| ISP | **ISP2** (`ak_isp.ko`) |
| NPU | present: `drivers/ak_npu.ko` (v2.0.17), `libplat_npu.so`, `libak_nne.so`, `libmpi_svp2.so`, `libakv_cnn_dt.so`, model `resource/model/model_0x0A00000D_V1.00.01.bin` (~960 KiB, proprietary). `yy_cam` drives an SVP2 "hd" detector (`ak_av130_svp2_hd_*`: score/IOU thresholds, target type, skip frames — "hd" = likely human detection, UNVERIFIED) plus classic motion detection (`ak_av130_svp_md_*`). Also `libakAIcrydetect.so` (audio cry detection). Native NPU use is Phase 3; reusing the stock detections is a Phase 2 lead (§6) |
| Wi-Fi | **Altobeam ATBM6132C** dual-band (`atbm6132c.ko`, `/dev/atbm_ioctl`), `wpa_supplicant` |
| Motor / PTZ | **MS41929 stepper driver IC** (`/dev/ms41929`) + Tuya motor node `/dev/tymotor` (`ty_driver.ko`); `ak_pwm_char.ko`, `ak_saradc.ko` |
| Audio | `ak_pcm.ko` |
| App | **`/appfs/bin/yy_cam`** (~3.6 MiB, monolithic: video/audio/RTSP/ONVIF/PTZ/cloud), `wpa_supplicant`, `yy_upgrade` |

### 2.2 Flash map (`/proc/mtd`, 6 partitions — DIFFERENT from the 1080p's 8)

| MTD | Name | Size | Type | Notes |
|---|---|---|---|---|
| mtd0 | uboot | 0x00040000 (256K) | raw | **NEVER write** (secure-boot, brick) |
| mtd1 | kernel | 0x00180000 (1.5M) | uImage | — |
| mtd2 | rootfs | 0x00150000 (1.3M) | squashfs xz 128K | BusyBox userland |
| mtd3 | appfs | 0x00490000 (4.56M) | squashfs xz 128K | `yy_cam`, `lib/`, `drivers/`, `conf/`, `script/` |
| mtd4 | config | 0x00040000 (256K) | jffs2 (writable) | holds `/config/param.conf` and nvram-like state |
| mtd5 | factory | 0x00020000 (128K) | raw | MAC, calibration |

### 2.3 Boot chain

1. U-Boot → kernel → mounts rootfs (`mtd2`) and appfs (`mtd3`).
2. Stock `init` runs **`/appfs/script/init.sh`**, which reads **`/mnt/sdcard/yy_debug.ini`**:
   - `mode=normal` → runs `/appfs/script/start.sh`.
   - `mode=debug`  → runs the `start_sh=` script from the card (this is our root hook), **instead** of `start.sh`.
   - `mode=factory` → unpacks `fac_av130.zip` from the card (factory test).
   - Then loops: watches for a mode change (reboots if it changes), low-mem `drop_caches`, and a `yy_upgrade` path.
3. **`/appfs/script/start.sh`** symlinks `/appfs/conf`→`/tmp/conf`, `insmod`s the drivers (efuse, pcm, i2c, **isp**,
   **sensor_sc1346 `check_id=1`**, **venc_adapter**, **venc_bridge**, npu, ty_driver, saradc, pwm_char, rtc), sets
   `eth0` MAC from `nvram get MASTER_MAC`, starts `telnetd` if `/mnt/sdcard/yy_telnet_start` exists, tunes VM, and
   finally `/appfs/bin/yy_cam &` **only if `nvram get STATUS == active`**.
4. nvram keys seen: `STATUS`, `MASTER_MAC`, `YYOS_UPGRADE`.
5. **OTA mechanism (read from stock `init.sh`):** the init loop waits for `/tmp/rmem_blk/yy_upgrade.bin` (downloaded by
   `yy_cam` from the cloud); when it exists **and** `nvram get YYOS_UPGRADE == on`, it copies `/appfs/bin/yy_upgrade` to
   RAM and runs it (this is what writes flash). So an OTA needs the cloud: blocking WAN egress blocks OTA. Our scripts
   must never create that file, set that nvram key, or run `yy_upgrade`. Note: changing `mode=` in `yy_debug.ini` while
   running makes init reboot the camera (harmless).

### 2.4 The decisive finding — why `ak_rtsp` is not a drop-in

`yy_cam` does **not** use the raw device nodes `ak_rtsp` drives on the 1080p (`/dev/isp-param-0`, `/dev/video-0-0`,
`/dev/venc-chn0`). It links the vendor **MPI SDK** in `/appfs/lib/`: `libplat_vi.so`, `libplat_vpss.so`,
`libplat_isp_sdk.so`, `libmpi_venc.so`, `libplat_ao/ai.so`, `librtsp_server.so`, … The encoder is Allegro (`/dev/allegro`),
the ISP is "isp2". So:

- **Reusable from `ak_rtsp` (hardware-independent upper layers):** RTSP (smolrtsp), control port, MQTT + HA discovery,
  ONVIF logic, day/night policy, config file handling, the SSH/lockdown/NTP helper scripts, the installer/kit/release
  machinery and the `akam` assistant (with additive model-awareness).
- **NOT reusable as-is (hardware layer):** capture + encode + ISP + AE. On this camera that means a **new backend against
  the MPI SDK**, which also means linking vendor `.so` (so **not** the static-musl model). This is the Phase 3 cost and
  the reason we do **not** replace `yy_cam` in Phases 1–2.

### 2.5 Stock app's built-in local capabilities (what Phase 1 leans on)

`yy_cam` already contains RTSP, ONVIF (incl. **ONVIF PTZ** builders — `onvif_AddPTZConfiguration`, `yy_ptz_motor.cpp`),
and PTZ, **disabled by default** (cloud mode only; only Tuya port `6668` is open out of the box). They are enabled by:

- **RTSP/ONVIF:** Tuya LAN **DP 237 = true**, or `/config/param.conf` `[rtsp] rtsp_on_off=1` + `[onvif] onvif_on_off=1`,
  then restart `yy_cam` (stock history: the stock app serves :8554, the Akam own app :554). Streams: `rtsp://<ip>:8554/stream0` (main 1080p), `/stream1` (sub). ONVIF:
  `http://<ip>:5000/onvif/device_service`, user `admin`, empty password (to be hardened).
- **PTZ:** Tuya LAN **DP 119** (direction `0`–`7`) and **DP 116** (stop); also the native `/dev/ms41929` / `/dev/tymotor`.
  `param.conf [ptz] ptz_pan_step_home=1275 ptz_tilt_step_home=816`.

> These DP/port facts come from the reference repo (see §9) and our own `strings` of `yy_cam`; **each must be re-verified
> on the live camera** in Phase 1 before being written into a spec.

---

## 3. What we already have

- **Root access:** `telnet <camera-ip>`, `root` / `root` (or a direct `/bin/sh` on port 23), via the SD rescue card.
- **Full flash backup (safety net), verified faithful, kept in two copies next to each other:**
  - `~/akam-backup-lsc-rotatable-2k-20261114_170301/`
  - `~/akam-backup-lsc-rotatable-2k-20261114_170301-copy2/`
  - Each: `mtd0.bin`…`mtd5.bin`, `MD5SUMS`, `SHA256SUMS`, `README.txt`. **Proprietary — never commit, never share.**
  - TODO (user): a third copy on a **separate medium**.
- **A working SD rescue/maintenance card** (the user's 4 GB card, label `AKAMSD`), prepared with the YYOS method
  (`yy_debug.ini` `mode=debug` → `hack.sh`). It also still carries the old 1080p toolkit (inert on YYOS) and
  `hack.sh.1080p.bak`. The static busybox on it is verified (`sha256 367d142b…`).
- **Offline-extracted firmware** for analysis in the session scratchpad (`rootfs-extract/{rootfs,appfs}`) — ephemeral;
  re-extract from the backup with `unsquashfs mtd2.bin` / `mtd3.bin` when needed.

---

## 4. Repo architecture (clean per-model separation)

**Principle:** one directory per model id; a `grep -r lsc-rotatable-2k-ak3918av130` finds *all* of its code. The 1080p
stays where it is so nothing about it changes.

### 4.1 Target layout (add, don't move)

```
src/
  ak_rtsp/                     # 1080p native firmware — UNCHANGED in Phases 1–2
  common/  smolrtsp/           # shared C — UNCHANGED
firmware/                      # NEW top-level home for per-model, non-ak_rtsp deliverables
  lsc-rotatable-2k-ak3918av130/
    overlay/                   # the SD-card tree that boots the camera fully-local (Phase 1)
      yy_debug.ini             #   mode=debug, start_sh=/mnt/sdcard/akam_boot.sh
      akam_boot.sh             #   our launcher: enable telnet+ssh, decloud, enable rtsp/onvif, then start.sh
      decloud.sh               #   block Tuya cloud egress (reversible)
      enable_local.sh          #   turn on RTSP/ONVIF (+ verify ports), via DP or param.conf
      ssh/                     #   dropbear + authorized_keys handling (key-only root)
      ...
    rescue/                    # the telnet-only rescue card (minimal: yy_debug.ini + hack.sh + telnetd + busybox)
    tools/                     # host-side helpers: ptz.py, enable_rtsp_onvif.py, onvif probe, etc.
    README.md                  # what this model is, how the overlay boots, how to build the card
docs/
  porting/lsc-rotatable-2k/    # THIS plan + the future as-built spec
  models.md                    # NEW index: model-id → dir, firmware kind, SoC, status  (the one-stop map)
tools/release/
  models/
    lsc-indoor-1080p-ak3918.txt            # existing
    lsc-rotatable-2k-ak3918av130.txt       # NEW registry entry (see §7)
```

- **Firmware "kind".** The 1080p is a *native-app image* (replaces the APP partition). The rotatable in Phases 1–2 is a
  *decloud overlay* (keeps the stock app, boots from SD). The registry/kit format must carry a `kind` field so the
  assistant treats them differently. Keep `kind` additive and defaulted to `native-app` so the 1080p file is unchanged.
- **Busybox binary** for the overlay/rescue card is a build artifact, not committed: fetch it from the reference
  release and verify its sha256 at card-build time (documented in the model README), like we do for other blobs.
- Do **not** relocate `src/ak_rtsp` into `firmware/lsc-indoor-1080p-ak3918/` yet. A later, isolated, well-tested step can
  do that move for symmetry; it is explicitly **out of scope** until Phase 2 is stable, to protect constraint 1.

### 4.2 No-regression rules (enforce in review/CI)

- Rotatable changes must touch **none** of: `src/ak_rtsp`, `src/common`, `third_party/smolrtsp`, the 1080p model file, the
  existing image/kit build path for `native-app`.
- CI keeps the current 1080p jobs exactly; **add separate jobs** for the rotatable (shellcheck the overlay scripts,
  python lint the tools, a card-build dry run, a fingerprint check of the model file). A rotatable job failing must not
  be able to mask a 1080p job.
- Any assistant change for model-awareness (layout table, firmware kind) is **keyed by model id** and covered by a test
  proving the 1080p path is byte-identical.

---

## 5. Phase 1 — fully-local camera

**Deliverable:** boot the camera with the SD card inserted and get **local live video + ONVIF + PTZ, no cloud, with
SSH**, writing **nothing** to flash. The same card is the rescue card. Everything under
`firmware/lsc-rotatable-2k-ak3918av130/overlay/`.

**Mechanism:** `yy_debug.ini` `mode=debug`, `start_sh=/mnt/sdcard/akam_boot.sh`. `akam_boot.sh` runs as root at boot and:

1. **Maintenance/rescue:** start `busybox telnetd -l /bin/sh -p 23` (passwordless root) + set a known `root/root`
   via a tmpfs `/etc/shadow` bind-mount (RAM only). *(= today's `hack.sh`, kept.)*
2. **SSH (Dropbear, key-only root):** drop `dropbear` + host keys in `/tmp`, read `authorized_keys` from the card
   (`overlay/ssh/authorized_keys`), start `dropbear` on port 22, password login disabled. Reuse the 1080p's Dropbear
   ARM build and key handling conventions (`specs/network-time-security.md`). Keys live on the card, never in the repo.
3. **Decloud (`decloud.sh`, reversible):** block all WAN egress while keeping the LAN — blackhole `0.0.0.0/1` and
   `128.0.0.0/1` routes (survive DHCP renew; no iptables on the box) and/or a DNS sinkhole over `/etc/hosts`. Gate it
   behind a card flag so it can be toggled without reflashing the card.
4. **Enable local services (`enable_local.sh`):** turn on RTSP + ONVIF. Prefer the **runtime DP path (DP 237=true)** to
   avoid any flash write; fall back to `param.conf` only if the DP path proves unreliable (that write lands in the
   writable `config` partition, not firmware — acceptable, but note it).
5. **Hand off:** `exec`/background **`/appfs/script/start.sh`** so the stock camera (`yy_cam`, drivers, Wi-Fi) comes up
   normally. (In `mode=debug`, init runs us *instead* of `start.sh`, so calling it is mandatory.)

**Validation on real hardware** (one unit, call it `akam-rot-01`; user "go" required for each boot):

- [ ] Card boots, telnet `root/root` works, SSH key login works, cloud is unreachable (camera still on LAN).
- [ ] `rtsp://<ip>:554/stream0` and `/stream1` play (ffmpeg/VLC); note real resolution/fps/codec.
- [ ] ONVIF discovery + `GetStreamUri` + snapshot work; record the exact device-service URL and credentials.
- [ ] PTZ moves via ONVIF **and** via the native path; capture pan/tilt ranges and step counts.
- [ ] Reboot **without** the card → camera returns to untouched stock behaviour (proves reversibility).
- [ ] Measure RAM/CPU headroom with services on (budget gate, per `akam-vision`).
- [ ] **NPU / AI detection, read-only check:** with decloud active, does `yy_cam` still load the NPU (`lsmod | grep npu`,
      `/proc/<yy_cam pid>/maps` shows `libplat_npu`/model, `dmesg | grep -i npu`) and still run detections (look for
      detection lines in `yy_cam` output/logs while walking in front of the camera, compare motion-only vs person)?
      Is the result exposed anywhere locally (ONVIF events / `GetEventProperties`, RTSP metadata, log)? Record what the
      detector targets (person/pet/vehicle) and the NPU RAM/CPU cost. No writes. Feeds the Phase 2 decision.
- [ ] **Decloud proof:** from the router/PC, confirm the camera makes no WAN connection for 30 min (router connection
      list or a `tcpdump` on a mirror/AP if available; on camera `netstat -tn` shows no non-LAN peer); `ping 8.8.8.8`
      from the camera fails while LAN ping works; Tuya app shows the camera offline.

**Exit criteria:** a documented, repeatable card build; the above checklist green on `akam-rot-01`; an as-built
spec drafted in `docs/porting/lsc-rotatable-2k/`.

---

## 6. Phase 2 — Akam integration (still no video reimplementation)

Make the local camera a first-class Akam citizen, reusing Akam's integration layer rather than its capture layer.

- **MQTT + Home Assistant discovery bridge.** A small agent (host-side or on-camera, TBD by RAM budget) that publishes
  HA discovery and exposes the camera's stream/PTZ/status over MQTT, talking to the camera via its **ONVIF/RTSP/Tuya-LAN
  DP** interfaces. Reuse the design of `src/ak_rtsp/mqtt_ha.*` and `specs/mqtt.md`; do not fork the 1080p binary.
- **`akam` assistant: make it model-aware (additive).**
  - Detect this model from the stock **appfs (`mtd3`) sha256** and/or its `/proc/mtd` (6-partition) layout.
  - The current installer compares the whole `/proc/mtd` against a single hard-coded 8-line table → it would **refuse**
    this camera. Move the expected layout into the per-model registry file and compare against the detected model's.
  - Teach `status`/`backup`/`update` the `kind=decloud-overlay` case: "backup" = the flash dump we already script;
    "install/update" = build + write the SD overlay card (no APP flash). Keep `native-app` behaviour identical.
- **Persistence (optional, later in Phase 2).** Decide whether to keep running from the SD card (simplest, reversible)
  or bake the overlay into `appfs` (`mtd3`) for a card-less install. Baking means writing flash → full backup + user
  "go" + a rollback path first. Default: **stay on SD** until there is a strong reason not to.
- **Model registry entry + kit.** Finalize `tools/release/models/lsc-rotatable-2k-ak3918av130.txt` and a per-model kit
  (overlay tar) in the release flow, reusing signing/verification. The assistant picks the newest release carrying a
  kit for the detected model (as it already does for the 1080p).

**Exit criteria:** the camera appears in Home Assistant with video + PTZ + status, managed through `akam`, with the
1080p flow unchanged and CI green for both.

---

## 7. Model registry entry (sketch)

Create `tools/release/models/lsc-rotatable-2k-ak3918av130.txt` by copying the 1080p file and its header comment, then:

- display name: `LSC Smart Rotatable Camera 2K (Dual Band)`; article `3225722`; label `SZ G26312`.
- SoC `AK3918AV130`; sensor `SC1346`; Wi-Fi `ATBM6132C`; flash 8 MiB.
- **`kind = decloud-overlay`** (new field; default elsewhere is `native-app`).
- expected `/proc/mtd` = the 6-line table in §2.2 (for the layout check).
- stock fingerprints, status `untested` first: kernel string (§2.1), **sha256 of stock `appfs` (`mtd3`)** from our
  backup, squashfs format (xz/128K), BusyBox version (read it on the camera). Flip to `validated` only after Phase 1
  passes on real hardware.
- Register it in `tools/assistant/internal/assets/sources.txt` and run `go generate ./internal/assets` (a test fails
  otherwise), exactly as `docs/compatible-cameras.md` → "Adding a new model" describes.

> Get the stock `appfs` sha256 from the backup: `sha256sum ~/akam-backup-lsc-rotatable-2k-20261114_170301/mtd3.bin`.

---

## 7bis. Gate before ANY flash write on the 2K — appfs (`mtd3`) signature / integrity check

U-Boot (`mtd0`) has secure boot. Before Phase 2 "bake the overlay into appfs" or Phase 3 "replace `yy_cam`" ever writes
`mtd3`, we must **prove that a modified appfs will still boot**. Until this gate is green, **no write to `mtd3`** (nor
`mtd1`/`mtd2`). All steps below are read-only.

**Offline (on the flash backup, no camera needed):**
1. `mtd0` (U-Boot): `strings` for `bootcmd`, `bootargs`, `sf read`, `bootm`, `fit`, `rsa`, `sha256`, `hash`, `verify`,
   `signature`, `secure`; identify exactly what U-Boot loads and verifies (expected: only the kernel `mtd1`).
2. `mtd1` (kernel): check config / strings for `dm-verity`, `IMA`, `EVM`, `CONFIG_SQUASHFS`-only mounts, any
   signature-check code on block devices; extract the embedded cmdline if any.
3. `mtd2`/`mtd3`: compare the squashfs `bytes_used` with the partition size and inspect the trailing bytes for an
   appended signature/hash block; check the rootfs init scripts for any hash/signature check before mounting appfs.
4. `yy_upgrade`: find whether OTA packages are signature-checked (update-time) and whether it writes any
   hash/signature/flag elsewhere (`config` jffs2, nvram) that a boot-time check could read.

**Read-only on the camera (via the SD rescue telnet):** `/proc/cmdline`, `/proc/mounts`, `dmesg | grep -iE
'verity|ima|sign|hash|secure'`, `fw_printenv`/U-Boot env if readable, `nvram show` (read only).

**Verdict (written in this file, with evidence):**
- **GREEN** = U-Boot verifies only the kernel, appfs is mounted by Linux with no integrity check → `mtd3` can be
  rebuilt (same rules as the 1080p: full backup, read-back verify, rollback image, user "go").
- **RED / UNKNOWN** = any integrity check on appfs or unclear → **no `mtd3` write**; stay on the SD-card approach
  (Phases 1–3 can all run from the card) until resolved.

---

### 7bis — findings 2026-11-14 (palier 3, read-only) → verdict: **RED / UNKNOWN — no flash write**

- `/proc/cmdline`: `root=/dev/mtdblock2 rootfstype=squashfs ro`, `mtdparts` = the 6 partitions of §2.2; no dm-verity/IMA in
  dmesg. Rootfs `/etc/init.d/rcS` mounts appfs with a plain `mount -t squashfs /dev/mtdblock3 /appfs` (no check there).
- **But** nvram holds `kernel_sign`, `rootfs_sign`, `appfs_sign` (32-hex, md5-sized) plus `*_version`, and U-Boot (`mtd0`)
  contains md5 code printing `md5 = %02x…` and a `%s_sign` format, plus `bootsecure`. So U-Boot very likely md5-checks
  partitions against these nvram values at boot (at least the kernel; possibly rootfs/appfs).
- The values do **not** equal a plain md5 of the partition, of the squashfs `bytes_used`, or of any 512-byte-aligned
  prefix → the hashed region/format is unknown (OTA-package hash, keyed, or header-based).
- Consequence: rewriting `mtd3` (or `mtd1`/`mtd2`) might fail this check and the boot. **Stay on the SD card.** Unblocking
  needs the exact algorithm (disassemble the U-Boot md5/`_sign` path, and `yy_upgrade`, offline) **and** a recovery path
  that does not depend on Linux booting (UART console to U-Boot, or CH341A on the SPI NOR), before any flash write.

### 7bis — findings 2026-11-15 (offline `yy_upgrade`/U-Boot disassembly) → verdict stays **RED — no flash write**

Full write-up: `flash-signature-findings.md`. Pure reverse engineering (disassembly + one read-only `nvram
show`/`cat /proc/mtd`); no `nvram set`, no `fw_setenv`, no `yy_upgrade` run anywhere (not even under `qemu-arm` on
the dev host), nothing written to the camera.

- **The signature algorithm is now fully known and cross-checked against the three live `*_sign` values**:
  `sign = hex(AES-128-ECB(key="yiyiot0518@_YYOS", md5(data)))`, same hardcoded key for every device
  (`yy_pack_calc_sign`, `yy_upgrade` `0x3718c`/`0x429a8`). The exact per-partition byte range is validated: appfs =
  `mtd3[0 : round_up(squashfs.bytes_used, 0x4000)]`, rootfs = `mtd2[0 : round_up(squashfs.bytes_used, 0x1000)]`,
  kernel = `mtd1[0 : round_up(0x10000+64+uimage.size, 128)]` (env+DTB+uImage). A host-only helper reproduces all
  three live signs exactly: `firmware/lsc-rotatable-2k-ak3918av130/tools/calc_partition_sign.py`.
  **The signature is forgeable with on-hand information — no asymmetric crypto, no per-device secret anywhere.**
- **New, more concrete risk found**: `yy_upgrade`'s OTA-package check (`yy_pack_check_ota_package`) only verifies
  the package is internally self-consistent (header self-md5, magic, size, count ≤ 6, per-entry
  `sign==AES(key,md5(payload))`) — it does **not** cross-check a partition entry's destination device against its
  declared name, a fixed allow-list, or `/proc/mtd`. On the evidence gathered (medium confidence on the exact
  struct byte offset, not execution-confirmed — the task forbids running `yy_upgrade` at all, even under emulation),
  a malformed self-built package could in principle misdirect a write at `mtd0` (uboot). There is also **no
  backup/rollback**: the only "has a backup" code path is a stub that always fails harmlessly; the real write path
  has zero rollback net.
- **Trigger for a card-less, no-cloud install is simple and real** (read from `appfs/script/init.sh`, not
  executed): drop a package at `/tmp/rmem_blk/yy_upgrade.bin` (tmpfs, not a flash write) and set `nvram
  YYOS_UPGRADE=on` (an nvram write — out of scope here, not performed) to make `init.sh` launch
  `/appfs/bin/yy_upgrade`.
- **U-Boot evidence, not confirmed**: `mtd0` carries the byte-identical sign-check vocabulary (`%s_sign`, `md5 = `,
  `sign = %s`, `set %s version and sign`, `Signature FAILED!`, `bootsecure`) as `yy_upgrade`, strongly suggesting
  the same check is statically linked into both — but whether it gates boot (and for which partitions) is
  **UNVERIFIED**: `mtd0.bin` is a raw, symbol-less Anyka boot image and its load base could not be recovered from
  static heuristics in the time budgeted (no UART/JTAG session attempted — out of scope for a read-only-SSH task).
- **Verdict unchanged, for a stronger reason**: the signature is no longer the blocker (it's forgeable, recipe
  known) — but using the vendor's own `yy_upgrade` writer sidesteps that question anyway, at the cost of a *new*
  risk (no destination allow-list found, no rollback). Gate stays RED until (1) the exact OTA-package entry layout
  is pinned with certainty (not just medium-confidence static analysis), (2) the CH341A+SOIC8 recovery path is
  proven by a read-back dump, and (3) an explicit user "go" for a single-partition (`appfs`-only), full-backup
  attempt — `mtd0` is never touched regardless.

### Palier results 2026-11-14 (hardware, unit akam-rot-01)

- Palier 1 (overlay, no decloud, no param.conf): telnet + SSH key OK, hand-off OK, Tuya app online with live video, no
  watchdog reboot, ~16–18 MB RAM available with video, CPU idle at rest.
- Palier 2 (decloud flag): IPv4 blackholes set at +3 s (before `yy_cam`), `ping 8.8.8.8` fails, LAN OK, a Tuya cloud SYN
  (AWS :443) never completed, Tuya app **offline over 4G**. The kernel has **no IPv6** (no `/proc/net/if_inet6`) → no v6
  egress. DNS still resolves via the router (only lookups, no connections). The phone app also reaches the camera on the
  LAN (TCP 6668). Multicast route added once wlan0 is up. `udhcpc` running → long-run route persistence still to watch.
- Palier 3 (read-only): listening TCP 22/23/6668, UDP 7000; RTSP/ONVIF **off**. Stock BusyBox **v1.30.1**.
  `/config/param.conf` has sections `[dsp] [cloud] [dp] [led] [ptz]` only (no `[rtsp]`/`[onvif]` yet); `yy_cam` does
  contain the keys `rtsp`, `onvif`, `rtsp_on_off`, `onvif_on_off`, `/config/default_param.conf`, a `rm -rf
  /config/param.conf` path, URL formats `rtsp://%s/stream0:%d` and `http://%s:%d/onvif/device_service`.
  NPU: `ak_npu` loaded (used by 1), `/dev/npu` present, `yy_cam` maps `libplat_npu`, `libak_nne`, `libmpi_svp2`
  (model file not mmapped). `yy_cam` RSS ~8 MB, 31 threads. Detection behaviour without cloud: still to observe.
  `nvram show` prints **device secrets** (Tuya auth/access keys, UUID, SN): never log or commit its values.

- Palier 4 (card removed): stock again — 22/23 closed, 6668 open, Tuya app online with video over 4G. Card back: local mode
  restored, SSH host key stable.
- Palier 5 (opt-in `akam/enable_param_conf`, after a `/config` tar backup on the card + on the PC, md5-identical; stock
  has no `tar`, use the card busybox): `enable_local.sh` appended `[rtsp] rtsp_on_off=1` and `[onvif] onvif_on_off=1`;
  `yy_cam` accepted it. **RTSP 8554 + ONVIF 5000 open.** `rtsp://<ip>:8554/stream0` = H.264 **2304×1296 @ 15 fps** +
  G.711 µ-law 8 kHz mono; `/stream1` = H.264 640×360 @ 15 fps + same audio; **no RTSP auth**. ONVIF
  `http://<ip>:5000/onvif/device_service`, WS-Security digest **user `admin`, empty password**; services device, media,
  events, ptz, imaging; profiles `PROFILE_0`/`PROFILE_1`; events expose only `tns1:RuleEngine/MotionDetector/Motion`
  (no person topic seen). **ONVIF PTZ not usable yet:** no PTZ configuration attached to the profiles
  (`GetConfigurations` empty, `ContinuousMove` → "PTZ Profile Not Exist") → investigate (`yy_cam` strings/param keys) or
  use the Tuya-LAN DP path. Security: RTSP/ONVIF are open to the whole LAN without real credentials → hardening item for
  Phase 2 (ONVIF password, RTSP auth if supported, or LAN-side firewalling).

- Point 4 (image quality), 2026-11-14: ONVIF `SetVideoEncoderConfiguration` is accepted (only with Quality in 21..50)
  but does NOT reconfigure the real encoder. The real knobs are `[dsp] video_chn0_*` keys read by `yy_cam` at start
  (defaults: AVBR `enc_mode`=4, max 800 / target 630 kbit/s, QP 32..51). Live test with, under `[dsp]`:
  `video_chn0_max_bitrate = 2048`, `video_chn0_max_bit_rate = 2048`, `video_chn0_target_bitrate = 1536`,
  `video_chn0_enc_mode = 1` (VBR), `video_chn0_max_qp = 42` → measured from packet timestamps **~1.9 Mbit/s at
  14.3 fps**, P-frames ~13 KB (vs ~2–5 KB stock), I-frames ~90 KB (vs ~41 KB); yy_cam ~5 % CPU, ~18 MB RAM available.
  ONVIF now reports BitrateLimit 2048 (keys are read). Visual check by the user pending. Original param.conf kept on the
  card (`akam/param.conf.before-hd`, also `akam/backup/param.conf.orig`). Note: `ffmpeg -t` byte counts under-estimate
  fps/bitrate at stream start; measure from packet timestamps.

---

## 8. Phase 3 — toward a native Akam port (later, only once Phases 1–2 are solid)

Replace `yy_cam` with an Akam app. The hard part is a **new video backend against the MPI SDK** (`libplat_vi/vpss`,
`libmpi_venc`, Allegro). Scope when we get there:

- Decide the build model: link the vendor `.so` from `appfs/lib` (so a non-static, uclibc/glibc build) vs. any
  ioctl path that may still exist. This breaks `ak_rtsp`'s static-musl assumption → likely a **separate app target**,
  not a compile flag on `ak_rtsp`.
- Reuse the upper layers (RTSP/control/MQTT/ONVIF/day-night) behind a clean hardware interface so the 1080p and the
  rotatable share them without `#ifdef` soup.
- Sensor/ISP bring-up for SC1346 (ISP2 + `isp_sc1346_mipi_1lane_av130.bin`), Allegro encode params, AE, IR/IR-cut/LED
  pin mapping (read from the camera — not assumed), PTZ driver (`/dev/ms41929` + `/dev/tymotor`) exposed as ONVIF PTZ.
- Develop and validate the replacement app **from the SD card first** (`yy_cam` stopped, nothing flashed).
- Only then, and only if the §7bis gate is GREEN, consider replacing the APP (`appfs`/`mtd3`) with an Akam image,
  with the usual backup + rollback + user go.

This phase is explicitly deferred; Phases 1–2 deliver the user's "full local camera" without it.

---

## 8bis. Roadmap after Phase 1 (decided 2026-11-14)

Goal stays: **a flashed camera, no SD card left inside**, with our own app instead of `yy_cam`. Order:

1. **Motor (PTZ):** ONVIF PTZ is not wired in this `yy_cam` build (see `ptz-findings.md`); drive `/dev/tymotor` with the
   tiny `akam_ptz` tool from the card, later expose it as ONVIF PTZ / MQTT from Akam.
2. **AI detection (NPU):** does the stock detection still run without the cloud, and can we get its events locally?
3. **Decloud persistence:** blackhole routes vs DHCP renewals (`udhcpc`), re-assert if needed.
4. **Image quality:** stream0 measured at ~770 kbit/s for 2304×1296@15 (P-frames ~5 KB) → noise and motion artefacts.
   Find where `yy_cam` sets bitrate / rate control / ISP noise reduction (param.conf, Tuya "HD/SD" DP, hard-coded).
5. **Unlock flashing (offline, no risk):** reverse the U-Boot `%s_sign` md5 check and `yy_upgrade` (what region/format is
   hashed into `kernel_sign`/`rootfs_sign`/`appfs_sign`, any keyed or asymmetric signature?), and look for a lighter
   hook: does anything at boot execute or load from the writable, unchecked `/config` (jffs2, 256 KiB)?
   **Prerequisite hardware for any later flash write:** a CH341A programmer + SOIC8 clip (external SPI NOR rewrite =
   recovery even if U-Boot/Linux no longer boot), tested by a read-only dump compared with our backup.
6. **Our own app, from the SD card:** replace `yy_cam` at boot (the card launcher starts our app instead), using the
   on-camera vendor libraries dynamically (uClibc, soft-float ARMv5; `libapp_video`, `libplat_vi/vpss/isp_sdk`,
   `libmpi_venc`, `libplat_npu`) — never redistributed. Separate toolchain/target from the 1080p (musl static) so the
   1080p cannot regress. Start with a proof of concept: one encoded frame from the sensor. Then RTSP/ONVIF/MQTT reuse,
   auth, bitrate control, PTZ, IR/LED. Remove the card → stock again.
   **Time sync (done, verified on the camera 2026-11-15: `akam/ntp` with the router as server, `sync ok` about 1 min after boot, clock in UTC):** opt-in via the card file `akam/ntp` (1080p format, `server=NAME`
   lines, e.g. your router's IP). The stub runs the shared `akam_ntp.sh` unchanged with `AKAM_NTPD` pointing at
   `akam2k_sntp`, a tiny one-shot SNTP client (BusyBox on this camera has no `ntpd`). Details and caveats:
   `own-app-feasibility.md`, section 3 "Time". Without `akam/ntp`: no process, no traffic; decloud does not block a LAN server.
   **Audio (offline study, 2026-11-15):** stock uses `ak_ai_*` (`libplat_ai`) at 8 kHz mono S16 with its own G.711 code; the 2K `ak_pcm.ko` has the
   same ioctl ABI as the 1080p, so a first mic PoC can reuse the 1080p raw `/dev/pcmC0D0c` method and the shared `rtp_clock`/`rtsp_sdp`/`g711` code
   (L16 track first). Findings, budgets, risks and a step plan: `audio-findings.md`.
   **Step 3 (mic PoC) DONE on the hardware 2026-11-15** (ioctl sequence accepted, 8 kHz mono s16, 7961.5 Hz effective, 0 errors, quiet room RMS about -55 dBFS, live signal; `audio-findings.md` §8.1). Next: level check with a voice. **RTSP track implemented (opt-in `audio=1` in `akam/akam2k.conf`, off by default; L16/8000 PT 97 on both streams, RTCP SRs, hardware soak pending): `audio-findings.md` section 9.**
7. **Flash (card-less install):** only when 5 is GREEN (algorithm known, no unforgeable signature), 6 is validated from the
   card, the CH341A recovery is proven, full backup + rollback image + read-back verify, and an explicit user "go".
   Write only what is needed (appfs/`mtd3` and the matching `appfs_sign`, never `mtd0`).

---

## 8ter. Shared code between firmwares (decided 2026-11-14)

Goal (user): one Akam code base whose optimised parts are shared by every model, so a fix or an optimisation lands
everywhere, while each model keeps its own hardware layer.

- **Shared (hardware-independent):** RTSP/RTP (smolrtsp + `rtsp*.c`, `rtp_*`, `tx_pace`, `net_tx`), RTSP auth, SDP/H.264
  params, control port (`control.c`, `ctl_*`), config files (`cfg_file`, `cred`), MQTT + Home Assistant discovery
  (`mqtt*`), ONVIF/SOAP/WS-Discovery (`onvif*`, `soap`, `wsd`), HTTP server, day/night *policy* (`night_logic`), audio
  codec/jitter/VAD (`g711`, `audio_jitter`, `audio_vad`), hashes (`md5`, `sha1`, `b64`), `time32_shim`, logging.
- **Per model (hardware abstraction):** video capture/encode (1080p: `vi.c`/`venc.c` ioctls; 2K: vendor MPI SDK),
  ISP/AE, IR-cut/IR light/LEDs/GPIO map, audio device, PTZ (2K only), watchdog, Wi-Fi bring-up (2K: gpio10 + atbm).
  Defined as a small C interface (`hal_video`, `hal_light`, `hal_ptz`, `hal_audio`, …); each model provides one
  implementation under its own directory.
- **Build:** the same shared sources compile with both toolchains (1080p: zig/musl static; 2K: Bootlin uClibc dynamic).
  Shared code must stay portable C (no libc-specific extensions, no model `#ifdef` soup — model differences go through
  the HAL or capability flags).
- **Zero-regression rule while sharing:** (1) phase A — the 2K app *reuses* existing shared files unchanged (compile them
  from their current place); (2) phase B — any extraction of upper layers out of `src/ak_rtsp` into a shared dir is a
  pure move/refactor that must leave the 1080p binary **byte-identical** (sha256 compared against `dev`, as done for the
  rotatable branches) — if not byte-identical, it needs the full 1080p test suite + a 1080p hardware validation before
  merge; (3) every later improvement to shared code runs both models' test suites in CI, and hardware validation on
  both cameras before a release.
- Order: start phase A with the 2K PoC-3 (RTSP) by linking `third_party/smolrtsp` + the RTSP/SDP files as they are; do phase B
  only when the 2K app needs a shared piece that is currently entangled with 1080p hardware code.

---

## 9. References, facts to re-verify, and risks

**Reference project (exact match for our camera, use for facts only — verify before trusting, mind its licence):**
- `github.com/alex-so-3/lsc-ak3918-yyos-root` — SD-card root for AK3918 YYOS, tested on "LSC Smart Rotatable Camera 2K
  (dual-band)", Action art. `3225722`. Source of: `yy_debug.ini`/`hack.sh` method, static busybox release
  (`busybox-v1.36.1`, asset `busybox-1.36.1-ak3918-armv5te-static`, sha256 `367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290`), DP map (`docs/FINDINGS.md`), `tools/ptz.py`, `tools/enable_rtsp_onvif.py`.
- `github.com/ErikBavenstrand/anyka-ak3918ev330-tuya-ipc-firmware` — a `yy_cam` binary patch for local RTSP (DNS
  sinkhole decloud). Useful as a second opinion on the decloud + enable-local mechanics.

**Must be re-verified on the live camera before being written into a spec** (do not ship facts taken from third parties):
RTSP/ONVIF ports and URLs, ONVIF credentials, DP numbers (116/119/237), PTZ ranges/steps, real stream resolution/fps,
BusyBox version, the stock `appfs` fingerprint, and that the decloud routes/sinkhole actually stop all cloud traffic.

**Risks / watch-outs:**
- **OTA = defence in depth:** (1) camera-side WAN blackhole (`decloud.sh`), (2) **recommended: also block the
  camera's internet access in the router** (independent of the camera, survives a boot without the card), (3) never
  press "update" in the Tuya app. Without the card the camera is stock and can reach the cloud again, unless (2).
- **Firmware update locks.** The app offered `V14.1.7` (current `V14.1.5`). **Do not update** — a newer YYOS build may
  close the `mode=debug` hook. Pin the finding; if it ever closes, fall back to UART / CH341A (we have the backup).
- **Watchdog.** `yy_cam` likely feeds `/dev/watchdog`. Our boot hook must not starve it (hand off to `start.sh`
  promptly; keep `akam_boot.sh` non-blocking).
- **Secure-boot on mtd0.** Never write U-Boot. Phase 1 writes no flash at all. Any future `mtd3` write is blocked
  until the §7bis signature/integrity gate is GREEN.
- **Licence / provenance.** Treat any third-party binary/script as untrusted; audit before running on hardware (we did
  for `hack.sh`), verify checksums, and check licences before importing anything into the repo.
- **MPI SDK vendor `.so`.** Phase 3 depends on vendor libraries whose licence and ABI stability are unknown — assess
  early if/when Phase 3 starts.

---

## 10. First actions for the next conversation

1. Create the model directory skeleton `firmware/lsc-rotatable-2k-ak3918av130/{overlay,rescue,tools}` + `docs/models.md`,
   in a worktree, touching nothing under `src/`.
2. Lift today's working rescue card into `rescue/` (versioned: `yy_debug.ini`, `hack.sh`, `telnetd`; busybox fetched +
   checksummed at build time) and write a `README.md` that builds the card reproducibly.
3. Build `overlay/akam_boot.sh` (steps in §5) + `decloud.sh` + `enable_local.sh` + Dropbear/SSH, and run the §5
   validation checklist on `akam-rot-01` (user "go" per boot).
4. Draft `tools/release/models/lsc-rotatable-2k-ak3918av130.txt` (status `untested`) and the `kind` field design.
5. Keep the 1080p build/CI green throughout; no file under `src/ak_rtsp` changes.
```
