# Changelog

## Unreleased: project renamed from Camyn to Akam

- Wizard core `./akam` (bash, English, Arch and Debian/Ubuntu): step menu with done/todo/blocked status, `doctor`, `get-kit` (checksum + signature via `verify_kit.sh`), `build`, `prepare-backup-sd`, `backup`, `image` (hardware verdict, stock compression, typed `I UNDERSTAND THIS IS UNTESTED`), `prepare-flash-sd`, `wipe-sd`, `--dry-run`, resumable state with stale-input detection, secret-free log; camera steps are placeholders until the next wave.
- Host dependencies for Arch and Debian/Ubuntu: `check_host.sh` (per-distro install command, `--profile`, `--json`), `bootstrap.sh` (per-arch Zig pin incl. aarch64, `--print-plan`, `--offline`, retries on transient 5xx), `env.sh` exports everything that exists even without Zig, `check_time32.sh` picks an ARM-capable gdb (`gdb-multiarch` on Debian/Ubuntu).
- Debian 12 / Ubuntu 24.04 portability: POSIX awk only in `check_time32.sh` (no gawk `or()`), shellcheck 0.9 clean (SC2015), and `tools/ci/distro_check.sh` re-runs the whole build and local check in clean containers.
- microSD preparation on Linux: `tools/sd/sd_card.sh` (`list-candidates`, `prepare --mode backup|first-boot|flash-files`, read-only `verify`, `wipe`) with a `backend_linux.sh` (sysfs size, USB/SD-only guards, typed `ERASE <device>`, `--dry-run --stage-dir`); `prepare_first_boot_sd.sh` is unchanged. macOS is deferred.
- Added `tools/lib/host.sh` (Linux host helpers) and `tools/ci/local_check.sh` (local shellcheck + tests + tracked-file hygiene check).
- README rewritten for newcomers (wizard quick start, supported hardware, stream, recovery, security), new `docs/troubleshooting.md`, and `tests/readme_links_test.sh` (relative links, anchors and `./akam` commands).

The project was called Camyn during early development. Everything user-visible was renamed; the
flash layout, ports, the `ak_rtsp` binary name, the 8091 protocol and the persistent key files under
`/etc/config/dropbear/` are unchanged, so an existing camera keeps its host key and `authorized_keys`
after a reflash.

| Former name | New name |
|---|---|
| `CAMYN_*` environment variables and flags (`CAMYN_ROOT`, `CAMYN_FLASH_I_KNOW`, ...) | `AKAM_*` |
| `camyn_*.sh` scripts (`camyn_ssh.sh`, `camyn_lockdown.sh`, `camyn_hwcheck.sh`, `camyn_reflash_ssh.sh`) | `akam_*.sh` |
| `docs/camyn_v1_*.md`, `specs/*-camyn-v1.md` | `docs/akam_v1_*.md`, `specs/*-akam-v1.md` |
| SD card folder `camyn/` (with `authorized_keys`) | `akam/` |
| SD card flag file `CAMYN_MAINTENANCE` | `AKAM_MAINTENANCE` |
| On the camera: `/usr/local/camyn/`, `/tmp/camyn_*` | `/usr/local/akam/`, `/tmp/akam_*` |
| `LICENSE.camyn` in the image | `LICENSE.akam` |

### Release kit

- `tools/release/{make_kit,sign_kit,verify_kit}.sh`: build, sign (`ssh-keygen -Y`) and verify the
  prebuilt-binaries kit (`akam-kit-<version>.tar.xz`, Akam's own files only, never a firmware image);
  see `docs/akam_v1_kit.md`.
- The kit ships `supported_hardware.txt` (validated: LSC 3215672.3 / SI D26228 with its stock APP fingerprint; untested: 3215672.2 and anything else), hashed in `MANIFEST.txt`.

- `src/ak_rtsp/build_firmware.sh --kit DIR|TARBALL` assembles the image from a verified kit (no compiler); source and kit modes install the Akam files from the same `tools/release/image_files.txt`. The image follows the stock APP compression (xz or gzip, block size, BCJ) read from the dump, `--comp` overrides it with a loud warning, other compressors are refused. The dump is compared with `supported_hardware.txt`: anything but `validated` is refused without `--i-understand-untested`, another MTD layout always. Verdicts and fingerprints go to `build_report.txt`. `tools/image/assemble_from_kit.sh` wraps it for a backup directory. A kit directory (no signature to check) needs `--allow-unsigned` like an unsigned tarball; a release kit's time64 gate is replayed on the installed binaries when gdb is present; a kit carrying overlay files this script does not install is refused; the backup must hold `mtdblock0..7.bin` and a readable uImage kernel for a `validated` verdict.

### Deprecated aliases (removed in the next release)

An SD card prepared before the rename still works with the new image:

- `akam_ssh.sh` reads `camyn/authorized_keys` when `akam/authorized_keys` is absent and logs a
  `DEPRECATED` line in `/tmp/akam_ssh.log`.
- The file `CAMYN_MAINTENANCE` at the card root is accepted as `AKAM_MAINTENANCE` by the patched
  `service.sh` (SD hooks and the RAM marker), by `akam_lockdown.sh` (logs `DEPRECATED`) and by
  `akam_hwcheck.sh` (prints a `WARN`). Build without the alias with `AKAM_MAINT_FLAG_LEGACY=`.
- `flash_guard.sh` accepts `CAMYN_FLASH_I_KNOW=1` as `AKAM_FLASH_I_KNOW=1`, with a warning.
- `patch_service_sh.sh` recognises daemon lines already disabled with the former `CAMYN-DISABLED:` marker.

What to change on your side: rename the `camyn` folder and the `CAMYN_MAINTENANCE` file on your SD
card, use the new script and variable names, and (optionally) rename your local `~/.ssh/camyn` key
and the `~/camyn-backup` / `~/camyn-work` / `~/camyn-sdprep` directories that the docs now call
`~/.ssh/akam`, `~/akam-backup`, `~/akam-work` and `~/akam-sdprep` (`prepare_first_boot_sd.sh` looks for
the toolkit clone in `~/akam-sdprep/toolkit` by default: pass `--toolkit ~/camyn-sdprep/toolkit` to reuse an old clone).
