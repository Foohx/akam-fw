#!/usr/bin/env bash
# Assemble the SD-card tree of the LSC Rotatable 2K (lsc-rotatable-2k-ak3918av130) into a DIRECTORY.
#
#   firmware/lsc-rotatable-2k-ak3918av130/image/build_card.sh <dir> --authorized-keys FILE [options]   overlay card
#   firmware/lsc-rotatable-2k-ak3918av130/image/build_card.sh <dir> --rescue-only                      rescue card
#
# <dir> is the mounted card (or any directory to copy from later). This script NEVER formats, partitions,
# mounts or writes a block device (no dd, no mkfs, no parted): the user prepares and mounts the card.
# It refuses a path that is not an existing directory, and "/". It only adds or overwrites the files it owns;
# it never deletes anything.
#
# Options:
#   --authorized-keys FILE   public keys for SSH (ssh-ed25519 lines), copied to <dir>/akam/authorized_keys.
#                            Required for the overlay. Never read from the repository.
#   --rescue-only            telnet-only rescue card: yy_debug.ini + hack.sh + telnetd + busybox
#   --decloud                create the flag akam/decloud (block WAN egress at boot)
#   --enable-param-conf      create the flag akam/enable_param_conf (opt-in: edit /config/param.conf, with a
#                            backup to the card first; this writes the jffs2 config partition)
#   --no-telnet              create the flag akam/no_telnet (no passwordless telnet rescue)
#   --own-app                create the flag akam/own_app (PoC-1: boot WITHOUT yy_cam; the stub feeds the
#                            watchdog and brings Wi-Fi up; 3 unfinished boots fall back to stock). The stub is
#                            always copied; only the flag arms it.
#   --wpa-conf FILE          copy FILE to <dir>/akam/wpa.conf (mode 600) for the stub; never read or printed.
#                            Without it the builder neither creates nor overwrites akam/wpa.conf.
#   --own-app-bin DIR        directory with akam2k_rtsp + BUILDINFO.txt (made by `make poc3-dist` in app/) to install as
#                            akam/bin/akam2k_rtsp (+ akam/bin/akam2k_rtsp.BUILDINFO). Default: $AKAM_OWN_APP_DIST or
#                            dist/rot2k-app when akam2k_rtsp is there. Refused unless BUILDINFO proves symcheck=passed,
#                            matches the binary and the CURRENT sources, and the binary is a 32-bit ARM ELF with the
#                            interpreter /lib/ld-uClibc.so.0. Only that one file is copied: vendor libs are never
#                            shipped (the camera's /appfs/lib is used at run time). The stub starts it when it is
#                            on the card (flag akam/own_app); optional akam/akam2k.conf is never created here.
#   (time sync) akam/akam_ntp.sh (the shared 1080p script, unchanged) is always copied; akam/bin/akam2k_sntp (+ its
#                            BUILDINFO) is copied when make poc3-dist built it (same dist directory as the app, refused unless
#                            BUILDINFO matches it and the current sntp sources). Nothing runs until YOU create akam/ntp.
#   -h, --help
#
# Environment:
#   AKAM_ROT2K_BUSYBOX        local path of busybox-1.36.1-ak3918-armv5te-static for offline builds (its sha256
#                             is verified exactly like a download)
#   AKAM_DROPBEAR_DIST        directory with dropbearmulti + BUILDINFO.txt (default: dist/dropbear)
#   AKAM_PTZ_DIST             directory with akam_ptz + BUILDINFO.txt (default: dist/akam_ptz), built by
#                             ptz/build_akam_ptz.sh. Optional: without it the card has no PTZ tool (a note is
#                             printed); a binary that does not match its BUILDINFO.txt or the current source is
#                             a hard failure.
#   AKAM_OWN_APP_DIST         default of --own-app-bin (default: dist/rot2k-app)
#   AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY   replaces the expected sha256, for the tests only
#
# The static busybox is a build artifact, never committed: fetched from the GitHub release busybox-v1.36.1 of
# alex-so-3/lsc-ak3918-yyos-root and verified against a pinned sha256 (hard failure on mismatch).
# Dropbear is the one built by tools/dropbear/build_dropbear.sh (static ARM, time32 shim: the camera kernel
# 4.4.302 does not run musl's time64 clock_gettime syscall). BUILDINFO.txt must prove that the binary is the
# shimmed build of the CURRENT shim source; otherwise the card build fails.
#
# Safety: the card never gets an upgrade or factory trigger. yy_debug.ini is always "mode=debug" (checked, never
# mode=factory), fac_av130.zip, *.bin and update payloads are never copied, and a target that already holds
# fac_av130.zip or a yy_upgrade* file is refused (other .bin/.zip/.img files: a warning).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FWD=$(cd "$HERE/.." && pwd)
REPO=$(cd "$HERE/../../.." && pwd)

BB_NAME=busybox-1.36.1-ak3918-armv5te-static
BB_URL=https://github.com/alex-so-3/lsc-ak3918-yyos-root/releases/download/busybox-v1.36.1/$BB_NAME
BB_SHA256=367d142bf8d25751e4d8c63442ee99779865a10e2073a28b28795510a3091290
# The test-only override of the expected sha256 is honoured ONLY together with a local busybox file
# (AKAM_ROT2K_BUSYBOX): a download or a cached copy is always checked against the pinned hash, so a stray
# environment variable cannot weaken a real build.
if [ -n "${AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY:-}" ] && [ -n "${AKAM_ROT2K_BUSYBOX:-}" ]; then
    BB_SHA256=$AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY
    echo "build_card: WARNING: AKAM_ROT2K_BUSYBOX_SHA256_TEST_ONLY is set: the pinned busybox hash is NOT enforced (tests only)" >&2
fi

die() { echo "build_card: error: $*" >&2; exit 1; }
log() { echo "build_card: $*"; }
usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d;s/^# \{0,1\}//'; }

target=""
keys=""
rescue_only=0
f_decloud=0
f_param=0
f_notelnet=0
f_ownapp=0
wpaconf=""
appbin=""
while [ "$#" -gt 0 ]; do
    case $1 in
        -h | --help) usage; exit 0 ;;
        --rescue-only) rescue_only=1 ;;
        --authorized-keys)
            [ "$#" -ge 2 ] || die "--authorized-keys needs a file"
            keys=$2
            shift
            ;;
        --decloud) f_decloud=1 ;;
        --enable-param-conf) f_param=1 ;;
        --no-telnet) f_notelnet=1 ;;
        --own-app) f_ownapp=1 ;;
        --own-app-bin)
            [ "$#" -ge 2 ] || die "--own-app-bin needs a directory"
            appbin=$2
            shift
            ;;
        --wpa-conf)
            [ "$#" -ge 2 ] || die "--wpa-conf needs a file"
            wpaconf=$2
            shift
            ;;
        -*) die "unknown option: $1 (see --help)" ;;
        *)
            [ -z "$target" ] || die "only one target directory is accepted"
            target=$1
            ;;
    esac
    shift
done

[ -n "$target" ] || { usage >&2; exit 2; }
[ -d "$target" ] || die "$target is not a directory (mount the card first; this script does not touch devices)"
target=$(cd "$target" && pwd -P)
[ "$target" != / ] || die "refusing to use / as the card directory"
[ -w "$target" ] || die "$target is not writable"
if [ "$rescue_only" = 1 ] && { [ -n "$keys" ] || [ "$f_decloud" = 1 ] || [ "$f_param" = 1 ] || [ "$f_notelnet" = 1 ] || [ "$f_ownapp" = 1 ] || [ -n "$wpaconf" ] || [ -n "$appbin" ]; }; then
    die "--rescue-only takes no key and no overlay flag"
fi
if [ "$rescue_only" = 0 ]; then
    [ -n "$keys" ] || die "the overlay needs --authorized-keys FILE (your SSH public keys; never taken from the repository)"
    [ -f "$keys" ] || die "$keys is not a file"
    grep -Eq '^ssh-ed25519 AAAAC3NzaC1lZDI1NTE5' "$keys" || die "$keys holds no ssh-ed25519 public key line"
    if grep -Eq 'PRIVATE KEY' "$keys"; then die "$keys looks like a PRIVATE key; give the .pub file"; fi
    [ -z "$wpaconf" ] || [ -f "$wpaconf" ] || die "$wpaconf is not a file"
fi

# --- anti-brick: the target must not already hold an upgrade or factory trigger ------------------------------
refuse=0
while IFS= read -r p; do
    [ -n "$p" ] || continue
    echo "build_card: REFUSED: $p is an upgrade/factory trigger; remove it from the card by hand and run again" >&2
    refuse=1
done < <(find "$target" -maxdepth 2 \( -iname 'fac_av130.zip' -o -iname 'yy_upgrade*' \) 2>/dev/null)
[ "$refuse" = 0 ] || exit 1
while IFS= read -r p; do
    [ -n "$p" ] || continue
    echo "build_card: warning: $p on the card (a firmware-like file); it is not used, consider removing it" >&2
done < <(find "$target" -maxdepth 2 \( -iname '*.bin' -o -iname '*.img' -o -iname '*.squashfs' -o -iname '*.zip' \) 2>/dev/null)

# --- the static busybox: local override or download, always verified -------------------------------------------
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
sha_of() { sha256sum "$1" | cut -d' ' -f1; }
bb=$work/busybox
if [ -n "${AKAM_ROT2K_BUSYBOX:-}" ]; then
    [ -f "$AKAM_ROT2K_BUSYBOX" ] || die "AKAM_ROT2K_BUSYBOX=$AKAM_ROT2K_BUSYBOX is not a file"
    cp "$AKAM_ROT2K_BUSYBOX" "$bb"
    log "busybox: local file $AKAM_ROT2K_BUSYBOX"
else
    cache=$REPO/tools/.cache/rot2k/$BB_NAME
    if [ -f "$cache" ] && [ "$(sha_of "$cache")" = "$BB_SHA256" ]; then
        cp "$cache" "$bb"
        log "busybox: cached $cache"
    else
        command -v curl >/dev/null || die "curl is required to fetch busybox (or set AKAM_ROT2K_BUSYBOX to a local file)"
        log "busybox: downloading $BB_URL"
        curl -fsSL --retry 3 -o "$bb" "$BB_URL" || die "download failed (offline? set AKAM_ROT2K_BUSYBOX to a local file)"
    fi
fi
got=$(sha_of "$bb")
[ "$got" = "$BB_SHA256" ] || die "busybox sha256 mismatch: expected $BB_SHA256, got $got (nothing was written to $target)"
if [ -z "${AKAM_ROT2K_BUSYBOX:-}" ] && [ ! -f "${cache:-/nonexistent}" ] && mkdir -p "$(dirname "$cache")" 2>/dev/null; then
    cp "$bb" "$cache" 2>/dev/null || true
fi

# --- mode check: debug only ---------------------------------------------------------------------------------------
check_ini() {
    [ "$(cat "$1")" = "$(printf 'mode=debug\nstart_sh=%s' "$2")" ] || die "$1 is not the expected mode=debug file"
    ! grep -q 'mode=factory' "$1" || die "$1 selects factory mode"
}

install_file() { # install_file <src> <dest> <mode>
    mkdir -p "$(dirname "$2")"
    cp "$1" "$2"
    chmod "$3" "$2" 2>/dev/null || true
}

if [ "$rescue_only" = 1 ]; then
    src=$FWD/boot/rescue
    check_ini "$src/yy_debug.ini" /mnt/sdcard/hack.sh
    install_file "$src/yy_debug.ini" "$target/yy_debug.ini" 644
    install_file "$src/hack.sh" "$target/hack.sh" 755
    install_file "$src/telnetd" "$target/telnetd" 755
    install_file "$bb" "$target/busybox" 755
    sync
    log "rescue card written to $target (telnet root/root, no flash writes)"
    exit 0
fi

# --- overlay: Dropbear must be the shimmed build of the current sources ----------------------------------------------
dist=${AKAM_DROPBEAR_DIST:-$REPO/dist/dropbear}
hint="build it with: tools/build/bootstrap.sh && tools/dropbear/build_dropbear.sh"
[ -f "$dist/dropbearmulti" ] || die "$dist/dropbearmulti not found; $hint"
[ -f "$dist/BUILDINFO.txt" ] || die "$dist/BUILDINFO.txt not found; $hint"
bi() { sed -n "s/^$1=//p" "$dist/BUILDINFO.txt" | head -n1; }
[ "$(bi sha256_stripped)" = "$(sha_of "$dist/dropbearmulti")" ] || die "dropbearmulti does not match its BUILDINFO.txt; rebuild: $hint"
shim_now=$(sha_of "$REPO/src/core/time32_shim.c")
[ "$(bi time32_shim_sha256)" = "$shim_now" ] || die "dropbearmulti was not built with the current src/core/time32_shim.c (time64 clock bug on kernel 4.4); rebuild: $hint"
progs=" $(bi programs) "
case $progs in *" dropbear "*) ;; *) die "dropbearmulti lacks dropbear (programs=$(bi programs))" ;; esac
case $progs in *" dropbearkey "*) ;; *) die "dropbearmulti lacks dropbearkey (programs=$(bi programs))" ;; esac

src=$FWD/boot/card
check_ini "$src/yy_debug.ini" /mnt/sdcard/akam_boot.sh
install_file "$src/yy_debug.ini" "$target/yy_debug.ini" 644
install_file "$src/akam_boot.sh" "$target/akam_boot.sh" 755
install_file "$bb" "$target/busybox" 755
for f in "$src"/akam/*.sh; do
    install_file "$f" "$target/akam/$(basename "$f")" 755
done
install_file "$dist/dropbearmulti" "$target/akam/dropbearmulti" 755
[ ! -f "$dist/LICENSE.dropbear" ] || install_file "$dist/LICENSE.dropbear" "$target/akam/LICENSE.dropbear" 644
ptz_dist=${AKAM_PTZ_DIST:-$REPO/dist/akam_ptz}
if [ -f "$ptz_dist/akam_ptz" ]; then
    [ -f "$ptz_dist/BUILDINFO.txt" ] || die "$ptz_dist/BUILDINFO.txt not found; rebuild: firmware/lsc-rotatable-2k-ak3918av130/helpers/ptz/build_akam_ptz.sh"
    pbi() { sed -n "s/^$1=//p" "$ptz_dist/BUILDINFO.txt" | head -n1; }
    [ "$(pbi sha256_stripped)" = "$(sha_of "$ptz_dist/akam_ptz")" ] || die "akam_ptz does not match its BUILDINFO.txt; rebuild it"
    [ "$(pbi source_sha256)" = "$(cat "$FWD/helpers/ptz/akam_ptz.c" "$FWD/helpers/ptz/akam_ptz_safe.h" | sha256sum | cut -d' ' -f1)" ] || die "akam_ptz was not built from the current ptz/akam_ptz.c + akam_ptz_safe.h; rebuild it"
    install_file "$ptz_dist/akam_ptz" "$target/akam/akam_ptz" 755
    install_file "$ptz_dist/BUILDINFO.txt" "$target/akam/akam_ptz.BUILDINFO" 644
    log "akam_ptz installed (sha256 $(pbi sha256_stripped))"
else
    log "note: no akam_ptz in $ptz_dist: the card has no PTZ tool (build it with ptz/build_akam_ptz.sh)"
fi

# --- the own app (akam2k_rtsp): built with the Bootlin uClibc toolchain, proven by its BUILDINFO ------------------------
app_dist=${appbin:-${AKAM_OWN_APP_DIST:-$REPO/dist/rot2k-app}}
app_hint="rebuild with: make -C platforms/anyka-ak3918av130 poc3-dist CAMERA_LIBC=<libuClibc from your backup>"
if [ -n "$appbin" ] && [ ! -f "$app_dist/akam2k_rtsp" ]; then
    die "--own-app-bin: $app_dist/akam2k_rtsp not found; $app_hint"
fi
if [ -f "$app_dist/akam2k_rtsp" ]; then
    app=$app_dist/akam2k_rtsp
    [ -f "$app_dist/BUILDINFO.txt" ] || die "$app_dist/BUILDINFO.txt not found (poc3-symcheck proof missing); $app_hint"
    abi() { sed -n "s/^$1=//p" "$app_dist/BUILDINFO.txt" | head -n1; }
    [ "$(abi symcheck)" = passed ] || die "akam2k_rtsp: BUILDINFO.txt does not say symcheck=passed (poc3-symcheck against the camera libc was not run); $app_hint"
    [ "$(abi sha256)" = "$(sha_of "$app")" ] || die "akam2k_rtsp does not match its BUILDINFO.txt; $app_hint"
    [ "$(abi source_sha256)" = "$(sh "$HERE/source_hash.sh")" ] || die "akam2k_rtsp was not built from the current sources; $app_hint"
    # ELF check without any ARM tool: magic, 32-bit class, little endian, e_machine ARM (40), and the interpreter string.
    [ "$(head -c 4 "$app" | od -An -c | tr -d ' ')" = '177ELF' ] || die "akam2k_rtsp is not an ELF file"
    [ "$(od -An -tu1 -j4 -N2 "$app" | tr -s ' ' | sed 's/^ //')" = "1 1" ] || die "akam2k_rtsp is not a 32-bit little-endian ELF"
    [ "$(od -An -tu1 -j18 -N2 "$app" | tr -s ' ' | sed 's/^ //')" = "40 0" ] || die "akam2k_rtsp is not an ARM executable"
    head -c 4096 "$app" | grep -a -q '/lib/ld-uClibc\.so\.0' || die "akam2k_rtsp does not request the interpreter /lib/ld-uClibc.so.0 (built with another libc?); $app_hint"
    install_file "$app" "$target/akam/bin/akam2k_rtsp" 755
    install_file "$app_dist/BUILDINFO.txt" "$target/akam/bin/akam2k_rtsp.BUILDINFO" 644
    log "akam2k_rtsp installed (sha256 $(abi sha256))"
else
    log "note: no akam2k_rtsp in $app_dist: the own-app stub will run without video (build it with make poc3-dist in app/)"
fi
# --- time sync (opt-in on the card by the file akam/ntp, see own_app_stub.sh) ------------------------------------------
# The shared 1080p script, copied UNCHANGED (inert without the card file akam/ntp), plus akam2k_sntp, the one-shot SNTP
# client that replaces the BusyBox ntpd this camera does not have. The binary comes from the same dist directory as the
# app and is refused unless BUILDINFO.txt matches it and the CURRENT ../sntp sources. Optional: without it the stub just
# logs that time sync cannot start.
install_file "$REPO/src/scripts/akam_ntp.sh" "$target/akam/akam_ntp.sh" 755
install_file "$REPO/src/scripts/akam_hostname.sh" "$target/akam/akam_hostname.sh" 755
if [ -f "$app_dist/akam2k_sntp" ]; then
    [ -f "$app_dist/BUILDINFO.txt" ] || die "$app_dist/BUILDINFO.txt not found (sntp proof missing); $app_hint"
    abi() { sed -n "s/^$1=//p" "$app_dist/BUILDINFO.txt" | head -n1; }
    [ "$(abi symcheck)" = passed ] || die "akam2k_sntp: BUILDINFO.txt does not say symcheck=passed; $app_hint"
    [ "$(abi sntp_sha256)" = "$(sha_of "$app_dist/akam2k_sntp")" ] || die "akam2k_sntp does not match its BUILDINFO.txt (sntp_sha256); $app_hint"
    [ "$(abi sntp_source_sha256)" = "$(sh "$HERE/source_hash.sh" sntp)" ] || die "akam2k_sntp was not built from the current sntp sources; $app_hint"
    sntp=$app_dist/akam2k_sntp
    [ "$(head -c 4 "$sntp" | od -An -c | tr -d ' ')" = '177ELF' ] || die "akam2k_sntp is not an ELF file"
    [ "$(od -An -tu1 -j18 -N2 "$sntp" | tr -s ' ' | sed 's/^ //')" = "40 0" ] || die "akam2k_sntp is not an ARM executable"
    head -c 4096 "$sntp" | grep -a -q '/lib/ld-uClibc\.so\.0' || die "akam2k_sntp does not request the interpreter /lib/ld-uClibc.so.0"
    install_file "$sntp" "$target/akam/bin/akam2k_sntp" 755
    printf 'tool=akam2k_sntp\nsha256=%s\n' "$(abi sntp_sha256)" >"$target/akam/bin/akam2k_sntp.BUILDINFO"
    log "akam2k_sntp installed (sha256 $(abi sntp_sha256)); time sync stays off until the card has akam/ntp"
else
    log "note: no akam2k_sntp in $app_dist: no time sync on this card (build it with make poc3-dist in app/)"
fi
install_file "$keys" "$target/akam/authorized_keys" 600
[ "$f_decloud" = 0 ] || : >"$target/akam/decloud"
[ "$f_param" = 0 ] || : >"$target/akam/enable_param_conf"
[ "$f_notelnet" = 0 ] || : >"$target/akam/no_telnet"
[ "$f_ownapp" = 0 ] || : >"$target/akam/own_app"
# Wi-Fi credentials: only ever copied from the user's file, never created, read or printed here.
[ -z "$wpaconf" ] || install_file "$wpaconf" "$target/akam/wpa.conf" 600
sync
log "overlay card written to $target"
log "flags: decloud=$f_decloud enable_param_conf=$f_param no_telnet=$f_notelnet own_app=$f_ownapp"
[ "$f_ownapp" = 0 ] || [ -f "$target/akam/wpa.conf" ] || log "warning: own_app is set but the card has no akam/wpa.conf: the stub will fail its health check and reset the camera"
log "no flash is written by the card (except the opt-in param.conf edit); remove the card to return to stock"
